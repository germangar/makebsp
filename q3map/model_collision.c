/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "../common/cmdlib.h"
#include "../libs/coacd_api.h"
#include "qbsp.h"

#define WRITE_COLLISION_MAP qtrue

#define DIRECT_AXIAL_BRUSH_SIZE 32 // An enclosed trisoup with axial planes becomes a brush directly

#define MAX_FUNC_CLIPS 16       // Max number of func_static groups
#define MIN_FUNC_CLIP_DENSITY 0.000001 // Minimum density for a func_static group
#define MAX_CLIP_ENTITY_GROUPS MAX_MODEL_INSTANCES // Max number of clip entity groups
typedef struct {
  entity_t *entity;
  float brush_density;
  vec3_t mins, maxs;
  int numBrushes;
} clip_entity_group_t;

clip_entity_group_t clip_entity_groups[MAX_CLIP_ENTITY_GROUPS];
int num_clip_entity_groups = 0;

typedef enum {
  MC_NONE,
  MC_OBJECT,
  MC_WALKABLE,
  MC_FULL,
  MC_SHELL
} modelCategory_t;

const char *CategoryString(modelCategory_t cat) {
  switch (cat) {
  case MC_WALKABLE:
    return "WALKABLE";
  case MC_FULL:
    return "FULL";
  case MC_SHELL:
    return "SHELL";
  case MC_OBJECT:
    return "OBJECT";
  case MC_NONE:
  default:
    return "NONE";
  }
}

int c_degenerate_triangles = 0;
int c_degenerate_hulls = 0;


/*
==================
WriteCollisionMap
==================
*/
static void WriteCollisionMap(const char *name) {
  FILE *f;
  int i, j;
  side_t *s;
  winding_t *w;
  clip_entity_group_t *group;

  _printf("Writing diagnostic map: %s\n", name);
  f = fopen(name, "wb");
  if (!f) {
    Error("Can't write %s", name);
  }

  // Worldspawn (empty or placeholder)
  fprintf(f, "{\n\"classname\" \"worldspawn\"\n}\n");

  for (i = 0; i < num_clip_entity_groups; i++) {
    group = &clip_entity_groups[i];
    if (!group->entity || !group->entity->brushes) {
      continue;
    }

    fprintf(f, "{\n");
    // Write entity epairs
    for (epair_t *ep = group->entity->epairs; ep; ep = ep->next) {
      if (ep->key && ep->value) {
        fprintf(f, "\"%s\" \"%s\"\n", ep->key, ep->value);
      }
    }

    // Write entity brushes
    for (bspbrush_t *b = group->entity->brushes; b; b = b->next) {
      fprintf(f, "{\n");
      for (j = 0; j < b->numsides; j++) {
        s = &b->sides[j];
        w = s->winding;
        if (!w || w->numpoints < 3) {
          continue;
        }

        // Write points in CW order (0, 2, 1) to define plane pointing OUT
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[0][0], w->p[0][1], w->p[0][2]);
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[2][0], w->p[2][1], w->p[2][2]);
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[1][0], w->p[1][1], w->p[1][2]);

        const char *shader = "textures/common/caulk";
        if (s->shaderInfo) {
          shader = s->shaderInfo->shader;
        }
        if (!Q_strncasecmp(shader, "textures/", 9)) {
          shader += 9;
        }
        fprintf(f, "%s 0 0 0 1 1\n", shader);
      }
      fprintf(f, "}\n");
    }
    fprintf(f, "}\n");
  }

  fclose(f);
}

/*
==================
CompareDensity

QSort comparison function
==================
*/
static int CompareDensity(const void *a, const void *b) {
  const clip_entity_group_t *ga = (const clip_entity_group_t *)a;
  const clip_entity_group_t *gb = (const clip_entity_group_t *)b;

  if (ga->brush_density > gb->brush_density)
    return -1;
  if (ga->brush_density < gb->brush_density)
    return 1;
  return 0;
}

