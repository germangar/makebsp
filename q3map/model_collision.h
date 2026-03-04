#ifndef MODEL_COLLISION_H
#define MODEL_COLLISION_H

#include "qbsp.h"

/* 
=====================================================================
Unified Collision Mesh/Hull Formats
===================================================================== 
*/

typedef int colTri_t[3];

// Represents complex decimated model geometry (non-convex)
typedef struct colMesh_s {
    vec3_t   *verts;
    int       numVerts;
    colTri_t *tris;
    int       numTris;
    struct shaderInfo_s *shaderInfo;
} colMesh_t;

// Represents a small convex hull from decomposition
typedef struct colHull_s {
    vec3_t   *verts;
    int       numVerts;
    colTri_t *tris;
    int       numTris;
} colHull_t;

/* Memory management */
void FreeCollisionMesh(colMesh_t *mesh);
void FreeCollisionHull(colHull_t *hull);

/* Shared logic */
bspbrush_t *BrushFromHull(colHull_t *hull, shaderInfo_t *si);
bspbrush_t *BrushesFromHulls(colHull_t **hulls, int numHulls, shaderInfo_t *si);
bspbrush_t *ExtrudeTrianglesToBrushes(colMesh_t *mesh, shaderInfo_t *si);
bspbrush_t *CombineBrushes(bspbrush_t *list, bspbrush_t *newBrushes);
#endif
