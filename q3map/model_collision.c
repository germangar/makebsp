/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "../common/cmdlib.h"
#include "qbsp.h"
#include "model_collision.h"

/* Mesh Lib Lite*/
#include "../libs/MeshLib-Lite/MRMeshC/MRMesh.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRMeshBuilder.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRMeshFixer.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRMeshFillHole.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRMeshDecimate.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRString.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRBitSet.h"

#define WRITE_COLLISION_MAP qtrue

#define DIRECT_AXIAL_BRUSH_SIZE 32 // An enclosed trisoup with axial planes becomes a brush directly

#define MAX_FUNC_CLIPS 0       // Max number of func_static groups
#define DENSITY_UNIT_SIZE 128.0f // Standard cube dimension for density calculation
#define DENSITY_STANDARD_VOLUME (DENSITY_UNIT_SIZE * DENSITY_UNIT_SIZE * DENSITY_UNIT_SIZE)
#define MIN_FUNC_CLIP_DENSITY 100.0f // Minimum standardized density for a func_static group
#define MIN_FUNC_CLIP_BRUSHCOUNT 50 // Minimum brush count for a func_static group
#define MAX_CLIP_ENTITY_GROUPS MAX_MODEL_INSTANCES // Max number of clip entity groups
typedef struct {
  entity_t *entity;
  float brush_density;
  vec3_t mins, maxs;
  int numBrushes;
} clip_entity_group_t;

clip_entity_group_t clip_entity_groups[MAX_CLIP_ENTITY_GROUPS];
int num_clip_entity_groups = 0;

static int c_degenerate_triangles = 0;
static int c_degenerate_hulls = 0;

static MRMesh *HealAndDecimateMesh(float *verts, int numVerts,
                                    int *indexes, int numIndexes,
                                    const char *debugName);

