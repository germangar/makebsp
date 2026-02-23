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
#define MAX_CLIP_ENTITY_GROUPS 16 // Max number of clip entity groups
typedef struct {
  entity_t *entity;
  float brush_density;
  vec3_t mins, maxs;
  int numBrushes;
} clip_entity_group_t;

clip_entity_group_t clip_entity_groups[MAX_CLIP_ENTITY_GROUPS];
int num_clip_entity_groups = 0;

int c_degenerate_triangles = 0;
int c_degenerate_hulls = 0;


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

  return b;
}

/*
====================
CategorizeModel

Reserved for future entity-specific classification for CoACD.
====================
*/
static void CategorizeModel(modelInstance_t *inst) {
  // Shell for future entity-based classification (e.g., checking epairs)
}

/*
====================
DecomposeModelCollision

Aggregates solid geometry for one instance, runs CoACD, and converts results to brushes.
====================
*/
static void DecomposeModelCollision(modelInstance_t *inst) {
  int j, k;
  mapDrawSurface_t *ds;
  int totalVerts = 0;
  int totalIndexes = 0;
  double *allVerts;
  int *allIndexes;
  int currentVert = 0;
  int currentIndex = 0;

  // Reset per-model degenerate counters
  c_degenerate_triangles = 0;
  c_degenerate_hulls = 0;

  // Step 1: Count solid vertices and indices for this instance
  for (j = 0; j < inst->numDrawSurfs; j++) {
    ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
      continue;
    }
    totalVerts += ds->numVerts;
    totalIndexes += ds->numIndexes;
  }

  if (totalVerts == 0) {
    return;
  }

  _printf("Instance %s: Aggregating %i vertices and %i indices...\n",
          inst->modelName, totalVerts, totalIndexes);

  // Step 2: Allocate and fill buffers
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

  // Step 3: Run CoACD for this instance
  CoACD_Mesh input;
  input.vertices_ptr = allVerts;
  input.vertices_count = totalVerts;
  input.triangles_ptr = allIndexes;
  input.triangles_count = totalIndexes / 3;

  _printf("Running CoACD decomposition for instance %s...\n", inst->modelName);
  fflush(stdout);

  CoACD_MeshArray hulls =
      CoACD_run(&input,
                0.2, // threshold
                -1,  // max_convex_hull
                COACD_PREPROCESS_AUTO,
                50,                      // prep_resolution
                2000,                    // sample_resolution
                20,                      // mcts_nodes
                100,                     // mcts_iteration
                3,                       // mcts_max_depth
                false,                   // pca
                true,                    // merge
                true,                    // decimate
                (int)(totalVerts * 0.9), // max_ch_vertex
                false,                   // extrude
                0.01,                    // extrude_margin
                COACD_APX_CH,            // apx_mode
                1234                     // seed
      );

  _printf("CoACD generated %i convex hulls.\n", (int)hulls.meshes_count);

  // Step 4: Convert hulls to bspbrushes and attach to instance
  shaderInfo_t *caulk = ShaderInfoForShader("textures/common/caulk");
  for (j = 0; j < (int)hulls.meshes_count; j++) {
    bspbrush_t *b = BrushFromMesh(&hulls.meshes_ptr[j], caulk);
    if (b) {
      b->next = inst->collisionBrushes;
      inst->collisionBrushes = b;
    }
  }

  // Step 5: Per-model reporting
  _printf("Instance %s: Converted to %i BSP brushes.\n", inst->modelName,
          CountBrushList(inst->collisionBrushes));

  if (c_degenerate_triangles > 0 || c_degenerate_hulls > 0) {
    _printf("Instance %s: Degenerate geometry skipped: %i triangles, %i hulls\n",
            inst->modelName, c_degenerate_triangles, c_degenerate_hulls);
  }

  // Cleanup
  CoACD_freeMeshArray(hulls);
  free(allVerts);
  free(allIndexes);
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
  bspbrush_t *allCollisionBrushes = NULL;
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

  // Step 2: Decomposition and Categorization Pass (per instance)
  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];
    CategorizeModel(inst);
    DecomposeModelCollision(inst);
  }

  if (WRITE_COLLISION_MAP) {
    // Collect all instance brushes for diagnostic map
    for (i = 0; i < numModelInstances; i++) {
      inst = &modelInstances[i];
      if (inst->collisionBrushes) {
        bspbrush_t *last = inst->collisionBrushes;
        while (last->next) {
          last = last->next;
        }
        last->next = allCollisionBrushes;
        allCollisionBrushes = inst->collisionBrushes;
      }
    }

    // Step 4: Diagnostic visualization
    if (allCollisionBrushes) {
      char debugName[1024];
      sprintf(debugName, "%s_collision.map", source);
      _printf("Writing diagnostic map: %s\n", debugName);
      WriteBspBrushMap(debugName, allCollisionBrushes);

      // We should probably detach them again if we don't want to pollute allCollisionBrushes,
      // but since this is the end of the function and allCollisionBrushes is local, it's fine.
    }
  }
}
