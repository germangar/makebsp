#include "qbsp.h"
#include "model_collision.h"

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

    bspbrush_t *b = AllocBrush(5 + 6);
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

    AddBevelsToBrush(b);

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
    // _printf("    WARNING: ExtrudePolygon failed (front plane degenerate, %d verts)\n", N);
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
    // _printf("    WARNING: ExtrudePolygon failed (back plane degenerate, %d verts)\n", N);
    free(pts);
    free(bpts);
    return NULL;
  }
  
  /* Allocate brush, plus space for up to 6 bevel planes */
  bspbrush_t *b = AllocBrush(numSides + 6);
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
    // _printf("    WARNING: ExtrudePolygon failed (edge plane degenerate, %d verts)\n", N);
    FreeBrush(b);
    return NULL;
  }
  
  if (!CreateBrushWindings(b)) {
    // _printf("    WARNING: ExtrudePolygon failed (CreateBrushWindings, %d verts)\n", N);
    FreeBrush(b);
    return NULL;
  }
  
  if (!BoundBrush(b)) {
    // _printf("    WARNING: ExtrudePolygon failed (BoundBrush, %d verts)\n", N);
    FreeBrush(b);
    return NULL;
  }
  
  AddBevelsToBrush(b);

  return b;
}

/*
====================
ExtrudeFanToBrush

Diamond/bipyramid approach: the hub vertex is the front tip, and a
computed back point is the rear tip. The ring vertices form the equator.
  - N front faces: each original fan triangle (hub, ring[i], ring[i+1])
  - N back faces: each from back point to ring edge (backPt, ring[i+1], ring[i])
  Total: 2N sides.

The back point is placed along the hub→centroid axis, at a distance
equal to the farthest ring vertex from the hub. This ensures the
brush is geometrically bounded and never spikes.
====================
*/
static bspbrush_t *ExtrudeFanToBrush(int hubIdx, int *ringVerts, int ringCount,
                                      clipTri_t *tris, int *fanTriIndices, int fanTriCount,
                                      float *verts, float extrudeDist, shaderInfo_t *si) {
  int N = fanTriCount;
  int numSides = 2 * N;  /* N front faces + N back faces */
  
  /*
  if (numSides > MAX_BRUSH_SIDES) {
    _printf("    WARNING: ExtrudeFan failed (numSides %d > MAX_BRUSH_SIDES)\n", numSides);
    return NULL;
  }
  */
  if (numSides > MAX_BRUSH_SIDES) return NULL;
  
  /* Get hub position */
  vec3_t hub;
  hub[0] = verts[hubIdx * 3 + 0];
  hub[1] = verts[hubIdx * 3 + 1];
  hub[2] = verts[hubIdx * 3 + 2];
  
  /* Compute ring centroid (average of non-hub vertices) */
  vec3_t centroid = {0, 0, 0};
  for (int i = 0; i < ringCount; i++) {
    centroid[0] += verts[ringVerts[i] * 3 + 0];
    centroid[1] += verts[ringVerts[i] * 3 + 1];
    centroid[2] += verts[ringVerts[i] * 3 + 2];
  }
  centroid[0] /= ringCount;
  centroid[1] /= ringCount;
  centroid[2] /= ringCount;
  
  /* Axis direction: hub → centroid */
  vec3_t axis;
  VectorSubtract(centroid, hub, axis);
  float axisLen = VectorNormalize(axis, axis);
  if (axisLen < 0.001f) {
    // _printf("    WARNING: ExtrudeFan failed (axis too short, hub=%d)\n", hubIdx);
    return NULL;
  }
  
  /* Compute max ring radius (distance from centroid to farthest ring vertex) */
  float maxRadius = 0;
  for (int i = 0; i < ringCount; i++) {
    vec3_t rv, diff;
    rv[0] = verts[ringVerts[i] * 3 + 0];
    rv[1] = verts[ringVerts[i] * 3 + 1];
    rv[2] = verts[ringVerts[i] * 3 + 2];
    VectorSubtract(rv, centroid, diff);
    float r = VectorLength(diff);
    if (r > maxRadius) maxRadius = r;
  }
  
  /* Reject flat fans: hub must protrude at least 30% of ring radius.
     Flat fans (like vertices on plank sides) produce thin diamonds that spike. */
  if (maxRadius > 0.001f && axisLen < maxRadius * 0.3f) {
    // _printf("    WARNING: ExtrudeFan rejected (flat fan: axisLen=%.3f, radius=%.3f, hub=%d, ring=%d)\n", axisLen, maxRadius, hubIdx, ringCount);
    return NULL;
  }
  
  /* Find the maximum projection of ring vertices onto the axis (from hub).
     This gives the actual "depth" of the fan, not the Euclidean spread.
     Using Euclidean distance caused huge diamonds on flat fans. */
  float maxProj = 0;
  for (int i = 0; i < ringCount; i++) {
    vec3_t rv, diff;
    rv[0] = verts[ringVerts[i] * 3 + 0];
    rv[1] = verts[ringVerts[i] * 3 + 1];
    rv[2] = verts[ringVerts[i] * 3 + 2];
    VectorSubtract(rv, hub, diff);
    float proj = DotProduct(diff, axis);
    if (proj > maxProj) maxProj = proj;
  }
  
  /* Place back point along axis, just past the deepest ring vertex */
  vec3_t backPoint;
  VectorMA(hub, maxProj + extrudeDist, axis, backPoint);
  
  /* Allocate brush, plus space for up to 6 bevel planes */
  bspbrush_t *b = AllocBrush(numSides + 6);
  b->numsides = numSides;
  b->detail = qtrue;
  b->contents = si->contents;
  b->contentShader = si;
  
  qboolean planesOk = qtrue;
  
  /* Sides 0..N-1: front face planes (original fan triangles) */
  for (int i = 0; i < N; i++) {
    vec3_t p0, p1, p2;
    clipTri_t *tri = &tris[fanTriIndices[i]];
    p0[0] = verts[tri->indices[0] * 3 + 0]; p0[1] = verts[tri->indices[0] * 3 + 1]; p0[2] = verts[tri->indices[0] * 3 + 2];
    p1[0] = verts[tri->indices[1] * 3 + 0]; p1[1] = verts[tri->indices[1] * 3 + 1]; p1[2] = verts[tri->indices[1] * 3 + 2];
    p2[0] = verts[tri->indices[2] * 3 + 0]; p2[1] = verts[tri->indices[2] * 3 + 1]; p2[2] = verts[tri->indices[2] * 3 + 2];
    
    b->sides[i].planenum = MapPlaneFromPoints(p0, p1, p2);
    b->sides[i].shaderInfo = si;
    if (b->sides[i].planenum == -1)
      planesOk = qfalse;
  }
  
  /* Sides N..2N-1: back face planes (backPoint to each ring edge, reversed winding) */
  for (int i = 0; i < N; i++) {
    int next = (i + 1) % ringCount;
    vec3_t r0, r1;
    r0[0] = verts[ringVerts[i] * 3 + 0]; r0[1] = verts[ringVerts[i] * 3 + 1]; r0[2] = verts[ringVerts[i] * 3 + 2];
    r1[0] = verts[ringVerts[next] * 3 + 0]; r1[1] = verts[ringVerts[next] * 3 + 1]; r1[2] = verts[ringVerts[next] * 3 + 2];
    
    /* Winding: backPoint, r1, r0 — normal faces inward toward hub */
    b->sides[N + i].planenum = MapPlaneFromPoints(backPoint, r1, r0);
    b->sides[N + i].shaderInfo = si;
    if (b->sides[N + i].planenum == -1)
      planesOk = qfalse;
  }
  
  if (!planesOk) {
    // _printf("    WARNING: ExtrudeFan failed (plane degenerate, hub=%d, ring=%d)\n", hubIdx, ringCount);
    FreeBrush(b);
    return NULL;
  }
  
  if (!CreateBrushWindings(b)) {
    // _printf("    WARNING: ExtrudeFan failed (CreateBrushWindings, hub=%d, ring=%d)\n", hubIdx, ringCount);
    FreeBrush(b);
    return NULL;
  }
  
  if (!BoundBrush(b)) {
    // _printf("    WARNING: ExtrudeFan failed (BoundBrush, hub=%d, ring=%d)\n", hubIdx, ringCount);
    FreeBrush(b);
    return NULL;
  }
  
  AddBevelsToBrush(b);

  return b;
}