const char *CategoryString(modelCategory_t cat) {
  switch (cat) {
  case MC_OBJECT:
    return "MC_OBJECT";
  case MC_WALKABLE:
    return "MC_WALKABLE";
  case MC_WRAP:
    return "MC_WRAP";
  case MC_SHELL:
    return "MC_SHELL";
  case MC_TERRAIN:
    return "MC_TERRAIN";
  default:
    return "MC_NONE";
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
        fprintf(f, "common/caulk 0 0 0 1 1\n");
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

  // Step 3: Partition groups by criteria (Density AND Brush Count)
  // Move eligible groups to the front of the array.
  int eligibleCount = 0;
  for (i = 0; i < num_clip_entity_groups; i++) {
    if (clip_entity_groups[i].brush_density >= MIN_FUNC_CLIP_DENSITY &&
        clip_entity_groups[i].numBrushes >= MIN_FUNC_CLIP_BRUSHCOUNT) {
      if (i != eligibleCount) {
        clip_entity_group_t temp = clip_entity_groups[eligibleCount];
        clip_entity_groups[eligibleCount] = clip_entity_groups[i];
        clip_entity_groups[i] = temp;
      }
      eligibleCount++;
    }
  }

  // Step 4: Identify the split point (top candidates, capped by count)
  int splitPoint = eligibleCount;
  if (splitPoint > MAX_FUNC_CLIPS) {
    splitPoint = MAX_FUNC_CLIPS;
  }

  // Step 5: Merge overflow/ineligible groups into a single func_group
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
static void CategorizeModel(modelInstance_t *inst) {
  int j, k;
  mapDrawSurface_t *ds;
  vec3_t mins, maxs, centroid;
  float upArea = 0;
  float upperHemisphereArea = 0;
  float totalArea = 0;
  float inwardArea = 0;
  float outwardArea = 0;
  int totalVerts = 0;

  ClearBounds(mins, maxs);
  VectorClear(centroid);

  // Pass 1: AABB, Centroid and Vert counts
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
    inst->category = MC_NONE;
    inst->triangle_density = 0.0f;
    return;
  }

  // Count decimated collision triangles for density
  int totalColTriangles = 0;
  for (j = 0; j < inst->num_collision_meshes; j++) {
    totalColTriangles += inst->collision_meshes[j]->numTris;
  }

  // Calculate volume and normalized density per 128-unit cube
  vec3_t size;
  VectorSubtract(maxs, mins, size);
  float volume = size[0] * size[1] * size[2];
  
  if (volume > 1.0f) {
    inst->triangle_density = ((float)totalColTriangles / volume) * DENSITY_STANDARD_VOLUME; 
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

      // Terrain and Ground detect
      if (normal[2] > 0.707f) { // Within 45 degrees of Up (0,0,1)
        upArea += area;
      }
      if (normal[2] > 0.0f) { // Upper hemisphere
        upperHemisphereArea += area;
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
    inst->category = MC_NONE;
    return;
  }

  float upRatio = upArea / totalArea;
  float upperHemisphereRatio = upperHemisphereArea / totalArea;
  float outwardRatio = outwardArea / totalArea;
  float inwardRatio = inwardArea / totalArea;
  float height = maxs[2] - mins[2];
  qboolean isFlat = height < 8.0f;

  modelCategory_t category = MC_OBJECT;

  if (upRatio > 0.5f && upperHemisphereRatio > 0.85f) {
    category = MC_TERRAIN;
  } else if (upRatio > 0.3f || (isFlat && upRatio > 0.15f)) {
    category = MC_WALKABLE;
  } else if (outwardRatio > 0.8f) {
    category = MC_WRAP;
  } else if (inwardRatio > 0.8f) {
    category = MC_SHELL;
  }
  
  inst->category = category;

  _printf("Instance %s: Categorized as %s\n", inst->modelName,
          CategoryString(category));
  _printf("  Metrics: Area %.1f, Up %.1f%%, UpperHemi %.1f%%, Outward %.1f%%, Inward %.1f%%, "
          "Height %.1f, Density: %.1f tris/128u^3\n",
          totalArea, upRatio * 100.0f, upperHemisphereRatio * 100.0f, outwardRatio * 100.0f,
          inwardRatio * 100.0f, height, inst->triangle_density);


  return;
}

/*
====================
GenerateCollisionTerrainExtrusion

Specialized pipeline for terrains:
1. Merges all draw surfaces into one mesh.
2. Decimates the result using MeshLib-Lite.
3. Extrudes the resulting decimated mesh into brushes.
====================
*/
bspbrush_t *GenerateCollisionTerrainExtrusion(modelInstance_t *inst, shaderInfo_t *shader) {
  int j, k;
  int totalVerts = 0;
  int totalIndexes = 0;
  float *allVerts;
  int *allIndexes;
  int currentVert = 0;
  int currentIndex = 0;
  
  _printf("Instance %s: Generating Terrain Extrusion\n", inst->modelName);

  for (j = 0; j < inst->numDrawSurfs; j++) {
    mapDrawSurface_t *ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) continue;
    totalVerts += ds->numVerts;
    totalIndexes += ds->numIndexes;
  }

  if (totalVerts == 0) return NULL;

  allVerts = malloc(totalVerts * 3 * sizeof(float));
  allIndexes = malloc(totalIndexes * sizeof(int));

  for (j = 0; j < inst->numDrawSurfs; j++) {
    mapDrawSurface_t *ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) continue;
    int baseVert = currentVert;
    for (k = 0; k < ds->numVerts; k++) {
      allVerts[currentVert * 3 + 0] = (float)ds->verts[k].xyz[0];
      allVerts[currentVert * 3 + 1] = (float)ds->verts[k].xyz[1];
      allVerts[currentVert * 3 + 2] = (float)ds->verts[k].xyz[2];
      currentVert++;
    }
    for (k = 0; k < ds->numIndexes; k++) {
      allIndexes[currentIndex++] = ds->indexes[k] + baseVert;
    }
  }

  MRMesh *healed = HealAndDecimateMesh(allVerts, totalVerts, allIndexes, totalIndexes, inst->modelName);
  free(allVerts);
  free(allIndexes);

  if (!healed) return NULL;

  // Convert MRMesh to colMesh_t
  colMesh_t *colMesh = malloc(sizeof(colMesh_t));
  memset(colMesh, 0, sizeof(colMesh_t));
  colMesh->shaderInfo = shader; // original material
  
  const MRVector3f *pts = mrMeshPoints(healed);
  size_t numPts = mrMeshPointsNum(healed);
  const MRMeshTopology *topo = mrMeshTopology(healed);
  MRTriangulation *tri = mrMeshTopologyGetTriangulation(topo);

  if (tri && tri->size > 0) {
      colMesh->numVerts = (int)numPts;
      colMesh->verts = malloc(colMesh->numVerts * sizeof(vec3_t));
      memcpy(colMesh->verts, pts, colMesh->numVerts * sizeof(vec3_t));

      int validTriCount = 0;
      for (size_t fi = 0; fi < tri->size; fi++) {
          if (tri->data[fi][0].id >= 0) validTriCount++;
      }

      colMesh->numTris = validTriCount;
      colMesh->tris = malloc(colMesh->numTris * sizeof(colTri_t));
      
      int triIdx = 0;
      for (size_t fi = 0; fi < tri->size; fi++) {
          if (tri->data[fi][0].id >= 0) {
              colMesh->tris[triIdx][0] = tri->data[fi][0].id;
              colMesh->tris[triIdx][1] = tri->data[fi][1].id;
              colMesh->tris[triIdx][2] = tri->data[fi][2].id;
              triIdx++;
          }
      }
  }
  mrMeshFree(healed);

  bspbrush_t *hulls = NULL;
  if (colMesh->numTris > 0) {
      hulls = ExtrudeTrianglesToBrushes(colMesh, shader);
  }
  FreeCollisionMesh(colMesh);
  return hulls;
}