/*
==================
PrepareClipEntityGroups

Sorts groups by density, merges overflow into a func_group,
and assigns model numbers to the top ones.
==================
*/
static void PrepareClipEntityGroups(void) {
  int i;
  int models;
  char value[32];

  if (num_clip_entity_groups <= 0) {
    return;
  }

  _printf("----- PrepareClipEntityGroups -----\n");

  // Step 1: Figure out the next available model number
  // Submodels start at *1 and go up for each map entity with brushes.
  models = 1;
  for (i = 1; i < num_entities; i++) {
    if (entities[i].brushes || entities[i].patches) {
      models++;
    }
  }
  _printf("Existing map models: %i. Starting clip models at *%i\n", models - 1,
          models);

  // Step 2: Sort by density (descending)
  qsort(clip_entity_groups, num_clip_entity_groups, sizeof(clip_entity_group_t),
        CompareDensity);

  // Step 3: Identify the split point for overflow or low density
  int splitPoint = num_clip_entity_groups;

  // First check: density-based filtering
  for (i = 0; i < num_clip_entity_groups; i++) {
    if (clip_entity_groups[i].brush_density < MIN_FUNC_CLIP_DENSITY) {
      splitPoint = i;
      break;
    }
  }

  // Second check: limit to MAX_FUNC_CLIPS
  if (splitPoint > MAX_FUNC_CLIPS) {
    splitPoint = MAX_FUNC_CLIPS;
  }

  // Step 4: Merge overflow/low-density groups into a single func_group
  if (splitPoint < num_clip_entity_groups) {
    _printf("Merging %i groups (overflow or low density) into a single func_group\n",
            num_clip_entity_groups - splitPoint);

    clip_entity_group_t *merged = &clip_entity_groups[splitPoint];
    entity_t *groupEnt = malloc(sizeof(entity_t));
    memset(groupEnt, 0, sizeof(entity_t));
    SetKeyValue(groupEnt, "classname", "func_group");

    ClearBounds(merged->mins, merged->maxs);
    merged->numBrushes = 0;

    for (i = splitPoint; i < num_clip_entity_groups; i++) {
      clip_entity_group_t *g = &clip_entity_groups[i];
      if (g->entity && g->entity->brushes) {
        bspbrush_t *b = g->entity->brushes;
        while (b->next) {
          b = b->next;
        }
        b->next = groupEnt->brushes;
        groupEnt->brushes = g->entity->brushes;

        AddPointToBounds(g->mins, merged->mins, merged->maxs);
        AddPointToBounds(g->maxs, merged->mins, merged->maxs);
        merged->numBrushes += g->numBrushes;

        // Cleanup the orphaned local entity (epairs and brushes are handled)
        g->entity->brushes = NULL; // Brushes moved, don't free them
        // Free epairs
        epair_t *next_ep;
        for (epair_t *curr_ep = g->entity->epairs; curr_ep; curr_ep = next_ep) {
          next_ep = curr_ep->next;
          free(curr_ep->key);
          free(curr_ep->value);
          free(curr_ep);
        }
        free(g->entity);
      }
    }

    merged->entity = groupEnt;
    num_clip_entity_groups = splitPoint + 1;
  }

  // Step 4: Assign model numbers to the remaining func_static entities
  for (i = 0; i < num_clip_entity_groups; i++) {
    clip_entity_group_t *g = &clip_entity_groups[i];
    const char *cls = ValueForKey(g->entity, "classname");

    if (!Q_stricmp(cls, "func_static")) {
      sprintf(value, "*%i", models++);
      SetKeyValue(g->entity, "model", value);
    }

    // Add metadata
    if (!Q_stricmp(cls, "func_static")) {
      sprintf(value, "%.8f", g->brush_density);
      SetKeyValue(g->entity, "density", value);
    }

    if (!Q_stricmp(cls, "func_static") || !Q_stricmp(cls, "func_group")) {
      sprintf(value, "%i", g->numBrushes);
      SetKeyValue(g->entity, "brushcount", value);
    }
  }
}

