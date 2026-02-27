/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "qbsp.h"
#include "../libs/coacd_api.h"
#include <stdio.h>

static int c_degenerate_triangles = 0;
static int c_degenerate_hulls = 0;

/*
====================
BrushFromMesh

Converts a convex hull (triangle soup) into a bspbrush_t.
Deduplicates coplanar faces by finding unique plane indices.
====================
*/
static bspbrush_t *BrushFromMesh(CoACD_Mesh *mesh, shaderInfo_t *si) {
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
        _printf("WARNING: BrushFromMesh reached MAX_BRUSH_SIDES\n");
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

  b = AllocBrush(numUniquePlanes);
  b->numsides = numUniquePlanes;
  b->detail = qtrue;
  b->contents = si->contents;
  b->contentShader = si;

  for (i = 0; i < numUniquePlanes; i++) {
    b->sides[i].planenum = uniquePlanes[i];
    b->sides[i].shaderInfo = si;
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

  return b;
}

/*
====================
GenerateCoACDCollision

Generates collision brushes for a given model instance using CoACD.
====================
*/
bspbrush_t *GenerateCoACDCollision(modelInstance_t *inst, qboolean mergeMeshes, shaderInfo_t *shader) {
  int j, k;
  mapDrawSurface_t *ds;
  int totalVerts = 0;
  int totalIndexes = 0;
  double *allVerts;
  int *allIndexes;
  int currentVert = 0;
  int currentIndex = 0;
  bspbrush_t *hulls_list = NULL;

  c_degenerate_triangles = 0;
  c_degenerate_hulls = 0;

  float threshold, resolution, prep_resolution;
  int mcts_max_depth;
  qboolean decimate;
  modelCategory_t category = inst->category;
  switch (category) {
    case MC_WALKABLE:
      threshold = 0.1f;
      resolution = 1500;
      prep_resolution = 100;
      mcts_max_depth = 3;
      decimate = qtrue;
      break;
    case MC_FULL:
      threshold = 0.2f;
      resolution = 1000;
      prep_resolution = 30;
      mcts_max_depth = 3;
      decimate = qtrue;
      break;
    case MC_SHELL:
      threshold = 0.2f;
      resolution = 80;
      prep_resolution = 50;
      mcts_max_depth = 3;
      decimate = qtrue;
      break;
    case MC_OBJECT:
    default:
      threshold = 0.025f;
      resolution = 1000;
      prep_resolution = 100;
      mcts_max_depth = 10;
      decimate = qfalse;
      break;
    case MC_NONE:
      return NULL;
  }

  _printf("Instance %s: Decomposing as %s (threshold %.2f)\n", 
          inst->modelName, CategoryString(category), threshold);

  if (mergeMeshes) {
    // ALL model vertexes callculated at once (meshes merged)
    for (j = 0; j < inst->numDrawSurfs; j++) {
      ds = inst->drawSurfs[j];
      if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
        continue;
      }
      totalVerts += ds->numVerts;
      totalIndexes += ds->numIndexes;
    }

    if (totalVerts == 0) return NULL;

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

    CoACD_MeshArray hulls = CoACD_run(
      &input, 
      threshold,            // threshold
      -1,                   // max_convex_hull (no limit)
      COACD_PREPROCESS_AUTO,// preprocess_mode
      prep_resolution,      // prep_resolution (voxel grid)
      resolution,           // sample_resolution (ACTUAL accuracy)
      20,                   // mcts_nodes
      100,                  // mcts_iterations
      mcts_max_depth,       // mcts_max_depth
      false,                // pca
      false,                // merge (DISABLED FOR FIDELITY)
      decimate,             // decimate
      MAX_POINTS_ON_WINDING,// max_ch_vertex
      false,                // extrude
      0.01,                 // extrude_margin
      COACD_APX_CH,         // apx_mode
      1234                  // seed
    );

    _printf("Instance %s: Library produced %i raw hulls\n", inst->modelName, (int)hulls.meshes_count);

    for (j = 0; j < (int)hulls.meshes_count; j++) {
      bspbrush_t *b = BrushFromMesh(&hulls.meshes_ptr[j], shader);
      if (b) {
        b->next = hulls_list;
        hulls_list = b;
      }
    }
    
    CoACD_freeMeshArray(hulls);
    free(allVerts);
    free(allIndexes);
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

      CoACD_MeshArray hulls = CoACD_run(
        &input, 
        threshold,            // threshold
        -1,                   // max_convex_hull (no limit)
        COACD_PREPROCESS_AUTO,// preprocess_mode
        prep_resolution,      // prep_resolution (voxel grid)
        resolution,           // sample_resolution (ACTUAL accuracy)
        20,                   // mcts_nodes
        100,                  // mcts_iterations
        mcts_max_depth,       // mcts_max_depth
        false,                // pca
        false,                // merge (DISABLED FOR FIDELITY)
        decimate,             // decimate
        MAX_POINTS_ON_WINDING,// max_ch_vertex
        false,                // extrude
        0.01,                 // extrude_margin
        COACD_APX_CH,         // apx_mode
        1234                  // seed
      );

      _printf("Instance %s: Library produced %i raw hulls (one mesh)\n", inst->modelName, (int)hulls.meshes_count);

      for (k = 0; k < (int)hulls.meshes_count; k++) {
        bspbrush_t *b = BrushFromMesh(&hulls.meshes_ptr[k], shader);
        if (b) {
          b->next = hulls_list;
          hulls_list = b;
        }
      }

      CoACD_freeMeshArray(hulls);
      free(meshVerts);
      free(meshIndexes);
    }
  }

  if (c_degenerate_triangles > 0 || c_degenerate_hulls > 0) {
    _printf("Instance %s: Degenerate geometry skipped: %i triangles, %i hulls\n",
            inst->modelName, c_degenerate_triangles, c_degenerate_hulls);
  }

  if (hulls_list) {
    CSGMergeBrushList(&hulls_list);
  }

  return hulls_list;
}
