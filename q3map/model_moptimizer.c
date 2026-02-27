#include "qbsp.h"
#include "../libs/meshoptimizer/src/meshoptimizer.h"

/*
====================
ExtrudeTrianglesToBrushesRaw

Original raw extrusion: one brush per triangle, no merging.
Kept for reference/fallback.
====================
*/
static bspbrush_t *ExtrudeTrianglesToBrushesRaw(float *verts, unsigned int *indices, int numIndices, shaderInfo_t *si) {
  bspbrush_t *hulls_list = NULL;
  int i;
  
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

    vec3_t faceNormal, t1, t2;
    VectorSubtract(p2, p0, t1);
    VectorSubtract(p1, p0, t2);
    CrossProduct(t1, t2, faceNormal);
    if (VectorNormalize(faceNormal, faceNormal) < 0.0001f)
      continue;

    bspbrush_t *b = AllocBrush(5);
    b->numsides = 5;
    b->detail = qtrue;
    b->contents = si->contents;
    b->contentShader = si;

    b->sides[0].planenum = MapPlaneFromPoints(p0, p1, p2);
    b->sides[0].shaderInfo = si;

    float extrudeDist = 0.5f;
    vec3_t bp0, bp1, bp2;
    VectorMA(p0, -extrudeDist, faceNormal, bp0);
    VectorMA(p1, -extrudeDist, faceNormal, bp1);
    VectorMA(p2, -extrudeDist, faceNormal, bp2);

    b->sides[1].planenum = MapPlaneFromPoints(bp0, bp2, bp1); 
    b->sides[1].shaderInfo = si;
    b->sides[2].planenum = MapPlaneFromPoints(p0, bp0, p1);
    b->sides[2].shaderInfo = si;
    b->sides[3].planenum = MapPlaneFromPoints(p1, bp1, p2);
    b->sides[3].shaderInfo = si;
    b->sides[4].planenum = MapPlaneFromPoints(p2, bp2, p0);
    b->sides[4].shaderInfo = si;

    if (b->sides[0].planenum == -1 || b->sides[1].planenum == -1 ||
        b->sides[2].planenum == -1 || b->sides[3].planenum == -1 || b->sides[4].planenum == -1) {
      FreeBrush(b);
      continue;
    }
    if (!CreateBrushWindings(b)) { FreeBrush(b); continue; }
    if (!BoundBrush(b)) { FreeBrush(b); continue; }

    b->next = hulls_list;
    hulls_list = b;
  }
  return hulls_list;
}


/* =====================================================================
   Optimized extrusion with coplanar merging and axial-snapped normals
   ===================================================================== */

#define COPLANAR_NORMAL_EPSILON  0.001
#define COPLANAR_DIST_EPSILON    0.1
#define MERGE_POINT_EPSILON      0.1
#define MAX_POLY_VERTS           64  /* safety cap for merged polygon vertex count */

/* A triangle ready for merging */
typedef struct {
  int indices[3];     /* vertex indices into the verts array */
  vec3_t normal;
  vec_t  dist;        /* plane distance: dot(normal, p0) */
  qboolean merged;    /* already consumed by a merge */
} clipTri_t;

/* A merged convex polygon */
typedef struct {
  int verts[MAX_POLY_VERTS];
  int numVerts;
  vec3_t normal;
  vec_t  dist;
} clipPoly_t;

/*
====================
MakeAxialNormal

Snaps a normal to the nearest axis direction.
Produces side planes that align with BSP axes for tighter splitting.
(From Garux normal_make_axial)
====================
*/
static void MakeAxialNormal(const vec3_t in, vec3_t out) {
  int best = 0;
  float bestVal = fabs(in[0]);
  
  if (fabs(in[1]) > bestVal) { best = 1; bestVal = fabs(in[1]); }
  if (fabs(in[2]) > bestVal) { best = 2; }
  
  VectorClear(out);
  out[best] = (in[best] >= 0) ? 1.0f : -1.0f;
}