/*
====================
FreeCollisionMesh
====================
*/
void FreeCollisionMesh(colMesh_t *mesh) {
  if (!mesh) return;
  if (mesh->verts) free(mesh->verts);
  if (mesh->tris) free(mesh->tris);
  free(mesh);
}

/*
====================
FreeCollisionHull
====================
*/
void FreeCollisionHull(colHull_t *hull) {
  if (!hull) return;
  if (hull->verts) free(hull->verts);
  if (hull->tris) free(hull->tris);
  free(hull);
}

/*
====================
BrushFromHull

Converts a convex hull (triangle soup) into a bspbrush_t.
Deduplicates coplanar faces by finding unique plane indices.
====================
*/

 bspbrush_t *BrushFromHull(colHull_t *hull, shaderInfo_t *si) {
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
  for (i = 0; i < hull->numTris; i++) {
    int idx0 = hull->tris[i][0];
    int idx1 = hull->tris[i][1];
    int idx2 = hull->tris[i][2];

    vec3_t p0, p1, p2;
    VectorCopy(hull->verts[idx0], p0);
    VectorCopy(hull->verts[idx1], p1);
    VectorCopy(hull->verts[idx2], p2);

    // Check for degenerate triangle (near zero area)
    vec3_t t1, t2, cross;
    VectorSubtract(p1, p0, t1);
    VectorSubtract(p2, p0, t2);
    CrossProduct(t1, t2, cross);
    float area = VectorLength(cross);
    if (area < 0.001f) {
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
        // Track the largest triangle per plane (for fallback windings)
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
        _printf("WARNING: BrushFromHull reached MAX_BRUSH_SIDES\n");
        break;
      }
      uniquePlanes[numUniquePlanes] = planenum;
      maxPlaneArea[numUniquePlanes] = area;
      VectorCopy(p0, trianglePoints[numUniquePlanes][0]);
      VectorCopy(p1, trianglePoints[numUniquePlanes][1]);
      VectorCopy(p2, trianglePoints[numUniquePlanes][2]);
      numUniquePlanes++;
    }
  }

  if (numUniquePlanes < 4) {
    return NULL;
  }

  // Allocate extra space for up to 6 axial bevel planes
  b = AllocBrush(numUniquePlanes + 6);
  b->numsides = numUniquePlanes;
  b->detail = qtrue;
  b->contents = CONTENTS_SOLID|CONTENTS_TRANSLUCENT|CONTENTS_DETAIL;
  b->contentShader = si;

  for (i = 0; i < numUniquePlanes; i++) {
    b->sides[i].planenum = uniquePlanes[i];
    b->sides[i].shaderInfo = si;
    b->sides[i].contents = b->contents;

    /* Allow some surface flags to pass through */
    int flags = si->surfaceFlags;
    flags &= ~(SURF_HINT | SURF_POINTLIGHT | SURF_NONSOLID | SURF_LIGHTFILTER | SURF_ALPHASHADOW);
    flags |= (SURF_NODRAW | SURF_NOLIGHTMAP | SURF_NODLIGHT);
    b->sides[i].surfaceFlags = flags;
  }

  // Try full-polygon windings from plane intersections first.
  // If that fails (e.g. plane snapping made faces degenerate),
  // fall back to 3-point windings from the best CoACD triangles.
  if (!CreateBrushWindings(b)) {
    _printf("WARNING: CreateBrushWindings failed, using triangle fallback\n");
    for (i = 0; i < numUniquePlanes; i++) {
      if (b->sides[i].winding) {
        FreeWinding(b->sides[i].winding);
      }
      b->sides[i].winding = AllocWinding(3);
      b->sides[i].winding->numpoints = 3;
      VectorCopy(trianglePoints[i][0], b->sides[i].winding->p[0]);
      VectorCopy(trianglePoints[i][1], b->sides[i].winding->p[1]);
      VectorCopy(trianglePoints[i][2], b->sides[i].winding->p[2]);
    }
    if (!BoundBrush(b)) {
      c_degenerate_hulls++;
      FreeBrush(b);
      return NULL;
    }
  }

  // Add axial bevels to prevent player snagging on seams
  AddBevelsToBrush(b);

  return b;
}