/*
====================
BrushFromMesh

Converts a convex hull (triangle soup) into a bspbrush_t.
Deduplicates coplanar faces by finding unique plane indices.
====================
*/
bspbrush_t *BrushFromMesh(CoACD_Mesh *mesh, shaderInfo_t *si) {
  int i, j;
  int numUniquePlanes = 0;
  int uniquePlanes[MAX_BRUSH_SIDES];
  float maxPlaneArea[MAX_BRUSH_SIDES];
  vec3_t trianglePoints[MAX_BRUSH_SIDES][3];
  bspbrush_t *b;

  for (i = 0; i < MAX_BRUSH_SIDES; i++) {
    maxPlaneArea[i] = -1.0f;
  }

  // For each triangle, find its unique plane
  for (i = 0; i < mesh->triangles_count; i++) {
    int idx0 = mesh->triangles_ptr[i * 3 + 0];
    int idx1 = mesh->triangles_ptr[i * 3 + 1];
    int idx2 = mesh->triangles_ptr[i * 3 + 2];

    vec3_t p0, p1, p2;
    p0[0] = mesh->vertices_ptr[idx0 * 3 + 0];
    p0[1] = mesh->vertices_ptr[idx0 * 3 + 1];
    p0[2] = mesh->vertices_ptr[idx0 * 3 + 2];
    p1[0] = mesh->vertices_ptr[idx1 * 3 + 0];
    p1[1] = mesh->vertices_ptr[idx1 * 3 + 1];
    p1[2] = mesh->vertices_ptr[idx1 * 3 + 2];
    p2[0] = mesh->vertices_ptr[idx2 * 3 + 0];
    p2[1] = mesh->vertices_ptr[idx2 * 3 + 1];
    p2[2] = mesh->vertices_ptr[idx2 * 3 + 2];

    // Check for degenerate triangle (near zero area)
    vec3_t t1, t2, cross;
    VectorSubtract(p1, p0, t1);
    VectorSubtract(p2, p0, t2);
    CrossProduct(t1, t2, cross);
    float area = VectorLength(cross);
    if (area < 0.1) {
      c_degenerate_triangles++;
      continue;
    }

    // MapPlaneFromPoints computes normal pointing OUT for CW triangles.
    // If our mesh is CCW (standard), feeding MapPlaneFromPoints(p0, p2, p1)
    // gives the correct outgoing normal.
    int planenum = MapPlaneFromPoints(p0, p2, p1);
    if (planenum == -1) {
      c_degenerate_triangles++;
      continue;
    }

    // Deduplicate against existing planes in this brush
    for (j = 0; j < numUniquePlanes; j++) {
      if (uniquePlanes[j] == planenum) {
        // If this triangle is significantly larger, use it for the .map points
        if (area > maxPlaneArea[j]) {
          maxPlaneArea[j] = area;
          VectorCopy(p0, trianglePoints[j][0]);
          VectorCopy(p1, trianglePoints[j][1]);
          VectorCopy(p2, trianglePoints[j][2]);
        }
        break;
      }
    }

    if (j == numUniquePlanes) {
      if (numUniquePlanes >= MAX_BRUSH_SIDES) {
        _printf("WARNING: BrushFromMesh reached MAX_BRUSH_SIDES\n");
        break;
      }
      uniquePlanes[numUniquePlanes] = planenum;
      maxPlaneArea[numUniquePlanes] = area;
      // Capture CCW triangle points (p0, p1, p2)
      VectorCopy(p0, trianglePoints[numUniquePlanes][0]);
      VectorCopy(p1, trianglePoints[numUniquePlanes][1]);
      VectorCopy(p2, trianglePoints[numUniquePlanes][2]);
      numUniquePlanes++;
    }
  }

  if (numUniquePlanes < 4) {
    return NULL;
  }

  b = AllocBrush(numUniquePlanes);
  b->numsides = numUniquePlanes;
  b->detail = qtrue;
  b->contents = si->contents;
  b->contentShader = si;

  for (i = 0; i < numUniquePlanes; i++) {
    b->sides[i].planenum = uniquePlanes[i];
    b->sides[i].shaderInfo = si;

    // Create a 3-point winding for WriteBspBrushMap.
    // Storing them as original CCW (p0, p1, p2).
    // WriteBspBrushMap will flip them to CW (0, 2, 1).
    b->sides[i].winding = AllocWinding(3);
    b->sides[i].winding->numpoints = 3;
    VectorCopy(trianglePoints[i][0], b->sides[i].winding->p[0]);
    VectorCopy(trianglePoints[i][1], b->sides[i].winding->p[1]);
    VectorCopy(trianglePoints[i][2], b->sides[i].winding->p[2]);
  }

  // Calculate bounds from the 3-point windings
  if (!BoundBrush(b)) {
    c_degenerate_hulls++;
    FreeBrush(b);
    return NULL;
  }

  return b;
}

