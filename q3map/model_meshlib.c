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
#include "model_collision.h"

/* HACD Wrapper */
#include "hacd_c_wrapper.h"

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

static bspbrush_t *BrushesFromHullsHACD(HACD_Wrapper *hacd, shaderInfo_t *si) {
    bspbrush_t *list = NULL;
    size_t numClusters = hacd_get_nclusters(hacd);
    colHull_t **hulls = malloc(numClusters * sizeof(colHull_t *));
    int validHulls = 0;
    
    for (size_t i = 0; i < numClusters; i++) {
        size_t nPoints = hacd_get_npoints_ch(hacd, i);
        size_t nTris = hacd_get_ntriangles_ch(hacd, i);
        
        HACD_Vec3 *points = malloc(nPoints * sizeof(HACD_Vec3));
        HACD_Triangle *tris = malloc(nTris * sizeof(HACD_Triangle));
        
        if (hacd_get_ch(hacd, i, points, tris)) {
            colHull_t *hull = malloc(sizeof(colHull_t));
            hull->numVerts = (int)nPoints;
            hull->verts = malloc(nPoints * sizeof(vec3_t));
            for (size_t v = 0; v < nPoints; v++) {
                hull->verts[v][0] = (float)points[v].x;
                hull->verts[v][1] = (float)points[v].y;
                hull->verts[v][2] = (float)points[v].z;
            }
            hull->numTris = (int)nTris;
            hull->tris = malloc(nTris * sizeof(colTri_t));
            for (size_t t = 0; t < nTris; t++) {
                hull->tris[t][0] = tris[t].v1;
                hull->tris[t][1] = tris[t].v2;
                hull->tris[t][2] = tris[t].v3;
            }
            hulls[validHulls++] = hull;
        }
        free(points);
        free(tris);
    }
    
    if (validHulls > 0) {
        list = BrushesFromHulls(hulls, validHulls, si);
    }
    
    for (int i = 0; i < validHulls; i++) {
        FreeCollisionHull(hulls[i]);
    }
    free(hulls);
    
    return list;
}


/*
====================
GenerateMLCollision

Generates collision brushes for a model instance using the pre-extracted
MeshLib colMesh_t geometries. It feeds these directly into HACD.
====================
*/
bspbrush_t *GenerateMLCollision(modelInstance_t *inst, shaderInfo_t *shader) {
  bspbrush_t *hulls_list = NULL;

  _printf("Instance %s: Running MeshLib/HACD Pipeline (%s)\n",
          inst->modelName, CategoryString(inst->category));

  if (inst->num_collision_meshes == 0) {
    return NULL;
  }

  for (int j = 0; j < inst->num_collision_meshes; j++) {
    colMesh_t *colMesh = inst->collision_meshes[j];
    if (!colMesh || colMesh->numTris == 0) continue;

    /* Call HACD extruder */
    shaderInfo_t *si = (shader != NULL) ? shader : NULL;

    HACD_Vec3 *hacdPts = malloc(colMesh->numVerts * sizeof(HACD_Vec3));
    for (int i = 0; i < colMesh->numVerts; i++) {
        hacdPts[i].x = colMesh->verts[i][0];
        hacdPts[i].y = colMesh->verts[i][1];
        hacdPts[i].z = colMesh->verts[i][2];
    }

    HACD_Triangle *hacdTris = malloc(colMesh->numTris * sizeof(HACD_Triangle));
    for (int i = 0; i < colMesh->numTris; i++) {
        hacdTris[i].v1 = colMesh->tris[i][0];
        hacdTris[i].v2 = colMesh->tris[i][1];
        hacdTris[i].v3 = colMesh->tris[i][2];
    }

    HACD_Wrapper *hacd = hacd_new();
    hacd_set_points(hacd, hacdPts, colMesh->numVerts);
    hacd_set_triangles(hacd, hacdTris, colMesh->numTris);
    
    /* --- DON'T DELETE THIS COMMENT. IT'S IMPORTANT. ---
    Good wood-bridge settings: 
    disable_normalize=false, 
    compacity=0.0001, 
    volume=0.0, 
    cc_connect_dist=0.0, 
    nclusters=1, 
    concavity=100000.0, 
    extra_dist_points=true, 
    add_faces_points=true
     --- DON'T DELETE THIS COMMENT. IT'S IMPORTANT. ---
    */
    
    /* --- Unnormalized Equivalence Scaling ---
       HACD's default behavior normalizes all models to a 2000.0 unit bounding box diagonal.
       This secretes scale factors that vary per-model. By disabling normalization and 
       dividing our distance thresholds by the scale factor that our baseline 'wood-bridge' 
       model used (1.8308), we can apply these precise unnormalized physical distances 
       consistently to ALL models. 
       Change this to 1.0 and disable_normalize to false to test native HACD behavior. */
    double hacd_scale_factor = 1.8308; // The "magic number" from wood-bridge (2000.0 / 1092.38)
    
    hacd_set_disable_normalize(hacd, true);
    hacd_set_compacity_weight(hacd, 0.0001); 
    hacd_set_volume_weight(hacd, 0.0000);    
    hacd_set_cc_connect_dist(hacd, 0.0 / hacd_scale_factor);
    
    size_t targetClusters = 1;
    hacd_set_nclusters(hacd, targetClusters); 
    hacd_set_concavity(hacd, 100000.0 / hacd_scale_factor); 
    hacd_set_add_extra_dist_points(hacd, true); 
    hacd_set_add_faces_points(hacd, true);      
    
    _printf("  Running HACD on %d verts, %d tris (Threshold: %.1f, Min Hulls: %zu)\n", 
            colMesh->numVerts, colMesh->numTris, 100.0, targetClusters);
            
    if (hacd_compute(hacd, false)) {
        bspbrush_t *surfBrushes = BrushesFromHullsHACD(hacd, si);
        hulls_list = CombineBrushes(hulls_list, surfBrushes);
    } else {
        _printf("  WARNING: HACD computation failed for this mesh.\n");
    }

    hacd_delete(hacd);
    free(hacdPts);
    free(hacdTris);
  }

  _printf("Instance %s: MeshLib pipeline complete (%s)\n",
          inst->modelName, hulls_list ? "Brushes generated" : "No brushes");

  return hulls_list;
}