/*
====================
BrushesFromHulls

Converts an array of convex hulls to a linked list of brushes.
====================
*/
bspbrush_t *BrushesFromHulls(colHull_t **hulls, int numHulls, shaderInfo_t *si) {
  int i;
  bspbrush_t *list = NULL;

  c_degenerate_triangles = 0;
  c_degenerate_hulls = 0;

  for (i = 0; i < numHulls; i++) {
    bspbrush_t *b = BrushFromHull(hulls[i], si);
    if (b) {
      b->next = list;
      list = b;
    }
  }

  if (c_degenerate_triangles > 0 || c_degenerate_hulls > 0) {
    _printf("  Degenerate geometry skipped during hull conversion: %i triangles, %i hulls\n",
            c_degenerate_triangles, c_degenerate_hulls);
  }

  return list;
}

/*
====================
HealAndDecimateMesh

Takes raw vertex/triangle data from a draw surface,
constructs an MRMesh, heals it, decimates it, and
writes the result as an OBJ file for debugging.

Returns the MRMesh (caller must free with mrMeshFree).
====================
*/
static MRMesh *HealAndDecimateMesh(float *verts, int numVerts,
                                    int *indexes, int numIndexes,
                                    const char *debugName) {
  int i;
  int inputTris = numIndexes / 3;
  int numTriangles = inputTris;

  /* --- Step 1: Build MRMesh from triangles --- */
  MRVector3f *mrVerts = malloc(numVerts * sizeof(MRVector3f));
  MRThreeVertIds *mrTris = malloc(numTriangles * sizeof(MRThreeVertIds));

  for (i = 0; i < numVerts; i++) {
    mrVerts[i].x = verts[i * 3 + 0];
    mrVerts[i].y = verts[i * 3 + 1];
    mrVerts[i].z = verts[i * 3 + 2];
  }

  for (i = 0; i < numTriangles; i++) {
    mrTris[i][0].id = indexes[i * 3 + 0];
    mrTris[i][1].id = indexes[i * 3 + 1];
    mrTris[i][2].id = indexes[i * 3 + 2];
  }

  MRMesh *mesh = mrMeshFromTrianglesDuplicatingNonManifoldVertices(
      mrVerts, (size_t)numVerts,
      mrTris, (size_t)numTriangles
  );

  free(mrVerts);
  free(mrTris);

  if (!mesh) {
    _printf("  WARNING: MRMesh construction failed\n");
    return NULL;
  }

  size_t origPoints = mrMeshPointsNum(mesh);
  const MRMeshTopology *topo = mrMeshTopology(mesh);
  int origHoles = mrMeshTopologyFindNumHoles(topo, NULL);
  _printf("  MRMesh constructed: %zu verts, %d holes\n", origPoints, origHoles);

  /* --- Step 2: Vertex Welding --- */
  int welded = mrMeshBuilderUniteCloseVertices(mesh, 0.001f, false, NULL);
  if (welded > 0) {
    _printf("  Welded %d close vertices\n", welded);
    mrMeshInvalidateCaches(mesh, true);
  }

  /* --- Step 3: Fix multiple edges (manifold repair) --- */
  findAndFixMultipleEdges(mesh);

  /* --- Step 4: Fix degeneracies --- */
  {
    MRString *errStr = NULL;
    MRFixMeshDegeneraciesParams params = mrFixMeshDegeneraciesParamsNew();
    params.maxDeviation = 0.5f;
    params.tinyEdgeLength = 0.01f;
    params.criticalTriAspectRatio = 20000.0f;
    params.maxAngleChange = 3.14159f / 6.0f;  /* 30 degrees */
    params.mode = MRFixMeshDegeneraciesParamsModeRemeshPatch;
    mrFixMeshDegeneracies(mesh, &params, &errStr);
    if (errStr) {
      _printf("  WARNING: fixDegeneracies: %s\n", mrStringData(errStr));
      mrStringFree(errStr);
    }
  }

  /* --- Step 5: Fill holes --- */
  {
    MREdgePath *holeEdges = mrMeshFindHoleRepresentiveEdges(mesh);
    if (holeEdges && holeEdges->size > 0) {
      _printf("  Filling %zu holes\n", holeEdges->size);
      MRFillHoleParams fillParams = mrFillHoleParamsNew();
      mrFillHoles(mesh, holeEdges->data, holeEdges->size, &fillParams);
      mrEdgePathFree(holeEdges);
    } else if (holeEdges) {
      mrEdgePathFree(holeEdges);
    }
  }

  /* Verify holes after filling */
  topo = mrMeshTopology(mesh);
  int remainingHoles = mrMeshTopologyFindNumHoles(topo, NULL);
  _printf("  After healing: %zu verts, %d remaining holes\n",
          mrMeshPointsNum(mesh), remainingHoles);

  /* --- Step 6: Decimate --- */
  {
    MRDecimateSettings settings = mrDecimateSettingsNew();
    settings.strategy = MRDecimateStrategyMinimizeError;
    settings.maxError = 4.0f;           /* max distance deviation in world units */
    settings.maxTriangleAspectRatio = 20.0f;
    settings.tinyEdgeLength = 0.01f;
    settings.stabilizer = 0.001f;
    settings.optimizeVertexPos = true;
    settings.packMesh = true;

    MRDecimateResult result = mrDecimateMesh(mesh, &settings);
    
    /* Get final face count */
    const MRMeshTopology *finalTopo = mrMeshTopology(mesh);
    size_t finalFaces = mrBitSetCount((const MRBitSet *)mrMeshTopologyGetValidFaces(finalTopo));
    double reduction = inputTris > 0 ? (1.0 - (double)finalFaces / inputTris) * 100.0 : 0.0;

    _printf("  Decimated: %d verts deleted, %d faces deleted, error=%.3f\n",
            result.vertsDeleted, result.facesDeleted, result.errorIntroduced);
    _printf("  Summary: %d original tris -> %zu remaining tris (%.1f%% reduction)\n",
            inputTris, finalFaces, reduction);
  }

  return mesh;
}