/*
====================
CategorizeModel

Reserved for future entity-specific classification for CoACD.
====================
*/
static modelCategory_t CategorizeModel(modelInstance_t *inst) {
  int j, k;
  mapDrawSurface_t *ds;
  vec3_t mins, maxs, centroid;
  float totalArea = 0;
  float groundArea = 0;
  float inwardArea = 0;
  float outwardArea = 0;
  int totalVerts = 0;

  ClearBounds(mins, maxs);
  VectorClear(centroid);

  // Pass 1: AABB and Centroid
  for (j = 0; j < inst->numDrawSurfs; j++) {
    ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
      continue;
    }
    for (k = 0; k < ds->numVerts; k++) {
      AddPointToBounds(ds->verts[k].xyz, mins, maxs);
      VectorAdd(centroid, ds->verts[k].xyz, centroid);
      totalVerts++;
    }
  }

  if (totalVerts == 0) {
    return MC_NONE;
  }

  VectorScale(centroid, 1.0f / totalVerts, centroid);

  // Pass 2: Orientation and Ground heuristics
  for (j = 0; j < inst->numDrawSurfs; j++) {
    ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
      continue;
    }

    for (k = 0; k < ds->numIndexes; k += 3) {
      vec3_t v[3], edge1, edge2, normal, center;
      VectorCopy(ds->verts[ds->indexes[k + 0]].xyz, v[0]);
      VectorCopy(ds->verts[ds->indexes[k + 1]].xyz, v[1]);
      VectorCopy(ds->verts[ds->indexes[k + 2]].xyz, v[2]);

      VectorSubtract(v[2], v[0], edge1);
      VectorSubtract(v[1], v[0], edge2);
      CrossProduct(edge1, edge2, normal);
      float area = VectorLength(normal) * 0.5f;
      if (area < 0.001f)
        continue;

      VectorNormalize(normal, normal);

      // Ground detect
      if (normal[2] > 0.7f) {
        groundArea += area;
      }

      // Orientation (Centroid Dot Product)
      VectorAdd(v[0], v[1], center);
      VectorAdd(center, v[2], center);
      VectorScale(center, 1.0f / 3.0f, center);

      vec3_t fromCentroid;
      VectorSubtract(center, centroid, fromCentroid);
      if (DotProduct(normal, fromCentroid) > 0) {
        outwardArea += area;
      } else {
        inwardArea += area;
      }

      totalArea += area;
    }
  }

  if (totalArea < 1.0f) {
    return MC_NONE;
  }

  float groundRatio = groundArea / totalArea;
  float outwardRatio = outwardArea / totalArea;
  float inwardRatio = inwardArea / totalArea;
  float height = maxs[2] - mins[2];
  qboolean isFlat = height < 8.0f;

  modelCategory_t category = MC_OBJECT;

  if (groundRatio > 0.3f || (isFlat && groundRatio > 0.15f)) {
    category = MC_WALKABLE;
  } else if (outwardRatio > 0.8f) {
    category = MC_FULL;
  } else if (inwardRatio > 0.8f) {
    category = MC_SHELL;
  }

  _printf("Instance %s: Categorized as %s\n", inst->modelName,
          CategoryString(category));
  _printf("  Metrics: Area %.1f, Ground %.1f%%, Outward %.1f%%, Inward %.1f%%, "
          "Height %.1f\n",
          totalArea, groundRatio * 100.0f, outwardRatio * 100.0f,
          inwardRatio * 100.0f, height);

  return category;
}