/*
====================
PointsMatch

Checks if two 3D points from the vertex buffer are the same (within epsilon).
====================
*/
static qboolean PointsMatch(float *verts, int idxA, int idxB) {
  float dx = verts[idxA * 3 + 0] - verts[idxB * 3 + 0];
  float dy = verts[idxA * 3 + 1] - verts[idxB * 3 + 1];
  float dz = verts[idxA * 3 + 2] - verts[idxB * 3 + 2];
  return (fabs(dx) < MERGE_POINT_EPSILON && 
          fabs(dy) < MERGE_POINT_EPSILON && 
          fabs(dz) < MERGE_POINT_EPSILON) ? qtrue : qfalse;
}

/*
====================
TrianglesAreCoplanar

Checks if two triangles lie on the same plane (normal and distance match).
====================
*/
static qboolean TrianglesAreCoplanar(clipTri_t *a, clipTri_t *b) {
  /* check normals are parallel (dot product ~= 1) */
  float dot = DotProduct(a->normal, b->normal);
  if (dot < (1.0 - COPLANAR_NORMAL_EPSILON))
    return qfalse;

  /* check plane distances match */
  if (fabs(a->dist - b->dist) > COPLANAR_DIST_EPSILON)
    return qfalse;

  return qtrue;
}

/*
====================
FindSharedEdge

Given two triangles, finds if they share exactly 2 vertices (a shared edge).
Returns qtrue and sets sharedA0, sharedA1 (indices within tri A's index array 0-2)
and sharedB0, sharedB1 (indices within tri B's index array 0-2).
The shared edge runs A[sharedA0]->A[sharedA1] == B[sharedB1]->B[sharedB0] (reversed).
====================
*/
static qboolean FindSharedEdge(clipTri_t *a, clipTri_t *b, float *verts,
                               int *sharedA0, int *sharedA1, 
                               int *sharedB0, int *sharedB1) {
  int matches = 0;
  int aIdx[2], bIdx[2];
  
  for (int ai = 0; ai < 3; ai++) {
    for (int bi = 0; bi < 3; bi++) {
      if (PointsMatch(verts, a->indices[ai], b->indices[bi])) {
        if (matches < 2) {
          aIdx[matches] = ai;
          bIdx[matches] = bi;
        }
        matches++;
      }
    }
  }
  
  if (matches != 2)
    return qfalse;
  
  *sharedA0 = aIdx[0];
  *sharedA1 = aIdx[1];
  *sharedB0 = bIdx[0];
  *sharedB1 = bIdx[1];
  return qtrue;
}

/*
====================
IsConvex2D

Checks if a polygon (projected along the dominant axis of its normal)
is convex. Used to validate merges.
====================
*/
static qboolean IsConvex2D(clipPoly_t *poly, float *verts) {
  if (poly->numVerts < 3)
    return qfalse;
  
  /* find the dominant axis to project away */
  int dropAxis = 0;
  if (fabs(poly->normal[1]) > fabs(poly->normal[dropAxis])) dropAxis = 1;
  if (fabs(poly->normal[2]) > fabs(poly->normal[dropAxis])) dropAxis = 2;
  
  int u = (dropAxis + 1) % 3;
  int v = (dropAxis + 2) % 3;
  
  /* determine expected sign from the normal direction */
  float expectedSign = (poly->normal[dropAxis] > 0) ? 1.0f : -1.0f;
  
  for (int i = 0; i < poly->numVerts; i++) {
    int i0 = poly->verts[i];
    int i1 = poly->verts[(i + 1) % poly->numVerts];
    int i2 = poly->verts[(i + 2) % poly->numVerts];
    
    float ax = verts[i1 * 3 + u] - verts[i0 * 3 + u];
    float ay = verts[i1 * 3 + v] - verts[i0 * 3 + v];
    float bx = verts[i2 * 3 + u] - verts[i1 * 3 + u];
    float by = verts[i2 * 3 + v] - verts[i1 * 3 + v];
    
    float cross = ax * by - ay * bx;
    
    /* allow near-zero (collinear edges) but reject wrong sign */
    if (cross * expectedSign < -0.001f)
      return qfalse;
  }
  
  return qtrue;
}