/*
====================
OrderFanRing

Given a set of triangle indices forming a fan around hubIdx,
orders the ring vertices so they form a contiguous closed chain.
Does NOT require all triangles to be part of the ring — finds the
largest closed chain it can from the available triangles.
Returns qtrue and fills ringVerts/orderedTriIdx if a ring of 3+ is found.
*pFanTriCount is updated to the actual number of triangles in the ring.
====================
*/
static qboolean OrderFanRing(int hubIdx, clipTri_t *tris, 
                              int *fanTriIdx, int *pFanTriCount, float *verts,
                              int *ringVerts, int *orderedTriIdx) {
  int fanTriCount = *pFanTriCount;
  if (fanTriCount < 3)
    return qfalse;
  
  /* For each triangle in the fan, find its two non-hub vertices (the "edge") */
  typedef struct { int v0, v1; int triIdx; qboolean used; } fanEdge_t;
  fanEdge_t *edges = malloc(fanTriCount * sizeof(fanEdge_t));
  
  for (int i = 0; i < fanTriCount; i++) {
    clipTri_t *tri = &tris[fanTriIdx[i]];
    int nonHub[2];
    int nh = 0;
    for (int j = 0; j < 3; j++) {
      if (PointsMatch(verts, tri->indices[j], hubIdx)) continue;
      if (nh < 2) nonHub[nh++] = tri->indices[j];
    }
    if (nh != 2) { free(edges); return qfalse; }
    edges[i].v0 = nonHub[0];
    edges[i].v1 = nonHub[1];
    edges[i].triIdx = fanTriIdx[i];
    edges[i].used = qfalse;
  }
  
  /* Try starting from each edge to find the best closed ring */
  int bestRingCount = 0;
  int bestOrderedCount = 0;
  int *bestRing = malloc(MAX_POLY_VERTS * sizeof(int));
  int *bestOrdered = malloc(MAX_POLY_VERTS * sizeof(int));
  
  for (int start = 0; start < fanTriCount; start++) {
    /* Reset used flags */
    for (int i = 0; i < fanTriCount; i++)
      edges[i].used = qfalse;
    
    edges[start].used = qtrue;
    int tempRing[MAX_POLY_VERTS];
    int tempOrdered[MAX_POLY_VERTS];
    tempRing[0] = edges[start].v0;
    tempRing[1] = edges[start].v1;
    tempOrdered[0] = edges[start].triIdx;
    int rc = 2;
    int oc = 1;
    
    /* Chain forward */
    for (int step = 1; step < fanTriCount; step++) {
      int lastVert = tempRing[rc - 1];
      qboolean found = qfalse;
      for (int e = 0; e < fanTriCount; e++) {
        if (edges[e].used) continue;
        
        int nextVert = -1;
        if (PointsMatch(verts, edges[e].v0, lastVert))
          nextVert = edges[e].v1;
        else if (PointsMatch(verts, edges[e].v1, lastVert))
          nextVert = edges[e].v0;
        
        if (nextVert >= 0) {
          edges[e].used = qtrue;
          tempOrdered[oc++] = edges[e].triIdx;
          
          /* Check if it closes the loop */
          if (PointsMatch(verts, nextVert, tempRing[0])) {
            /* Closed ring found! */
            if (oc >= 3 && oc > bestOrderedCount) {
              bestRingCount = rc;
              bestOrderedCount = oc;
              for (int k = 0; k < rc; k++) bestRing[k] = tempRing[k];
              for (int k = 0; k < oc; k++) bestOrdered[k] = tempOrdered[k];
            }
            found = qtrue;
            break;
          }
          
          if (rc < MAX_POLY_VERTS)
            tempRing[rc++] = nextVert;
          found = qtrue;
          break;
        }
      }
      if (!found) break;
    }
  }
  
  free(edges);
  
  if (bestOrderedCount < 3) {
    free(bestRing);
    free(bestOrdered);
    return qfalse;
  }
  
  /* Copy best result */
  for (int i = 0; i < bestRingCount; i++) ringVerts[i] = bestRing[i];
  for (int i = 0; i < bestOrderedCount; i++) orderedTriIdx[i] = bestOrdered[i];
  *pFanTriCount = bestOrderedCount;
  
  free(bestRing);
  free(bestOrdered);
  return qtrue;
}

