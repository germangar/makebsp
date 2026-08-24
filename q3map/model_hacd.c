/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

/*
HACD generates convex hulls from meshes without voxelization.
It works better than V-HACD when you don't want soft wrapping.

The HACD library option to disable model size normalization
was broken. We had to patch the library source code to
reenable the hacd_set_disable_normalize functionality
*/

#include "qbsp.h"
#include "model_collision.h"

/* HACD Wrapper */
#include "../libs/hacd/hacd_c_wrapper.h"


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

#define MS_TINY 64.0
#define MS_SMALL 256.0
#define MS_MEDIUM 512.0
#define MS_LARGE 1024.0

typedef struct {
    double scaleFactor;
    qboolean disableNormalize;
    double compacity;
    double volume;
    double concavity;
    size_t nClusters;
    double ccConnectDist;
    qboolean extraPoints;
    qboolean facePoints;
} hacdSettings_t;

/* Settings for large models (like wood-bridge) - Unnormalized */
static hacdSettings_t hacd_settings_object = {
    1.8308,   // scaleFactor (2000 / 1092.38)
    qtrue,    // disableNormalize
    0.0001,   // compacity
    0.0,      // volume
    100000.0, // concavity
    1,        // nClusters
    0.0,      // ccConnectDist
    qtrue,    // extraPoints
    qtrue     // facePoints
};

/* Settings for wrap/soft-wrap (TINY/Default) - Normalized soft wrap for stability */
static hacdSettings_t hacd_settings_wrap = {
    1.0,      // scaleFactor (standard)
    qfalse,   // disableNormalize (Scale tiny models to 2000u box for stability)
    0.0,      // compacity
    0.0,      // volume
    100000.0, // concavity (Merge everything into one hull)
    1,        // nClusters
    100.0,    // ccConnectDist (Force merge disconnected tiny parts)
    qtrue,    // extraPoints (Helps library see the shell properly)
    qfalse    // facePoints
};

/* Settings for wrap/soft-wrap (MC_WRAP & MS_LARGE)*/
static hacdSettings_t hacd_settings_bigwrap = {
    1.0,      // scaleFactor (standard)
    qtrue,    // disableNormalize (unnormalized scaling)
    0.1,      // compacity (Pull the hull tighter to the surface)
    0.0,      // volume
    100000.0, // concavity (Merge everything into one hull)
    1,        // nClusters
    50.0,     // ccConnectDist (Bridge minor gaps but stay tighter)
    qtrue,    // extraPoints (Helps library see the silhouette)
    qtrue     // facePoints (Captures interior mesh points for tighter wrap)
};

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
GenerateHACDCollision

Generates collision brushes for a model instance using the pre-extracted
MeshLib colMesh_t geometries. It feeds these directly into HACD.
====================
*/
bspbrush_t *GenerateHACDCollision(modelInstance_t *inst, shaderInfo_t *shader) {
  bspbrush_t *hulls_list = NULL;


  if (inst->num_collision_meshes == 0) {
    return NULL;
  }

  for (int j = 0; j < inst->num_collision_meshes; j++) {
    colMesh_t *colMesh = inst->collision_meshes[j];
    if (!colMesh || colMesh->numTris == 0) continue;

    /* Call HACD extruder */

    #define ENABLE_SNAP_GRID 0
    #define SNAP_GRID 0.125f
    HACD_Vec3 *hacdPts = malloc(colMesh->numVerts * sizeof(HACD_Vec3));
    for (int i = 0; i < colMesh->numVerts; i++) {
#if ENABLE_SNAP_GRID
        hacdPts[i].x = roundf(colMesh->verts[i][0] / SNAP_GRID) * SNAP_GRID;
        hacdPts[i].y = roundf(colMesh->verts[i][1] / SNAP_GRID) * SNAP_GRID;
        hacdPts[i].z = roundf(colMesh->verts[i][2] / SNAP_GRID) * SNAP_GRID;
#else
        hacdPts[i].x = colMesh->verts[i][0];
        hacdPts[i].y = colMesh->verts[i][1];
        hacdPts[i].z = colMesh->verts[i][2];
#endif
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
    
    /* Calculate model size to choose settings */
    vec3_t mins, maxs;
    ClearBounds(mins, maxs);
    for (int i = 0; i < colMesh->numVerts; i++) {
        AddPointToBounds(colMesh->verts[i], mins, maxs);
    }

    vec3_t size;
    VectorSubtract(maxs, mins, size);
    double diagonal = VectorLength(size);

    hacdSettings_t *s;

    if (inst->category == MC_WRAP || inst->category == MC_WRAPDETAIL)
    {
        if (diagonal > MS_LARGE) {
            s = &hacd_settings_bigwrap;
        } else {
            s = &hacd_settings_wrap;
        }
    }
    else 
    {
        if (diagonal <= MS_TINY) {
            s = &hacd_settings_wrap;
        } else {
            s = &hacd_settings_object;
        }
    }

    hacd_set_disable_normalize(hacd, s->disableNormalize);
    hacd_set_compacity_weight(hacd, s->compacity); 
    hacd_set_volume_weight(hacd, s->volume);    
    hacd_set_cc_connect_dist(hacd, s->ccConnectDist);

    hacd_set_nclusters(hacd, s->nClusters); 
    hacd_set_scale_factor(hacd, s->scaleFactor * 1000.0);
    hacd_set_concavity(hacd, s->concavity); 
    hacd_set_add_extra_dist_points(hacd, s->extraPoints); 
    hacd_set_add_faces_points(hacd, s->facePoints);      
    
            
    if (hacd_compute(hacd, false)) {
        bspbrush_t *surfBrushes = BrushesFromHullsHACD(hacd, colMesh->shaderInfo);
        hulls_list = CombineBrushes(hulls_list, surfBrushes);
    } else {
        _printf("  WARNING: HACD computation failed for this mesh.\n");
    }

    hacd_delete(hacd);
    free(hacdPts);
    free(hacdTris);
  }


  return hulls_list;
}