/*
====================
DecomposeModelCollision

Aggregates solid geometry for one instance, runs CoACD, and converts results to brushes.
====================
*/
static void DecomposeModelCollision(modelInstance_t *inst, modelCategory_t category) {
  int j, k;
  mapDrawSurface_t *ds;
  int totalVerts = 0;
  int totalIndexes = 0;
  double *allVerts;
  int *allIndexes;
  int currentVert = 0;
  int currentIndex = 0;
  bspbrush_t *hulls_list = NULL;
  int numHulls = 0;

  if (num_clip_entity_groups >= MAX_CLIP_ENTITY_GROUPS) {
    _printf("WARNING: MAX_CLIP_ENTITY_GROUPS reached\n");
    return;
  }

  float threshold;
  switch (category) {
  case MC_WALKABLE:
    threshold = 0.1f;
    break;
  case MC_FULL:
    threshold = 0.2f;
    break;
  case MC_SHELL:
    threshold = 0.2f;
    break;
  case MC_OBJECT:
  default:
    threshold = 0.05f;
    break;
  case MC_NONE:
    return;
  }

  // Reset per-model degenerate counters
  c_degenerate_triangles = 0;
  c_degenerate_hulls = 0;

  // Step 1: Pre-calculate thresholds and print info
  _printf("Instance %s: Decomposing as %s (threshold %.2f)\n", 
          inst->modelName, CategoryString(category), threshold);

  shaderInfo_t *caulk = ShaderInfoForShader("textures/common/caulk");

  if (category == MC_FULL) {
    // ALL model vertexes callculated at once (meshes merged)
    for (j = 0; j < inst->numDrawSurfs; j++) {
      ds = inst->drawSurfs[j];
      if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
        continue;
      }
      totalVerts += ds->numVerts;
      totalIndexes += ds->numIndexes;
    }

    if (totalVerts == 0) return;

    allVerts = malloc(totalVerts * 3 * sizeof(double));
    allIndexes = malloc(totalIndexes * sizeof(int));

    for (j = 0; j < inst->numDrawSurfs; j++) {
      ds = inst->drawSurfs[j];
      if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
        continue;
      }

      int startVert = currentVert;
      for (k = 0; k < ds->numVerts; k++) {
        allVerts[currentVert * 3 + 0] = ds->verts[k].xyz[0];
        allVerts[currentVert * 3 + 1] = ds->verts[k].xyz[1];
        allVerts[currentVert * 3 + 2] = ds->verts[k].xyz[2];
        currentVert++;
      }

      for (k = 0; k < ds->numIndexes; k++) {
        allIndexes[currentIndex++] = startVert + ds->indexes[k];
      }
    }

    CoACD_Mesh input;
    input.vertices_ptr = allVerts;
    input.vertices_count = totalVerts;
    input.triangles_ptr = allIndexes;
    input.triangles_count = totalIndexes / 3;

    CoACD_MeshArray hulls = CoACD_run(&input, threshold, -1, COACD_PREPROCESS_AUTO, 50, 2000, 20, 100, 3, false, true, true, (int)(totalVerts * 0.9), false, 0.01, COACD_APX_CH, 1234);

    for (j = 0; j < (int)hulls.meshes_count; j++) {
      bspbrush_t *b = BrushFromMesh(&hulls.meshes_ptr[j], caulk);
      if (b) {
        b->next = hulls_list;
        hulls_list = b;
        numHulls++;
      }
    }
    
    free(allVerts);
    free(allIndexes);
    // hulls.meshes_ptr is managed by CoACD, but in our current glue we don't have an explicit free for it yet
    // unless we added it to CoACD_MeshArray structure.
  } else {
    // Calculate Collision Per Mesh
    for (j = 0; j < inst->numDrawSurfs; j++) {
      ds = inst->drawSurfs[j];
      if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
        continue;
      }

      if (ds->numVerts == 0 || ds->numIndexes == 0) {
        continue;
      }

      double *meshVerts = malloc(ds->numVerts * 3 * sizeof(double));
      int *meshIndexes = malloc(ds->numIndexes * sizeof(int));

      for (k = 0; k < ds->numVerts; k++) {
        meshVerts[k * 3 + 0] = ds->verts[k].xyz[0];
        meshVerts[k * 3 + 1] = ds->verts[k].xyz[1];
        meshVerts[k * 3 + 2] = ds->verts[k].xyz[2];
      }
      for (k = 0; k < ds->numIndexes; k++) {
        meshIndexes[k] = ds->indexes[k];
      }

      CoACD_Mesh input;
      input.vertices_ptr = meshVerts;
      input.vertices_count = ds->numVerts;
      input.triangles_ptr = meshIndexes;
      input.triangles_count = ds->numIndexes / 3;

      CoACD_MeshArray hulls = CoACD_run(&input, threshold, -1, COACD_PREPROCESS_AUTO, 50, 2000, 20, 100, 3, false, true, true, (int)(ds->numVerts * 0.9), false, 0.01, COACD_APX_CH, 1234);

      for (k = 0; k < (int)hulls.meshes_count; k++) {
        bspbrush_t *b = BrushFromMesh(&hulls.meshes_ptr[k], caulk);
        if (b) {
          b->next = hulls_list;
          hulls_list = b;
          numHulls++;
        }
      }

      free(meshVerts);
      free(meshIndexes);
    }
  }

  _printf("Instance %s: Generated total %i convex hulls.\n", inst->modelName, numHulls);

  // Step 5: Populate clip entity group
  if (hulls_list) {
    clip_entity_group_t *group = &clip_entity_groups[num_clip_entity_groups++];

    // Create a local entity (not part of the map entities yet)
    entity_t *ent = malloc(sizeof(entity_t));
    memset(ent, 0, sizeof(entity_t));
    SetKeyValue(ent, "classname", "func_static");
    ent->brushes = hulls_list;

    group->entity = ent;
    group->numBrushes = numHulls;

    // Calculate bounds
    ClearBounds(group->mins, group->maxs);
    for (bspbrush_t *b = hulls_list; b; b = b->next) {
      AddPointToBounds(b->mins, group->mins, group->maxs);
      AddPointToBounds(b->maxs, group->mins, group->maxs);
    }

    // Calculate brush density
    vec3_t size;
    VectorSubtract(group->maxs, group->mins, size);
    float volume = size[0] * size[1] * size[2];
    if (volume > 1.0f) {
      group->brush_density = (float)numHulls / volume;
    } else {
      group->brush_density = 0;
    }

    _printf("Instance %s: Created clip group with %i brushes, density %e\n",
            inst->modelName, numHulls, group->brush_density);
  }

  // Step 6: Per-model reporting
  if (c_degenerate_triangles > 0 || c_degenerate_hulls > 0) {
    _printf("Instance %s: Degenerate geometry skipped: %i triangles, %i hulls\n",
            inst->modelName, c_degenerate_triangles, c_degenerate_hulls);
  }
}