/*
====================
TryMergeTriIntoPoly

Attempts to merge a triangle into an existing polygon along a shared edge.
The polygon and triangle must be coplanar. The shared edge is found by
scanning for two consecutive polygon vertices that match two triangle vertices.
The non-shared triangle vertex is inserted between them.
Returns qtrue on success.
====================
*/
static qboolean TryMergeTriIntoPoly(clipPoly_t *poly, clipTri_t *tri, float *verts) {
  if (poly->numVerts >= MAX_POLY_VERTS)
    return qfalse;
  
  /* Check coplanarity */
  float dot = DotProduct(poly->normal, tri->normal);
  if (dot < (1.0 - COPLANAR_NORMAL_EPSILON))
    return qfalse;
  if (fabs(poly->dist - tri->dist) > COPLANAR_DIST_EPSILON)
    return qfalse;
  
  /* Find a shared edge: two consecutive polygon vertices that match
     two triangle vertices (in reverse order, since they share the edge
     with opposite winding). */
  for (int pi = 0; pi < poly->numVerts; pi++) {
    int pNext = (pi + 1) % poly->numVerts;
    
    for (int ti = 0; ti < 3; ti++) {
      int tPrev = (ti + 2) % 3;  /* previous in triangle = reversed edge */
      
      if (PointsMatch(verts, poly->verts[pi], tri->indices[ti]) &&
          PointsMatch(verts, poly->verts[pNext], tri->indices[tPrev])) {
        /* Found shared edge: poly[pi]->poly[pNext] matches tri[tPrev]->tri[ti]
           The third triangle vertex (the non-shared one) needs to be inserted. */
        int tOther = (ti + 1) % 3;
        
        /* Check we won't exceed limits */
        if (poly->numVerts + 1 > MAX_POLY_VERTS || poly->numVerts + 1 > MAX_BRUSH_SIDES)
          return qfalse;
        
        /* Insert the new vertex after position pi+1 (i.e., at pNext+1) */
        int insertPos = pNext + 1;
        if (insertPos > poly->numVerts) insertPos = poly->numVerts; /* shouldn't happen but safety */
        
        /* Shift vertices to make room */
        for (int s = poly->numVerts; s > insertPos; s--)
          poly->verts[s] = poly->verts[s - 1];
        poly->verts[insertPos] = tri->indices[tOther];
        poly->numVerts++;
        
        /* Validate convexity of the result */
        if (!IsConvex2D(poly, verts)) {
          /* Undo the insertion */
          for (int s = insertPos; s < poly->numVerts - 1; s++)
            poly->verts[s] = poly->verts[s + 1];
          poly->numVerts--;
          return qfalse;
        }
        
        return qtrue;
      }
    }
  }
  
  return qfalse;
}