/*
====================
WriteCollisionOBJ

Writes multiple collision meshes into a single OBJ file.
Q3 Z-up -> OBJ Y-up (X=X, Y=Z, Z=-Y)
====================
*/
static void WriteCollisionOBJ(colMesh_t **collision_meshes, int num_collision_meshes, const char *filename) {
  FILE *f = fopen(filename, "w");
  if (!f) {
    _printf("ERROR: Could not open %s for writing\n", filename);
    return;
  }

  fprintf(f, "# Unified Collision OBJ: %d meshes\n", num_collision_meshes);
  fprintf(f, "# Axis swap: Q3 Z-up -> OBJ Y-up (X=X, Y=Z, Z=-Y)\n");

  int vertexOffset = 0;
  for (int i = 0; i < num_collision_meshes; i++) {
    colMesh_t *m = collision_meshes[i];
    if (!m) continue;

    fprintf(f, "o mesh_%d\n", i);
    
    /* Vertices */
    for (int v = 0; v < m->numVerts; v++) {
      fprintf(f, "v %f %f %f\n", m->verts[v][0], m->verts[v][2], -m->verts[v][1]);
    }

    /* Faces (1-indexed + offset) */
    for (int t = 0; t < m->numTris; t++) {
      fprintf(f, "f %d %d %d\n", 
              m->tris[t][0] + vertexOffset + 1,
              m->tris[t][1] + vertexOffset + 1,
              m->tris[t][2] + vertexOffset + 1);
    }

    vertexOffset += m->numVerts;
  }

  fclose(f);
  _printf("  Wrote collision debug to %s\n", filename);
}

