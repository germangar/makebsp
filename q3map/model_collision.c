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

/*
====================
BrushFromMesh

Converts a convex hull (triangle soup) into a bspbrush_t.
Deduplicates coplanar faces by finding unique plane indices.
====================
*/
bspbrush_t *BrushFromMesh(CoACD_Mesh *mesh) {
  int i, j;
  int numUniquePlanes = 0;
  int uniquePlanes[MAX_BRUSH_SIDES];
  vec3_t trianglePoints[MAX_BRUSH_SIDES][3];
  bspbrush_t *b;

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

    // MapPlaneFromPoints computes normal pointing OUT for CW triangles
    int planenum = MapPlaneFromPoints(p0, p2, p1);

    // Deduplicate against existing planes in this brush
    for (j = 0; j < numUniquePlanes; j++) {
      if (uniquePlanes[j] == planenum)
        break;
    }

    if (j == numUniquePlanes) {
      if (numUniquePlanes >= MAX_BRUSH_SIDES) {
        _printf("WARNING: BrushFromMesh reached MAX_BRUSH_SIDES\n");
        break;
      }
      uniquePlanes[numUniquePlanes] = planenum;
      // Capture the triangle points to define the plane in the .map file
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
  for (i = 0; i < numUniquePlanes; i++) {
    b->sides[i].planenum = uniquePlanes[i];
    // Create a 3-point winding for WriteBspBrushMap
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
CreateTriangleModelCollision

Generates collision brushes from aggregated model geometry using CoACD.
====================
*/
void CreateTriangleModelCollision(void) {
  int i, j, k;
  modelInstance_t *inst;
  mapDrawSurface_t *ds;
  int totalVerts = 0;
  int totalIndexes = 0;
  double *allVerts;
  int *allIndexes;
  int currentVert = 0;
  int currentIndex = 0;
  bspbrush_t *allCollisionBrushes = NULL;

  _printf("----- CreateTriangleModelCollision -----\n");

  // Step 1: Count total solid vertices and indices
  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];
    for (j = 0; j < inst->numDrawSurfs; j++) {
      ds = inst->drawSurfs[j];
      if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
        continue;
      }
      totalVerts += ds->numVerts;
      totalIndexes += ds->numIndexes;
    }
  }

  if (totalVerts == 0) {
    _printf("No solid model geometry found for collision.\n");
    return;
  }

  _printf("Aggregating %i vertices and %i indices...\n", totalVerts,
          totalIndexes);

  // Step 2: Allocate and fill buffers
  allVerts = malloc(totalVerts * 3 * sizeof(double));
  allIndexes = malloc(totalIndexes * sizeof(int));

  for (i = 0; i < numModelInstances; i++) {
    inst = &modelInstances[i];
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
  }

  // Step 3: Run CoACD
  CoACD_Mesh input;
  input.vertices_ptr = allVerts;
  input.vertices_count = totalVerts;
  input.triangles_ptr = allIndexes;
  input.triangles_count = totalIndexes / 3;

  _printf("Running CoACD decomposition (threshold=0.05, preprocess=auto)...\n");
  fflush(stdout);

  CoACD_MeshArray hulls = CoACD_run(&input,
                                    0.05, // threshold
                                    -1,   // max_convex_hull
                                    COACD_PREPROCESS_AUTO,
                                    50,           // prep_resolution
                                    2000,         // sample_resolution
                                    20,           // mcts_nodes
                                    100,          // mcts_iteration
                                    3,            // mcts_max_depth
                                    false,        // pca
                                    true,         // merge
                                    false,        // decimate
                                    256,          // max_ch_vertex
                                    false,        // extrude
                                    0.01,         // extrude_margin
                                    COACD_APX_CH, // apx_mode
                                    1234          // seed
  );

  _printf("CoACD generated %i convex hulls.\n", (int)hulls.meshes_count);

  // Step 4: Convert hulls to bspbrushes
  for (i = 0; i < hulls.meshes_count; i++) {
    bspbrush_t *b = BrushFromMesh(&hulls.meshes_ptr[i]);
    if (b) {
      b->next = allCollisionBrushes;
      allCollisionBrushes = b;
    }
  }

  _printf("Converted to %i BSP brushes.\n",
          CountBrushList(allCollisionBrushes));

  // Step 5: Diagnostic visualization
  if (allCollisionBrushes) {
    char debugName[1024];
    sprintf(debugName, "%s_collision.map", source);
    _printf("Writing diagnostic map: %s\n", debugName);
    WriteBspBrushMap(debugName, allCollisionBrushes, "common/fullclip");
  }

  // Cleanup
  CoACD_freeMeshArray(hulls);
  free(allVerts);
  free(allIndexes);

  // TODO: Classification and linkage to entities
}
