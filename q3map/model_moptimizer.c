#include "qbsp.h"
#include "../libs/meshoptimizer/src/meshoptimizer.h"

/*
====================
ExtrudeTrianglesToBrushes

Takes a set of optimized vertices and indices, iterates through 
the triangles, and extrudes each one into a convex bspbrush_t.
Currently processes a single triangle per brush, but signature
allows future grouping of convex shapes.
====================
*/
static bspbrush_t *ExtrudeTrianglesToBrushes(double *vertsRaw, unsigned int *indices, int numIndices, shaderInfo_t *si) {
  bspbrush_t *hulls_list = NULL;
  int i;
  float *verts = (float*)vertsRaw;
  
  // Create one brush per triangle
  for (i = 0; i < numIndices; i += 3) {
    vec3_t p0, p1, p2;
    p0[0] = (double)verts[indices[i+0] * 3 + 0];
    p0[1] = (double)verts[indices[i+0] * 3 + 1];
    p0[2] = (double)verts[indices[i+0] * 3 + 2];
    
    p1[0] = (double)verts[indices[i+1] * 3 + 0];
    p1[1] = (double)verts[indices[i+1] * 3 + 1];
    p1[2] = (double)verts[indices[i+1] * 3 + 2];
    
    p2[0] = (double)verts[indices[i+2] * 3 + 0];
    p2[1] = (double)verts[indices[i+2] * 3 + 1];
    p2[2] = (double)verts[indices[i+2] * 3 + 2];
    
    vec3_t faceNormal;
    vec3_t t1, t2;
    VectorSubtract(p1, p0, t1);
    VectorSubtract(p2, p0, t2);
    CrossProduct(t1, t2, faceNormal);
    VectorNormalize(faceNormal, faceNormal);
    
    // We want 5 planes for a simple extrusion (face, backface, 3 edges)
    bspbrush_t *b = AllocBrush(5);
    b->numsides = 5;
    b->detail = qtrue;
    b->contents = si->contents;
    b->contentShader = si;
    
    // 1. The main face plane (pointing outwards)
    b->sides[0].planenum = MapPlaneFromPoints(p0, p2, p1);
    b->sides[0].shaderInfo = si;
    
    // 2. The back face (extruded inwards along the negative normal)
    // For a rough extrusion, push the points inward by 4 units
    float extrudeDist = 4.0f;
    vec3_t bp0, bp1, bp2;
    VectorMA(p0, -extrudeDist, faceNormal, bp0);
    VectorMA(p1, -extrudeDist, faceNormal, bp1);
    VectorMA(p2, -extrudeDist, faceNormal, bp2);
    
    b->sides[1].planenum = MapPlaneFromPoints(bp0, bp1, bp2); // Flipped winding
    b->sides[1].shaderInfo = si;
    
    // 3, 4, 5. The edge planes 
    // Edge 1: p0 -> p1
    b->sides[2].planenum = MapPlaneFromPoints(p0, p1, bp0);
    b->sides[2].shaderInfo = si;
    
    // Edge 2: p1 -> p2
    b->sides[3].planenum = MapPlaneFromPoints(p1, p2, bp1);
    b->sides[3].shaderInfo = si;
    
    // Edge 3: p2 -> p0
    b->sides[4].planenum = MapPlaneFromPoints(p2, p0, bp2);
    b->sides[4].shaderInfo = si;
    
    // Check if any plane failed to generate
    if (b->sides[0].planenum == -1 || b->sides[1].planenum == -1 ||
        b->sides[2].planenum == -1 || b->sides[3].planenum == -1 || b->sides[4].planenum == -1) {
      FreeBrush(b);
      continue;
    }

    if (!CreateBrushWindings(b)) {
      FreeBrush(b);
      continue;
    }
    
    if (!BoundBrush(b)) {
      FreeBrush(b);
      continue;
    }
    
    b->next = hulls_list;
    hulls_list = b;
  }
  
  return hulls_list;
}

/*
====================
GenerateMOCollision

Processes a model instance, optimizes its meshes via meshoptimizer,
and passes the geometry to an extrusion helper.
====================
*/
bspbrush_t *GenerateMOCollision(modelInstance_t *inst, modelCategory_t category, shaderInfo_t *shader) {
  bspbrush_t *hulls_list = NULL;
  int j, k;
  mapDrawSurface_t *ds;
  
  _printf("Instance %s: Running MeshOptimizer Extrusion (MC_OBJECT)\n", inst->modelName);
  
  for (j = 0; j < inst->numDrawSurfs; j++) {
    ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
      continue;
    }

    if (ds->numVerts == 0 || ds->numIndexes == 0) {
      continue;
    }
    
    // Extract vertices for optimization
    float *meshVerts = malloc(ds->numVerts * 3 * sizeof(float));
    unsigned int *meshIndexes = malloc(ds->numIndexes * sizeof(unsigned int));
    
    for (k = 0; k < ds->numVerts; k++) {
      meshVerts[k * 3 + 0] = (float)ds->verts[k].xyz[0];
      meshVerts[k * 3 + 1] = (float)ds->verts[k].xyz[1];
      meshVerts[k * 3 + 2] = (float)ds->verts[k].xyz[2];
    }
    for (k = 0; k < ds->numIndexes; k++) {
      meshIndexes[k] = (unsigned int)ds->indexes[k];
    }

    // 1. Simplify the mesh to reduce triangle count
    float optimization_target = 0.02f; // User-adjustable baseline: 2% of original indices
    
    size_t target_index_count = (size_t)(ds->numIndexes * optimization_target);
    // Allow up to 5% deformation
    float target_error = 0.05f;
    
    unsigned int *simplifiedIndexes = malloc(ds->numIndexes * sizeof(unsigned int));
    size_t simplifiedIndexCount = meshopt_simplify(
        simplifiedIndexes, 
        meshIndexes, ds->numIndexes, 
        meshVerts, ds->numVerts, 
        sizeof(float) * 3, 
        target_index_count, 
        target_error, 
        0, NULL
    );
    
    _printf("Instance %s: Simplified from %i to %zu indices\n", inst->modelName, ds->numIndexes, simplifiedIndexCount);

    // 2. Optimize Vertex Cache for the simplified mesh
    // meshoptimizer requires the destination buffer to be pre-allocated.
    unsigned int *optimizedIndexes = malloc(simplifiedIndexCount * sizeof(unsigned int));
    meshopt_optimizeVertexCache(optimizedIndexes, simplifiedIndexes, simplifiedIndexCount, ds->numVerts);
    
    // 3. Delegate Extrusion
    bspbrush_t *surfBrushes = ExtrudeTrianglesToBrushes((double*)meshVerts, optimizedIndexes, (int)simplifiedIndexCount, shader);
    
    // Append to main list
    if (surfBrushes) {
      bspbrush_t *tail = surfBrushes;
      while (tail->next) {
        tail = tail->next;
      }
      tail->next = hulls_list;
      hulls_list = surfBrushes;
    }
    
    free(meshVerts);
    free(meshIndexes);
    free(simplifiedIndexes);
    free(optimizedIndexes);
  }
  
  return hulls_list;
}