/*
====================
CreateCollisionTris

Runs the decimation and healing pipeline (MeshLib or MeshOptimizer) 
for a single instance, extracting cleaned `colMesh_t` objects prior to categorization.
====================
*/
void CreateCollisionTris(modelInstance_t *inst) {
  int j, k;

  int numRaw = inst->num_collision_meshes;
  colMesh_t **rawMeshes = malloc(sizeof(colMesh_t*) * numRaw);
  for (int i = 0; i < numRaw; i++) {
    rawMeshes[i] = inst->collision_meshes[i];
  }
  inst->num_collision_meshes = 0;

  for (int i = 0; i < numRaw; i++) {
    colMesh_t *raw = rawMeshes[i];
    if (raw->numVerts == 0 || raw->numTris == 0) {
      FreeCollisionMesh(raw);
      continue;
    }

    // Convert colMesh_t to raw arrays for HealAndDecimateMesh
    float *meshVerts = malloc(raw->numVerts * 3 * sizeof(float));
    int *meshIndexes = malloc(raw->numTris * 3 * sizeof(int));

    for (int k = 0; k < raw->numVerts; k++) {
      meshVerts[k * 3 + 0] = raw->verts[k][0];
      meshVerts[k * 3 + 1] = raw->verts[k][1];
      meshVerts[k * 3 + 2] = raw->verts[k][2];
    }
    for (int k = 0; k < raw->numTris; k++) {
      meshIndexes[k * 3 + 0] = raw->tris[k][0];
      meshIndexes[k * 3 + 1] = raw->tris[k][1];
      meshIndexes[k * 3 + 2] = raw->tris[k][2];
    }

    colMesh_t *colMesh = malloc(sizeof(colMesh_t));
    memset(colMesh, 0, sizeof(colMesh_t));
    colMesh->shaderInfo = raw->shaderInfo;

    /* --- MeshLib path --- */
    MRMesh *healed = HealAndDecimateMesh(meshVerts, raw->numVerts,
                                         meshIndexes, raw->numTris * 3,
                                         inst->modelName);
    if (healed) {
      const MRVector3f *pts = mrMeshPoints(healed);
      size_t numPts = mrMeshPointsNum(healed);
      const MRMeshTopology *topo = mrMeshTopology(healed);
      MRTriangulation *tri = mrMeshTopologyGetTriangulation(topo);

      if (tri && tri->size > 0) {
        colMesh->numVerts = (int)numPts;
        colMesh->verts = malloc(colMesh->numVerts * sizeof(vec3_t));
        memcpy(colMesh->verts, pts, colMesh->numVerts * sizeof(vec3_t));

        int validTriCount = 0;
        for (size_t fi = 0; fi < tri->size; fi++) {
          if (tri->data[fi][0].id >= 0) validTriCount++;
        }

        colMesh->numTris = validTriCount;
        colMesh->tris = malloc(colMesh->numTris * sizeof(colTri_t));
        
        int triIdx = 0;
        for (size_t fi = 0; fi < tri->size; fi++) {
          if (tri->data[fi][0].id >= 0) {
            colMesh->tris[triIdx][0] = tri->data[fi][0].id;
            colMesh->tris[triIdx][1] = tri->data[fi][1].id;
            colMesh->tris[triIdx][2] = tri->data[fi][2].id;
            triIdx++;
          }
        }
      }
      mrMeshFree(healed);
    }

    /* Store result if extraction succeeded */
    if (colMesh->numTris > 0 && inst->num_collision_meshes < MAX_MODEL_COLLISION_MESHES) {
      inst->collision_meshes[inst->num_collision_meshes++] = colMesh;
    } else {
      FreeCollisionMesh(colMesh);
    }

    free(meshIndexes);
    free(meshVerts);
    FreeCollisionMesh(raw); // Clean up the raw mesh!
  }
  free(rawMeshes);

  /* Unified OBJ export at the end */
  if (inst->num_collision_meshes > 0) {
    char objPath[1024];
    strcpy(objPath, inst->modelName);
    char *ext = strrchr(objPath, '.');
    if (ext && strchr(ext, '/') == NULL && strchr(ext, '\\') == NULL) {
      *ext = '\0';
    }
    strcat(objPath, "_coltris.obj"); 
    
    WriteCollisionOBJ(inst->collision_meshes, inst->num_collision_meshes, objPath);
  }
}

/*
====================
FreeCollisionTris

Frees all generated collision meshes cached on the instance.
====================
*/
void FreeCollisionTris(modelInstance_t *inst) {
  int i;
  for (i = 0; i < inst->num_collision_meshes; i++) {
    if (inst->collision_meshes[i]) {
      FreeCollisionMesh(inst->collision_meshes[i]);
      inst->collision_meshes[i] = NULL;
    }
  }
  inst->num_collision_meshes = 0;
}