/*
====================
ValidateFanConvexity

Checks that a triangle fan forms a convex shape:
- The hub vertex must be on the "outward" side of the base ring polygon
- All fan face normals must point generally in the same direction
====================
*/
static qboolean ValidateFanConvexity(int hubIdx, int *ringVerts, int ringCount,
                                      clipTri_t *tris, int *fanTriIdx, int fanTriCount,
                                      float *verts) {
  /* With diamond/bipyramid shape, no angle constraint needed.
     Just compute avgNormal for the hub-outward check below. */
  vec3_t avgNormal = {0, 0, 0};
  for (int i = 0; i < fanTriCount; i++) {
    VectorAdd(avgNormal, tris[fanTriIdx[i]].normal, avgNormal);
  }
  if (VectorNormalize(avgNormal, avgNormal) < 0.0001f)
    return qfalse;
  
  /* The hub must be on the outward side of the ring plane.
     Compute the ring centroid and check that hub is further along avgNormal. */
  vec3_t ringCenter = {0, 0, 0};
  for (int i = 0; i < ringCount; i++) {
    ringCenter[0] += verts[ringVerts[i] * 3 + 0];
    ringCenter[1] += verts[ringVerts[i] * 3 + 1];
    ringCenter[2] += verts[ringVerts[i] * 3 + 2];
  }
  ringCenter[0] /= ringCount;
  ringCenter[1] /= ringCount;
  ringCenter[2] /= ringCount;
  
  vec3_t hubPos;
  hubPos[0] = verts[hubIdx * 3 + 0];
  hubPos[1] = verts[hubIdx * 3 + 1];
  hubPos[2] = verts[hubIdx * 3 + 2];
  
  vec3_t toHub;
  VectorSubtract(hubPos, ringCenter, toHub);
  float hubDot = DotProduct(toHub, avgNormal);
  
  /* Hub should be in front of (or near) the ring centroid */
  if (hubDot < -0.01f)
    return qfalse;
  
  return qtrue;
}