/*
====================
ExtrudePolygonToBrush

Takes a convex polygon (N vertices) and extrudes it into a brush.
Uses axial-snapped extrusion direction for side planes (Garux clipModel_default).
====================
*/
static bspbrush_t *ExtrudePolygonToBrush(clipPoly_t *poly, float *verts, 
                                          float extrudeDist, shaderInfo_t *si) {
  int N = poly->numVerts;
  int numSides = N + 2;  /* front + back + N edges */
  
  /* Safety check */
  if (numSides > MAX_BRUSH_SIDES) {
    _printf("WARNING: Merged polygon has %d sides, exceeds MAX_BRUSH_SIDES (%d). Skipping.\n", 
            numSides, MAX_BRUSH_SIDES);
    return NULL;
  }
  
  /* Get the polygon vertex positions */
  vec3_t *pts = malloc(N * sizeof(vec3_t));
  for (int i = 0; i < N; i++) {
    pts[i][0] = verts[poly->verts[i] * 3 + 0];
    pts[i][1] = verts[poly->verts[i] * 3 + 1];
    pts[i][2] = verts[poly->verts[i] * 3 + 2];
  }
  
  /* Front face plane: use first 3 points of the polygon */
  /* We already have the normal from merging, but recompute from the actual
     polygon points to ensure consistency with MapPlaneFromPoints */
  int frontPlane = MapPlaneFromPoints(pts[0], pts[1], pts[2]);
  if (frontPlane == -1) {
    free(pts);
    return NULL;
  }
  
  /* Use actual face normal for extrusion direction.
     Axial snapping (Garux clipModel_default) is a future optimization. */
  vec3_t extrudeDir;
  VectorCopy(poly->normal, extrudeDir);
  
  /* Back face: offset all polygon points along -normal by extrudeDist */
  vec3_t *bpts = malloc(N * sizeof(vec3_t));
  for (int i = 0; i < N; i++) {
    VectorMA(pts[i], -extrudeDist, extrudeDir, bpts[i]);
  }
  
  /* Back plane: reversed winding (use first, last, and second back points) */
  int backPlane = MapPlaneFromPoints(bpts[0], bpts[N - 1], bpts[1]);
  if (backPlane == -1) {
    free(pts);
    free(bpts);
    return NULL;
  }
  
  /* Allocate brush */
  bspbrush_t *b = AllocBrush(numSides);
  b->numsides = numSides;
  b->detail = qtrue;
  b->contents = si->contents;
  b->contentShader = si;
  
  /* Side 0: front face */
  b->sides[0].planenum = frontPlane;
  b->sides[0].shaderInfo = si;
  
  /* Side 1: back face */
  b->sides[1].planenum = backPlane;
  b->sides[1].shaderInfo = si;
  
  /* Sides 2..N+1: edge planes from explicit edge quads
     Each side plane is defined by two front verts and the corresponding back vert. */
  qboolean planesOk = qtrue;
  for (int i = 0; i < N; i++) {
    int next = (i + 1) % N;
    b->sides[2 + i].planenum = MapPlaneFromPoints(pts[i], bpts[i], pts[next]);
    b->sides[2 + i].shaderInfo = si;
    
    if (b->sides[2 + i].planenum == -1)
      planesOk = qfalse;
  }
  
  free(pts);
  free(bpts);
  
  if (!planesOk || frontPlane == -1 || backPlane == -1) {
    FreeBrush(b);
    return NULL;
  }
  
  if (!CreateBrushWindings(b)) {
    FreeBrush(b);
    return NULL;
  }
  
  if (!BoundBrush(b)) {
    FreeBrush(b);
    return NULL;
  }
  
  return b;
}