/*
====================
CreateTriangleModelCollision

Generates collision brushes from model geometry using CoACD (per-instance pass).
====================
*/
void CreateTriangleModelCollision(void) {
  int i;
  modelInstance_t *inst;
  qboolean hasSolid = qfalse;

  _printf("----- CreateTriangleModelCollision -----\n");

  // Step 1: Quick check for any solid geometry (optimization)
  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];
    for (int j = 0; j < inst->numDrawSurfs; j++) {
      if (inst->drawSurfs[j]->shaderInfo &&
          (inst->drawSurfs[j]->shaderInfo->contents & CONTENTS_SOLID)) {
        hasSolid = qtrue;
        break;
      }
    }
    if (hasSolid)
      break;
  }

  if (!hasSolid) {
    _printf("No solid model geometry found for collision.\n");
    return;
  }

  // Reset groups
  num_clip_entity_groups = 0;

  // Step 2: Decomposition and Categorization Pass (per instance)
  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];
    modelCategory_t category = CategorizeModel(inst);
    if (category != MC_NONE) {
      DecomposeModelCollision(inst, category);
    }
  }

  // Step 3: Preparation pass
  PrepareClipEntityGroups();

  if (WRITE_COLLISION_MAP) {
    char debugName[1024];
    sprintf(debugName, "%s_collision.map", source);
    WriteCollisionMap(debugName);
  }
}