/*
====================
ExtrudeTrianglesToBrushes

Optimized version with two merge passes:
1. Triangle fan detection — fans of N triangles sharing a hub vertex
   become single N+1-sided pyramidal brushes
2. Coplanar merging — remaining coplanar adjacent triangles are merged
   into larger polygons
3. Individual extrusion — any remaining triangles extruded as 5-sided brushes
====================
*/
bspbrush_t *ExtrudeTrianglesToBrushes(colMesh_t *mesh, shaderInfo_t *si) {
  bspbrush_t *hulls_list = NULL;
  float *verts = (float *)mesh->verts;
  unsigned int *indices = (unsigned int *)mesh->tris;
  int numIndices = mesh->numTris * 3;
  int numTris = mesh->numTris;
  
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
  
  /* 1.5 Fan detection: build vertex→triangle adjacency and detect fans */
  
  /* Find max vertex index to size the adjacency arrays */
  int maxVertIdx = 0;
  for (int i = 0; i < numTris; i++) {
    for (int j = 0; j < 3; j++) {
      if (tris[i].indices[j] > maxVertIdx)
        maxVertIdx = tris[i].indices[j];
    }
  }
  maxVertIdx++;
  
  /* Count triangles per vertex */
  int *vertTriCount = calloc(maxVertIdx, sizeof(int));
  int *vertTriOffset = calloc(maxVertIdx, sizeof(int));
  for (int i = 0; i < numTris; i++) {
    if (tris[i].merged) continue;
    for (int j = 0; j < 3; j++)
      vertTriCount[tris[i].indices[j]]++;
  }
  
  /* Build offset table (prefix sum) */
  int totalAdj = 0;
  for (int v = 0; v < maxVertIdx; v++) {
    vertTriOffset[v] = totalAdj;
    totalAdj += vertTriCount[v];
  }
  
  /* Fill adjacency list */
  int *adjTriangles = malloc(totalAdj * sizeof(int));
  int *fillPos = calloc(maxVertIdx, sizeof(int));
  for (int i = 0; i < numTris; i++) {
    if (tris[i].merged) continue;
    for (int j = 0; j < 3; j++) {
      int v = tris[i].indices[j];
      adjTriangles[vertTriOffset[v] + fillPos[v]] = i;
      fillPos[v]++;
    }
  }
  free(fillPos);
  
  /* Try to form fans around vertices with 3+ adjacent triangles */
  int fanBrushCount = 0;
  int fanTriConsumed = 0;
  float extrudeDist = 0.5f;
  
  int *ringVerts = malloc(MAX_POLY_VERTS * sizeof(int));
  int *orderedTriIdx = malloc(MAX_POLY_VERTS * sizeof(int));
  
  for (int v = 0; v < maxVertIdx; v++) {
    if (vertTriCount[v] < 3)
      continue;
    
    /* Collect un-merged triangles adjacent to this vertex */
    int fanCount = 0;
    int fanTriIdx[MAX_POLY_VERTS];
    for (int a = 0; a < vertTriCount[v] && fanCount < MAX_POLY_VERTS; a++) {
      int ti = adjTriangles[vertTriOffset[v] + a];
      if (!tris[ti].merged)
        fanTriIdx[fanCount++] = ti;
    }
    
    if (fanCount < 3)
      continue;
    
    /* Try to order the ring vertices into a contiguous chain */
    if (!OrderFanRing(v, tris, fanTriIdx, &fanCount, verts, ringVerts, orderedTriIdx))
      continue;
    
    /* Validate that the fan is convex */
    if (!ValidateFanConvexity(v, ringVerts, fanCount, tris, orderedTriIdx, fanCount, verts))
      continue;
    
    /* Extrude the fan as a single pyramidal brush */
    bspbrush_t *b = ExtrudeFanToBrush(v, ringVerts, fanCount, tris, orderedTriIdx, fanCount,
                                       verts, extrudeDist, si);
    if (b) {
      b->next = hulls_list;
      hulls_list = b;
      fanBrushCount++;
      
      /* Mark all fan triangles as consumed */
      for (int f = 0; f < fanCount; f++) {
        tris[orderedTriIdx[f]].merged = qtrue;
        fanTriConsumed++;
      }
    }
  }
  
  free(ringVerts);
  free(orderedTriIdx);
  free(vertTriCount);
  free(vertTriOffset);
  free(adjTriangles);
  
  /*
  if (fanBrushCount > 0) {
    _printf("  Fan merge: %d triangles -> %d diamond brushes\n", 
            fanTriConsumed, fanBrushCount);
  }
  */
  
  /* 2. Merge coplanar adjacent triangles into polygons (remaining non-fan tris) */
  int remaining = 0;
  for (int i = 0; i < numTris; i++) {
    if (!tris[i].merged) remaining++;
  }
  
  int maxPolys = remaining > 0 ? remaining : 1;
  clipPoly_t *polys = calloc(maxPolys, sizeof(clipPoly_t));
  int numPolys = 0;
  
  for (int i = 0; i < numTris; i++) {
    if (tris[i].merged)
      continue;
    
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
  /*
  if (mergedCount > 0) {
    _printf("  Coplanar merge: %d remaining -> %d polygons (%d merged)\n", 
            remaining, numPolys, mergedCount);
  }
  */
  
  /* 3. Extrude each remaining polygon into a brush */
  int failedPolys = 0;
  for (int i = 0; i < numPolys; i++) {
    bspbrush_t *b = ExtrudePolygonToBrush(&polys[i], verts, extrudeDist, si);
    if (b) {
      b->next = hulls_list;
      hulls_list = b;
    } else {
      failedPolys++;
    }
  }

  
  free(tris);
  free(polys);
  
  return hulls_list;
}


/*
====================
CombineBrushes
====================
*/
bspbrush_t *CombineBrushes(bspbrush_t *list, bspbrush_t *newBrushes) {
  if (!newBrushes) return list;
  if (!list) return newBrushes;
  bspbrush_t *tail = newBrushes;
  while (tail->next) tail = tail->next;
  tail->next = list;
  return newBrushes;
}



/*
====================
GenerateExtrusionCollision

Processes a model instance using its pre-extracted, decimated meshes,
and passes the geometry generically to the extrusion helper without caring about origin.
====================
*/
bspbrush_t *GenerateExtrusionCollision(modelInstance_t *inst, shaderInfo_t *shader) {
  bspbrush_t *hulls_list = NULL;

  _printf("Instance %s: Running Generic Extrusion (%s)\n", inst->modelName, CategoryString(inst->category));

  if (inst->num_collision_meshes == 0) {
    return NULL;
  }

  for (int j = 0; j < inst->num_collision_meshes; j++) {
    colMesh_t *colMesh = inst->collision_meshes[j];
    if (!colMesh || colMesh->numTris == 0) continue;

    _printf("Mesh #%d\n", j);

    /* 3. Extrude using the shared function */
    shaderInfo_t *si = (shader != NULL) ? shader : NULL; // Uses caulk if shader passed, otherwise wait for fix below

    // We lost per-surface shader info by pre-extracting them generically.
    // For now we will just use the default shader passed in (usually caulk).
    bspbrush_t *surfBrushes = ExtrudeTrianglesToBrushes(colMesh, si);

    /* Append to main list */
    hulls_list = CombineBrushes(hulls_list, surfBrushes);
  }

  return hulls_list;
}

