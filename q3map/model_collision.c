/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "../common/cmdlib.h"
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

        // Derive 3 export points directly from the plane equation.
        // This ensures all faces are written even if CreateBrushWindings
        // produced a NULL winding (e.g. due to plane snapping), and gives
        // maximally precise plane definitions for the editor.
        w = BaseWindingForPlane(mapplanes[s->planenum].normal,
                                mapplanes[s->planenum].dist);

        // BaseWindingForPlane produces CW from normal direction.
        // Writing (0,1,2) gives correct outward normal via MapPlaneFromPoints.
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[0][0], w->p[0][1], w->p[0][2]);
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[1][0], w->p[1][1], w->p[1][2]);
        fprintf(f, "( %.3f %.3f %.3f ) ", w->p[2][0], w->p[2][1], w->p[2][2]);

        FreeWinding(w);

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
  int totalTriangles = 0;

  ClearBounds(mins, maxs);
  VectorClear(centroid);

  // Pass 1: AABB, Centroid and Vert/Tri counts
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
    totalTriangles += (ds->numIndexes / 3);
  }

  if (totalVerts == 0) {
    inst->category = MC_NONE;
    inst->triangle_density = 0.0f;
    return MC_NONE;
  }

  // Calculate volume and normalized density per 128-unit cube
  vec3_t size;
  VectorSubtract(maxs, mins, size);
  float volume = size[0] * size[1] * size[2];
  
  if (volume > 1.0f) {
    inst->triangle_density = ((float)totalTriangles / volume) * 2097152.0f; 
  } else {
    inst->triangle_density = 0.0f; 
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
  
  inst->category = category;

  _printf("Instance %s: Categorized as %s\n", inst->modelName,
          CategoryString(category));
  _printf("  Metrics: Area %.1f, Ground %.1f%%, Outward %.1f%%, Inward %.1f%%, "
          "Height %.1f, Density: %.1f tris/128u^3\n",
          totalArea, groundRatio * 100.0f, outwardRatio * 100.0f,
          inwardRatio * 100.0f, height, inst->triangle_density);

  return category;
}

/*
====================
DecomposeModelCollision


====================
*/
static void DecomposeModelCollision(modelInstance_t *inst, modelCategory_t category) {
  bspbrush_t *hulls_list = NULL;
  int numHulls = 0;

  if (num_clip_entity_groups >= MAX_CLIP_ENTITY_GROUPS) {
    _printf("WARNING: MAX_CLIP_ENTITY_GROUPS reached\n");
    return;
  }

  // Step 1: Pre-calculate thresholds and print info
  _printf("Instance %s: Decomposing as %s\n", inst->modelName, CategoryString(category));

  shaderInfo_t *caulk = ShaderInfoForShader("textures/common/caulk");
  qboolean mergeMeshes = (category == MC_FULL) ? qtrue : qfalse;

  hulls_list = GenerateCoACDCollision(inst, category, mergeMeshes, caulk);

  for (bspbrush_t *b = hulls_list; b; b = b->next) {
    numHulls++;
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