/*
====================
DecomposeModelCollision

Generate collision hulls from the model's geometry.
====================
*/
static void DecomposeModelCollision(modelInstance_t *inst) {
  modelCategory_t category = inst->category;
  bspbrush_t *hulls_list = NULL;
  int numHulls = 0;

  if (num_clip_entity_groups >= MAX_CLIP_ENTITY_GROUPS) {
    _printf("WARNING: MAX_CLIP_ENTITY_GROUPS reached\n");
    return;
  }

  // Step 1: Pre-calculate thresholds and print info
  _printf("Instance %s: Decomposing as %s\n", inst->modelName, CategoryString(category));

  shaderInfo_t *caulk = ShaderInfoForShader("textures/common/caulk");
  qboolean mergeMeshes = (category == MC_WRAP) ? qtrue : qfalse;

  if (category == MC_TERRAIN) {
    hulls_list = GenerateCollisionTerrainExtrusion(inst, caulk);
  } else if (category == MC_OBJECT || category == MC_WALKABLE || category == MC_WRAP) {
      hulls_list = GenerateHACDCollision(inst, caulk);
  } else {
    hulls_list = GenerateCoACDCollision(inst, mergeMeshes, caulk);
  }

  if (hulls_list) {
    //CSGMergeBrushList(&hulls_list);
  }

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
    SetKeyValue(ent, "misc_model", inst->modelName);
    {
      char triangle_density_str[32];
      sprintf(triangle_density_str, "%.8f", inst->triangle_density);
      SetKeyValue(ent, "triangle_density", triangle_density_str);
    }
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
      group->brush_density = ((float)numHulls / volume) * DENSITY_STANDARD_VOLUME;
    } else {
      group->brush_density = 0;
    }

    _printf("Instance %s: Created clip group with %i brushes, density %.6f\n",
            inst->modelName, numHulls, group->brush_density);
  }
}


/*
====================
CreateTriangleModelCollision

Generates collision brushes from model geometry (per-instance pass).
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

  // Step 2: Extraction and Categorization Pass (per instance)
  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];
    
    // Abstracted unified mesh processing executes unconditionally first
    CreateCollisionTris(inst);
    CategorizeModel(inst);
  }

  // Step 3: Decomposition Pass (per instance)
  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];

    if (inst->category != MC_NONE) {
      DecomposeModelCollision(inst);
    }
    
    // Free the cleanly generated triangle geometry when done with the instance
    FreeCollisionTris(inst);
  }

  // Step 3: Preparation pass
  PrepareClipEntityGroups();

  if (WRITE_COLLISION_MAP) {
    char debugName[1024];
    sprintf(debugName, "%s_collision.map", source);
    WriteCollisionMap(debugName);
  }

  // Step 4: Map Entity Integration
  for (i = 0; i < num_clip_entity_groups; i++) {
    clip_entity_group_t *g = &clip_entity_groups[i];
    if (!g->entity) continue;

    const char *cls = ValueForKey(g->entity, "classname");

    if (!Q_stricmp(cls, "func_group")) {
      // Dump func_group brushes directly into worldspawn (entities[0])
      EmitBrushes(g->entity->brushes);
      MoveBrushesToWorld(g->entity);
      
      // Cleanup the temporary local entity container
      epair_t *next_ep;
      for (epair_t *curr_ep = g->entity->epairs; curr_ep; curr_ep = next_ep) {
        next_ep = curr_ep->next;
        free(curr_ep->key);
        free(curr_ep->value);
        free(curr_ep);
      }
      free(g->entity);
      g->entity = NULL;
    } 
    else if (!Q_stricmp(cls, "func_static")) {
      // Append func_static entities to the global map entities list
      if (num_entities >= MAX_MAP_ENTITIES) {
        Error("CreateTriangleModelCollision: num_entities == MAX_MAP_ENTITIES");
      }

      // Shallow copy transfers ownership of epairs and brushes pointers
      entities[num_entities] = *(g->entity);
      
      // Update entity reference for all attached brushes
      for (bspbrush_t *b = entities[num_entities].brushes; b; b = b->next) {
        b->entitynum = num_entities;
      }
      
      EmitBrushes(entities[num_entities].brushes);
      num_entities++;

      // Free just the local struct container, since contents are linked now
      free(g->entity);
      g->entity = NULL;
    }
  }
}
