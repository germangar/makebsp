/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

/*
model_meshlib.c — Enhanced Collision Pipeline

Pipeline:
  1. Extract mesh data from draw surfaces (Assimp already loaded them)
  2. Construct MRMesh (handling non-manifold vertices)
  3. Heal: vertex welding, fix multiple edges, fix degeneracies, fill holes
  4. Decimate: reduce triangle count while preserving shape
  5. Export debug OBJ for visual verification
  6. (Future) Feed healed/decimated mesh into HACD for convex decomposition
  
Called from model_collision.c for MC_OBJECT category.
*/

#include "qbsp.h"

/* MRMeshC headers */
#include "MRMeshFwd.h"
#include "MRVector3.h"
#include "MRId.h"
#include "MRMesh.h"
#include "MRMeshTopology.h"
#include "MRMeshBuilder.h"
#include "MRMeshFixer.h"
#include "MRMeshFillHole.h"
#include "MRMeshDecimate.h"
#include "MRMeshSave.h"
#include "MRString.h"
#include "MRBitSet.h"
#include "MRVector.h"

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

  /* --- Step 7: Export debug OBJ (Q3 Z-up → OBJ Y-up) --- */
  {
    char objPath[1024];
    sprintf(objPath, "%s_meshlib.obj", debugName);

    FILE *objFile = fopen(objPath, "w");
    if (objFile) {
      const MRVector3f *pts = mrMeshPoints(mesh);
      size_t numPts = mrMeshPointsNum(mesh);
      const MRMeshTopology *saveTopo = mrMeshTopology(mesh);
      MRTriangulation *tri = mrMeshTopologyGetTriangulation(saveTopo);

      fprintf(objFile, "# MeshLib healed/decimated mesh: %s\n", debugName);
      fprintf(objFile, "# %zu verts, %zu faces\n", numPts, tri ? tri->size : 0);
      fprintf(objFile, "# Axis swap: Q3 Z-up -> OBJ Y-up (X=X, Y=Z, Z=-Y)\n");

      /* Write vertices with axis swap */
      for (size_t vi = 0; vi < numPts; vi++) {
        fprintf(objFile, "v %f %f %f\n", pts[vi].x, pts[vi].z, -pts[vi].y);
      }

      /* Write faces (1-indexed) */
      if (tri) {
        for (size_t fi = 0; fi < tri->size; fi++) {
          /* Only write valid faces */
          MRThreeVertIds *face = &tri->data[fi];
          if ((*face)[0].id >= 0 && (*face)[1].id >= 0 && (*face)[2].id >= 0) {
            fprintf(objFile, "f %d %d %d\n", 
                    (*face)[0].id + 1, (*face)[1].id + 1, (*face)[2].id + 1);
          }
        }
        mrTriangulationFree(tri);
      }

      fclose(objFile);
      // _printf("  DEBUG: Wrote healed/decimated mesh to %s (%zu verts)\n", objPath, numPts);
    }
  }

  return mesh;
}

/*
====================
GenerateMLCollision

Generates collision brushes for a model instance using the
Enhanced Collision Pipeline (MeshLib heal/decimate + future HACD decompose).

Currently: processes each draw surface mesh through MeshLib healing
and decimation, exports debug OBJ, and returns an empty brush list.
HACD decomposition will be added after visual verification.
====================
*/
bspbrush_t *GenerateMLCollision(modelInstance_t *inst, shaderInfo_t *shader) {
  int j, k;
  mapDrawSurface_t *ds;

  _printf("Instance %s: Running MeshLib Pipeline (%s)\n",
          inst->modelName, CategoryString(inst->category));

  for (j = 0; j < inst->numDrawSurfs; j++) {
    ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
      continue;
    }

    if (ds->numVerts == 0 || ds->numIndexes == 0) {
      continue;
    }

    /* Extract vertex data as flat float array */
    float *meshVerts = malloc(ds->numVerts * 3 * sizeof(float));
    int *meshIndexes = malloc(ds->numIndexes * sizeof(int));

    for (k = 0; k < ds->numVerts; k++) {
      meshVerts[k * 3 + 0] = (float)ds->verts[k].xyz[0];
      meshVerts[k * 3 + 1] = (float)ds->verts[k].xyz[1];
      meshVerts[k * 3 + 2] = (float)ds->verts[k].xyz[2];
    }
    for (k = 0; k < ds->numIndexes; k++) {
      meshIndexes[k] = ds->indexes[k];
    }

    _printf("  Mesh %d: %d verts, %d tris\n", j, ds->numVerts, ds->numIndexes / 3);

    /* Run the MeshLib healing + decimation pipeline */
    MRMesh *healed = HealAndDecimateMesh(meshVerts, ds->numVerts,
                                          meshIndexes, ds->numIndexes,
                                          inst->modelName);

    if (healed) {
      /* TODO: Extract healed mesh data and feed into HACD */
      mrMeshFree(healed);
    }

    free(meshVerts);
    free(meshIndexes);
  }

  _printf("Instance %s: MeshLib pipeline complete (OBJ exported, no brushes yet)\n",
          inst->modelName);

  /* Return NULL for now — no brushes until HACD is integrated */
  return NULL;
}