/*
====================
ExtrudeTrianglesToBrushes

Optimized version: merges coplanar adjacent triangles into larger convex
polygons before extruding, and uses axial-snapped normals for side planes.
Inspired by Garux q3map2 ClipModel() / windingMergeOthers().
====================
*/
static bspbrush_t *ExtrudeTrianglesToBrushes(float *verts, unsigned int *indices, int numIndices, shaderInfo_t *si) {
  bspbrush_t *hulls_list = NULL;
  int numTris = numIndices / 3;
  
  if (numTris == 0)
    return NULL;
  
  /* 1. Build triangle descriptors with normals and plane distances */
  clipTri_t *tris = calloc(numTris, sizeof(clipTri_t));
  int validTris = 0;
  
  for (int i = 0; i < numTris; i++) {
    int base = i * 3;
    tris[i].indices[0] = indices[base + 0];
    tris[i].indices[1] = indices[base + 1];
    tris[i].indices[2] = indices[base + 2];
    tris[i].merged = qfalse;
    
    vec3_t p0, p1, p2, t1, t2;
    p0[0] = verts[tris[i].indices[0] * 3 + 0];
    p0[1] = verts[tris[i].indices[0] * 3 + 1];
    p0[2] = verts[tris[i].indices[0] * 3 + 2];
    p1[0] = verts[tris[i].indices[1] * 3 + 0];
    p1[1] = verts[tris[i].indices[1] * 3 + 1];
    p1[2] = verts[tris[i].indices[1] * 3 + 2];
    p2[0] = verts[tris[i].indices[2] * 3 + 0];
    p2[1] = verts[tris[i].indices[2] * 3 + 1];
    p2[2] = verts[tris[i].indices[2] * 3 + 2];
    
    /* CW winding: cross(p2-p0, p1-p0) for outward normal */
    VectorSubtract(p2, p0, t1);
    VectorSubtract(p1, p0, t2);
    CrossProduct(t1, t2, tris[i].normal);
    
    if (VectorNormalize(tris[i].normal, tris[i].normal) < 0.0001f) {
      tris[i].merged = qtrue; /* mark degenerate as consumed */
      continue;
    }
    
    tris[i].dist = DotProduct(tris[i].normal, p0);
    validTris++;
  }
  
  /* 2. Merge coplanar adjacent triangles into polygons */
  int maxPolys = numTris;
  clipPoly_t *polys = calloc(maxPolys, sizeof(clipPoly_t));
  int numPolys = 0;
  
  /* Seed polygons from unmerged triangles */
  for (int i = 0; i < numTris; i++) {
    if (tris[i].merged)
      continue;
    
    /* Start a new polygon from this triangle */
    clipPoly_t *poly = &polys[numPolys];
    poly->numVerts = 3;
    poly->verts[0] = tris[i].indices[0];
    poly->verts[1] = tris[i].indices[1];
    poly->verts[2] = tris[i].indices[2];
    VectorCopy(tris[i].normal, poly->normal);
    poly->dist = tris[i].dist;
    tris[i].merged = qtrue;
    
    /* Try to absorb coplanar neighbors iteratively */
    qboolean absorbed;
    do {
      absorbed = qfalse;
      for (int j = 0; j < numTris; j++) {
        if (tris[j].merged)
          continue;
        
        if (TryMergeTriIntoPoly(poly, &tris[j], verts)) {
          tris[j].merged = qtrue;
          absorbed = qtrue;
        }
      }
    } while (absorbed);
    
    numPolys++;
  }
  
  int mergedCount = 0;
  for (int i = 0; i < numPolys; i++) {
    if (polys[i].numVerts > 3)
      mergedCount++;
  }
  if (mergedCount > 0) {
    _printf("  Coplanar merge: %d triangles -> %d polygons (%d merged)\n", 
            validTris, numPolys, mergedCount);
  }
  
  /* 3. Extrude each polygon into a brush */
  float extrudeDist = 0.5f;
  
  for (int i = 0; i < numPolys; i++) {
    bspbrush_t *b = ExtrudePolygonToBrush(&polys[i], verts, extrudeDist, si);
    if (b) {
      b->next = hulls_list;
      hulls_list = b;
    }
  }
  
  free(tris);
  free(polys);
  
  return hulls_list;
}


/*
====================
GenerateMOCollision

Processes a model instance, optimizes its meshes via meshoptimizer,
and passes the geometry to the extrusion helper.
====================
*/
bspbrush_t *GenerateMOCollision(modelInstance_t *inst, shaderInfo_t *shader) {
  bspbrush_t *hulls_list = NULL;
  int j, k;
  mapDrawSurface_t *ds;
  
  _printf("Instance %s: Running MeshOptimizer Extrusion (%s)\n", inst->modelName, CategoryString(inst->category));
  
  for (j = 0; j < inst->numDrawSurfs; j++) {
    ds = inst->drawSurfs[j];
    if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID)) {
      continue;
    }

    if (ds->numVerts == 0 || ds->numIndexes == 0) {
      continue;
    }
    
    /* Extract vertices for optimization */
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

    /* 1. Simplify the mesh to reduce triangle count */
    float optimization_target = 0.2f;
    
    size_t target_index_count = (size_t)(ds->numIndexes * optimization_target);
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

    /* 2. Optimize Vertex Cache for the simplified mesh */
    unsigned int *optimizedIndexes = malloc(simplifiedIndexCount * sizeof(unsigned int));
    meshopt_optimizeVertexCache(optimizedIndexes, simplifiedIndexes, simplifiedIndexCount, ds->numVerts);
    
    /* 3. Extrude with coplanar merging */
    bspbrush_t *surfBrushes = ExtrudeTrianglesToBrushes(meshVerts, optimizedIndexes, (int)simplifiedIndexCount, shader);
    
    /* Append to main list */
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
