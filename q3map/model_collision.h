#ifndef MODEL_COLLISION_H
#define MODEL_COLLISION_H

#include "qbsp.h"

/* 
=====================================================================
Unified Collision Mesh Format
Used as an intermediate representation between simplifiers and extruders/HACD.
Memory layout matches MeshLib and MeshOptimizer for zero-copy/fast-copy.
===================================================================== 
*/

typedef int colTri_t[3];

typedef struct {
    vec3_t   *verts;
    int       numVerts;
    colTri_t *tris;
    int       numTris;
} collisionMesh_t;

/* Memory management */
void FreeCollisionMesh(collisionMesh_t *mesh);

/* Shared extrusion logic (from model_moptimizer.c) */
bspbrush_t *ExtrudeTrianglesToBrushes(collisionMesh_t *mesh, shaderInfo_t *si);
bspbrush_t *CombineBrushes(bspbrush_t *list, bspbrush_t *newBrushes);

#endif
