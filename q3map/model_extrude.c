#include "qbsp.h"
#include "model_collision.h"

/* =====================================================================
   Optimized extrusion with coplanar merging and axial-snapped normals
   ===================================================================== */

#define COPLANAR_NORMAL_EPSILON 0.001
#define COPLANAR_DIST_EPSILON 0.1
#define MERGE_POINT_EPSILON 0.1
#define MAX_POLY_VERTS 64 /* safety cap for merged polygon vertex count */

/* A triangle ready for merging */
typedef struct
{
    int indices[3]; /* vertex indices into the verts array */
    vec3_t normal;
    vec_t dist;      /* plane distance: dot(normal, p0) */
    qboolean merged; /* already consumed by a merge */
} clipTri_t;

/* A merged convex polygon */
typedef struct
{
    int verts[MAX_POLY_VERTS];
    int numVerts;
    vec3_t normal;
    vec_t dist;
} clipPoly_t;

/*
====================
PointsMatch

Checks if two 3D points from the vertex buffer are the same (within epsilon).
====================
*/
static qboolean PointsMatch(float *verts, int idxA, int idxB)
{
    float dx = verts[idxA * 3 + 0] - verts[idxB * 3 + 0];
    float dy = verts[idxA * 3 + 1] - verts[idxB * 3 + 1];
    float dz = verts[idxA * 3 + 2] - verts[idxB * 3 + 2];
    return (fabs(dx) < MERGE_POINT_EPSILON &&
            fabs(dy) < MERGE_POINT_EPSILON &&
            fabs(dz) < MERGE_POINT_EPSILON)
               ? qtrue
               : qfalse;
}

/*
====================
IsConvex2D

Checks if a polygon (projected along the dominant axis of its normal)
is convex. Used to validate merges.
====================
*/
static qboolean IsConvex2D(clipPoly_t *poly, float *verts)
{
    if (poly->numVerts < 3)
        return qfalse;

    /* find the dominant axis to project away */
    int dropAxis = 0;
    if (fabs(poly->normal[1]) > fabs(poly->normal[dropAxis]))
        dropAxis = 1;
    if (fabs(poly->normal[2]) > fabs(poly->normal[dropAxis]))
        dropAxis = 2;

    int u = (dropAxis + 1) % 3;
    int v = (dropAxis + 2) % 3;

    /* determine expected sign from the normal direction */
    float expectedSign = (poly->normal[dropAxis] > 0) ? 1.0f : -1.0f;

    for (int i = 0; i < poly->numVerts; i++)
    {
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
static qboolean TryMergeTriIntoPoly(clipPoly_t *poly, clipTri_t *tri, float *verts)
{
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
    for (int pi = 0; pi < poly->numVerts; pi++)
    {
        int pNext = (pi + 1) % poly->numVerts;

        for (int ti = 0; ti < 3; ti++)
        {
            int tPrev = (ti + 2) % 3; /* previous in triangle = reversed edge */

            if (PointsMatch(verts, poly->verts[pi], tri->indices[ti]) &&
                PointsMatch(verts, poly->verts[pNext], tri->indices[tPrev]))
            {
                /* Found shared edge: poly[pi]->poly[pNext] matches tri[tPrev]->tri[ti]
                   The third triangle vertex (the non-shared one) needs to be inserted. */
                int tOther = (ti + 1) % 3;

                /* Check we won't exceed limits */
                if (poly->numVerts + 1 > MAX_POLY_VERTS || poly->numVerts + 1 > MAX_BRUSH_SIDES)
                    return qfalse;

                /* Insert the new vertex after position pi+1 (i.e., at pNext+1) */
                int insertPos = pNext + 1;
                if (insertPos > poly->numVerts)
                    insertPos = poly->numVerts; /* shouldn't happen but safety */

                /* Shift vertices to make room */
                for (int s = poly->numVerts; s > insertPos; s--)
                    poly->verts[s] = poly->verts[s - 1];
                poly->verts[insertPos] = tri->indices[tOther];
                poly->numVerts++;

                /* Validate convexity of the result */
                if (!IsConvex2D(poly, verts))
                {
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
                                         float extrudeDist, shaderInfo_t *si)
{
    int N = poly->numVerts;
    int numSides = N + 2; /* front + back + N edges */

    /* Safety check */
    if (numSides > MAX_BRUSH_SIDES)
    {
        _printf("WARNING: Merged polygon has %d sides, exceeds MAX_BRUSH_SIDES (%d). Skipping.\n",
                numSides, MAX_BRUSH_SIDES);
        return NULL;
    }

    /* Get the polygon vertex positions */
    vec3_t *pts = malloc(N * sizeof(vec3_t));
    for (int i = 0; i < N; i++)
    {
        pts[i][0] = verts[poly->verts[i] * 3 + 0];
        pts[i][1] = verts[poly->verts[i] * 3 + 1];
        pts[i][2] = verts[poly->verts[i] * 3 + 2];
    }

    /* Front face plane: use first 3 points of the polygon */
    /* We already have the normal from merging, but recompute from the actual
       polygon points to ensure consistency with MapPlaneFromPoints */
    int frontPlane = MapPlaneFromPoints(pts[0], pts[1], pts[2]);
    if (frontPlane == -1)
    {
        // _printf("    WARNING: ExtrudePolygon failed (front plane degenerate, %d verts)\n", N);
        free(pts);
        return NULL;
    }

    /* Use actual face normal for extrusion direction. Needs improving*/
    vec3_t extrudeDir;
    VectorCopy(poly->normal, extrudeDir);

    /* Back face: offset all polygon points along -normal by extrudeDist */
    vec3_t *bpts = malloc(N * sizeof(vec3_t));
    for (int i = 0; i < N; i++)
    {
        VectorMA(pts[i], -extrudeDist, extrudeDir, bpts[i]);
    }

    /* Back plane: reversed winding (use first, last, and second back points) */
    int backPlane = MapPlaneFromPoints(bpts[0], bpts[N - 1], bpts[1]);
    if (backPlane == -1)
    {
        // _printf("    WARNING: ExtrudePolygon failed (back plane degenerate, %d verts)\n", N);
        free(pts);
        free(bpts);
        return NULL;
    }

    /* Allocate brush, plus space for up to 6 bevel planes */
    bspbrush_t *b = AllocBrush(numSides + 6);
    b->numsides = numSides;
    b->detail = qtrue;
    b->contents = CONTENTS_SOLID | CONTENTS_TRANSLUCENT | CONTENTS_DETAIL;
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
    for (int i = 0; i < N; i++)
    {
        int next = (i + 1) % N;
        b->sides[2 + i].planenum = MapPlaneFromPoints(pts[i], bpts[i], pts[next]);
        b->sides[2 + i].shaderInfo = si;

        if (b->sides[2 + i].planenum == -1)
            planesOk = qfalse;
    }

    free(pts);
    free(bpts);

    int flags = si->surfaceFlags;
    flags &= ~(SURF_HINT | SURF_POINTLIGHT | SURF_NONSOLID | SURF_LIGHTFILTER | SURF_ALPHASHADOW);
    flags |= (SURF_NODRAW | SURF_NOLIGHTMAP | SURF_NODLIGHT);

    for (int i = 0; i < numSides; i++)
    {
        b->sides[i].contents = b->contents;
        b->sides[i].surfaceFlags = flags;
    }

    if (!planesOk || frontPlane == -1 || backPlane == -1)
    {
        // _printf("    WARNING: ExtrudePolygon failed (edge plane degenerate, %d verts)\n", N);
        FreeBrush(b);
        return NULL;
    }

    if (!CreateBrushWindings(b))
    {
        // _printf("    WARNING: ExtrudePolygon failed (CreateBrushWindings, %d verts)\n", N);
        FreeBrush(b);
        return NULL;
    }

    if (!BoundBrush(b))
    {
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
static __attribute__((unused)) bspbrush_t *ExtrudeFanToBrush(int hubIdx, int *ringVerts, int ringCount,
                                     clipTri_t *tris, int *fanTriIndices, int fanTriCount,
                                     float *verts, float extrudeDist, shaderInfo_t *si)
{
    int N = fanTriCount;
    int numSides = 2 * N; /* N front faces + N back faces */

    if (numSides > MAX_BRUSH_SIDES)
        return NULL;

    /* Get hub position */
    vec3_t hub;
    hub[0] = verts[hubIdx * 3 + 0];
    hub[1] = verts[hubIdx * 3 + 1];
    hub[2] = verts[hubIdx * 3 + 2];

    /* Compute ring centroid (average of non-hub vertices) */
    vec3_t centroid = {0, 0, 0};
    for (int i = 0; i < ringCount; i++)
    {
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
    if (axisLen < 0.001f)
    {
        return NULL;
    }

    /* Compute max ring radius (distance from centroid to farthest ring vertex) */
    float maxRadius = 0;
    for (int i = 0; i < ringCount; i++)
    {
        vec3_t rv, diff;
        rv[0] = verts[ringVerts[i] * 3 + 0];
        rv[1] = verts[ringVerts[i] * 3 + 1];
        rv[2] = verts[ringVerts[i] * 3 + 2];
        VectorSubtract(rv, centroid, diff);
        float r = VectorLength(diff);
        if (r > maxRadius)
            maxRadius = r;
    }

    /* Reject flat fans: hub must protrude at least 30% of ring radius. */
    if (maxRadius > 0.001f && axisLen < maxRadius * 0.3f)
    {
        return NULL;
    }

    /* Find the maximum projection of ring vertices onto the axis (from hub). */
    float maxProj = 0;
    for (int i = 0; i < ringCount; i++)
    {
        vec3_t rv, diff;
        rv[0] = verts[ringVerts[i] * 3 + 0];
        rv[1] = verts[ringVerts[i] * 3 + 1];
        rv[2] = verts[ringVerts[i] * 3 + 2];
        VectorSubtract(rv, hub, diff);
        float proj = DotProduct(diff, axis);
        if (proj > maxProj)
            maxProj = proj;
    }

    /* Place back point along axis, just past the deepest ring vertex */
    vec3_t backPoint;
    VectorMA(hub, maxProj + extrudeDist, axis, backPoint);

    /* Allocate brush, plus space for up to 6 bevel planes */
    bspbrush_t *b = AllocBrush(numSides + 6);
    b->numsides = numSides;
    b->detail = qtrue;
    b->contents = CONTENTS_SOLID | CONTENTS_TRANSLUCENT | CONTENTS_DETAIL;
    b->contentShader = si;

    qboolean planesOk = qtrue;

    /* Sides 0..N-1: front face planes (original fan triangles) */
    for (int i = 0; i < N; i++)
    {
        vec3_t p0, p1, p2;
        clipTri_t *tri = &tris[fanTriIndices[i]];
        p0[0] = verts[tri->indices[0] * 3 + 0];
        p0[1] = verts[tri->indices[0] * 3 + 1];
        p0[2] = verts[tri->indices[0] * 3 + 2];
        p1[0] = verts[tri->indices[1] * 3 + 0];
        p1[1] = verts[tri->indices[1] * 3 + 1];
        p1[2] = verts[tri->indices[1] * 3 + 2];
        p2[0] = verts[tri->indices[2] * 3 + 0];
        p2[1] = verts[tri->indices[2] * 3 + 1];
        p2[2] = verts[tri->indices[2] * 3 + 2];

        b->sides[i].planenum = MapPlaneFromPoints(p0, p1, p2);
        b->sides[i].shaderInfo = si;
        if (b->sides[i].planenum == -1)
            planesOk = qfalse;
    }

    /* Sides N..2N-1: back face planes (backPoint to each ring edge, reversed winding) */
    for (int i = 0; i < N; i++)
    {
        int next = (i + 1) % ringCount;
        vec3_t r0, r1;
        r0[0] = verts[ringVerts[i] * 3 + 0];
        r0[1] = verts[ringVerts[i] * 3 + 1];
        r0[2] = verts[ringVerts[i] * 3 + 2];
        r1[0] = verts[ringVerts[next] * 3 + 0];
        r1[1] = verts[ringVerts[next] * 3 + 1];
        r1[2] = verts[ringVerts[next] * 3 + 2];

        /* Winding: backPoint, r1, r0 — normal faces inward toward hub */
        b->sides[N + i].planenum = MapPlaneFromPoints(backPoint, r1, r0);
        b->sides[N + i].shaderInfo = si;
        if (b->sides[N + i].planenum == -1)
            planesOk = qfalse;
    }

    int flags = si->surfaceFlags;
    flags &= ~(SURF_HINT | SURF_POINTLIGHT | SURF_NONSOLID | SURF_LIGHTFILTER | SURF_ALPHASHADOW);
    flags |= (SURF_NODRAW | SURF_NOLIGHTMAP | SURF_NODLIGHT);

    for (int i = 0; i < numSides; i++)
    {
        b->sides[i].contents = b->contents;
        b->sides[i].surfaceFlags = flags;
    }

    if (!planesOk)
    {
        FreeBrush(b);
        return NULL;
    }

    if (!CreateBrushWindings(b))
    {
        FreeBrush(b);
        return NULL;
    }

    if (!BoundBrush(b))
    {
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
static __attribute__((unused)) qboolean OrderFanRing(int hubIdx, clipTri_t *tris,
                             int *fanTriIdx, int *pFanTriCount, float *verts,
                             int *ringVerts, int *orderedTriIdx)
{
    int fanTriCount = *pFanTriCount;
    if (fanTriCount < 3)
        return qfalse;

    /* For each triangle in the fan, find its two non-hub vertices (the "edge") */
    typedef struct
    {
        int v0, v1;
        int triIdx;
        qboolean used;
    } fanEdge_t;
    fanEdge_t *edges = malloc(fanTriCount * sizeof(fanEdge_t));

    for (int i = 0; i < fanTriCount; i++)
    {
        clipTri_t *tri = &tris[fanTriIdx[i]];
        int nonHub[2];
        int nh = 0;
        for (int j = 0; j < 3; j++)
        {
            if (PointsMatch(verts, tri->indices[j], hubIdx))
                continue;
            if (nh < 2)
                nonHub[nh++] = tri->indices[j];
        }
        if (nh != 2)
        {
            free(edges);
            return qfalse;
        }
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

    for (int start = 0; start < fanTriCount; start++)
    {
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
        for (int step = 1; step < fanTriCount; step++)
        {
            int lastVert = tempRing[rc - 1];
            qboolean found = qfalse;
            for (int e = 0; e < fanTriCount; e++)
            {
                if (edges[e].used)
                    continue;

                int nextVert = -1;
                if (PointsMatch(verts, edges[e].v0, lastVert))
                    nextVert = edges[e].v1;
                else if (PointsMatch(verts, edges[e].v1, lastVert))
                    nextVert = edges[e].v0;

                if (nextVert >= 0)
                {
                    edges[e].used = qtrue;
                    tempOrdered[oc++] = edges[e].triIdx;

                    /* Check if it closes the loop */
                    if (PointsMatch(verts, nextVert, tempRing[0]))
                    {
                        /* Closed ring found! */
                        if (oc >= 3 && oc > bestOrderedCount)
                        {
                            bestRingCount = rc;
                            bestOrderedCount = oc;
                            for (int k = 0; k < rc; k++)
                                bestRing[k] = tempRing[k];
                            for (int k = 0; k < oc; k++)
                                bestOrdered[k] = tempOrdered[k];
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
            if (!found)
                break;
        }
    }

    free(edges);

    if (bestOrderedCount < 3)
    {
        free(bestRing);
        free(bestOrdered);
        return qfalse;
    }

    /* Copy best result */
    for (int i = 0; i < bestRingCount; i++)
        ringVerts[i] = bestRing[i];
    for (int i = 0; i < bestOrderedCount; i++)
        orderedTriIdx[i] = bestOrdered[i];
    *pFanTriCount = bestOrderedCount;

    free(bestRing);
    free(bestOrdered);
    return qtrue;
}

/*
====================
ValidateFanConvexity

Strictly checks that the diamond/bipyramid brush generated from this fan
is mathematically convex. If any vertex of the proposed shape falls on
the "front" (positive) side of any of its bounding planes, it is concave
and cannot be a valid BSP brush.
====================
*/
static __attribute__((unused)) qboolean ValidateFanConvexity(int hubIdx, int *ringVerts, int ringCount,
                                     clipTri_t *tris, int *fanTriIdx, int fanTriCount,
                                     float *verts, float extrudeDist)
{
    int N = fanTriCount;

    /* Build the list of vertices for the diamond */
    int numPts = N + 2; /* hub, backPoint, and N ring verts */
    vec3_t *pts = malloc(numPts * sizeof(vec3_t));

    /* 0: hub */
    pts[0][0] = verts[hubIdx * 3 + 0];
    pts[0][1] = verts[hubIdx * 3 + 1];
    pts[0][2] = verts[hubIdx * 3 + 2];

    /* 1..N: ring verts */
    vec3_t centroid = {0, 0, 0};
    for (int i = 0; i < N; i++)
    {
        pts[i + 1][0] = verts[ringVerts[i] * 3 + 0];
        pts[i + 1][1] = verts[ringVerts[i] * 3 + 1];
        pts[i + 1][2] = verts[ringVerts[i] * 3 + 2];
        VectorAdd(centroid, pts[i + 1], centroid);
    }
    VectorScale(centroid, 1.0f / N, centroid);

    /* Axis */
    vec3_t axis;
    VectorSubtract(centroid, pts[0], axis);
    if (VectorNormalize(axis, axis) < 0.001f)
    {
        free(pts);
        return qfalse;
    }

    /* Find back projection depth */
    float maxProj = 0;
    for (int i = 0; i < N; i++)
    {
        vec3_t diff;
        VectorSubtract(pts[i + 1], pts[0], diff);
        float proj = DotProduct(diff, axis);
        if (proj > maxProj)
            maxProj = proj;
    }

    /* N+1: backPoint */
    VectorMA(pts[0], maxProj + extrudeDist, axis, pts[N + 1]);

    /* Now, test strict convexity: 
       For every face (N front, N back), ALL points must be on or behind the plane. */
    
    qboolean isConvex = qtrue;

    /* Front faces */
    for (int i = 0; i < N; i++)
    {
        clipTri_t *tri = &tris[fanTriIdx[i]];
        vec3_t p0, p1, p2;
        p0[0] = verts[tri->indices[0] * 3 + 0];
        p0[1] = verts[tri->indices[0] * 3 + 1];
        p0[2] = verts[tri->indices[0] * 3 + 2];
        p1[0] = verts[tri->indices[1] * 3 + 0];
        p1[1] = verts[tri->indices[1] * 3 + 1];
        p1[2] = verts[tri->indices[1] * 3 + 2];
        p2[0] = verts[tri->indices[2] * 3 + 0];
        p2[1] = verts[tri->indices[2] * 3 + 1];
        p2[2] = verts[tri->indices[2] * 3 + 2];

        vec3_t normal;
        vec3_t v1, v2;
        VectorSubtract(p2, p0, v1);
        VectorSubtract(p1, p0, v2);
        CrossProduct(v1, v2, normal);
        VectorNormalize(normal, normal);
        float dist = DotProduct(normal, p0);

        for (int p = 0; p < numPts; p++)
        {
            float d = DotProduct(pts[p], normal) - dist;
            if (d > 0.01f) /* Point is in front of the plane! Shape is concave. */
            {
                isConvex = qfalse;
                break;
            }
        }
        if (!isConvex) break;
    }

    if (!isConvex)
    {
        free(pts);
        return qfalse;
    }

    /* Back faces */
    for (int i = 0; i < N; i++)
    {
        int next = (i + 1) % N;
        vec3_t r0, r1;
        VectorCopy(pts[i + 1], r0);
        VectorCopy(pts[next + 1], r1);

        vec3_t normal;
        vec3_t v1, v2;
        VectorSubtract(r1, pts[N + 1], v1);
        VectorSubtract(r0, pts[N + 1], v2);
        CrossProduct(v1, v2, normal);
        VectorNormalize(normal, normal);
        float dist = DotProduct(normal, pts[N + 1]);

        for (int p = 0; p < numPts; p++)
        {
            float d = DotProduct(pts[p], normal) - dist;
            if (d > 0.01f) /* Point is in front of the plane! Shape is concave. */
            {
                isConvex = qfalse;
                break;
            }
        }
        if (!isConvex) break;
    }

    free(pts);
    return isConvex;
}

/*
====================
ExtrudeTrianglesToBrushes

Optimized version:
1. Triangle fan detection — fans of strictly convex geometry are merged
2. Coplanar merging — adjacent coplanar triangles are merged
3. Individual extrusion — remaining triangles are extruded individually
====================
*/
bspbrush_t *ExtrudeTrianglesToBrushes(colMesh_t *mesh, shaderInfo_t *si)
{
    bspbrush_t *hulls_list = NULL;
    float *verts = (float *)mesh->verts;
    unsigned int *indices = (unsigned int *)mesh->tris;
    int numTris = mesh->numTris;

    if (numTris == 0)
        return NULL;

    #define ENABLE_SNAP_GRID 0
    #define SNAP_GRID 0.125f
#if ENABLE_SNAP_GRID
    for (int i = 0; i < mesh->numVerts; i++) {
        verts[i * 3 + 0] = roundf(verts[i * 3 + 0] / SNAP_GRID) * SNAP_GRID;
        verts[i * 3 + 1] = roundf(verts[i * 3 + 1] / SNAP_GRID) * SNAP_GRID;
        verts[i * 3 + 2] = roundf(verts[i * 3 + 2] / SNAP_GRID) * SNAP_GRID;
    }
#endif

    /* 1. Build triangle descriptors with normals and plane distances */
    clipTri_t *tris = calloc(numTris, sizeof(clipTri_t));
    int validTris = 0;

    for (int i = 0; i < numTris; i++)
    {
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

        if (VectorNormalize(tris[i].normal, tris[i].normal) < 0.0001f)
        {
            tris[i].merged = qtrue; /* mark degenerate as consumed */
            continue;
        }

        tris[i].dist = DotProduct(tris[i].normal, p0);
        validTris++;
    }

    /* 1.5 Fan detection: build vertex→triangle adjacency and detect perfectly convex fans */

    /* Find max vertex index to size the adjacency arrays */
    int maxVertIdx = 0;
    for (int i = 0; i < numTris; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            if (tris[i].indices[j] > maxVertIdx)
                maxVertIdx = tris[i].indices[j];
        }
    }
    maxVertIdx++;

    /* Count triangles per vertex */
    int *vertTriCount = calloc(maxVertIdx, sizeof(int));
    int *vertTriOffset = calloc(maxVertIdx, sizeof(int));
    for (int i = 0; i < numTris; i++)
    {
        if (tris[i].merged)
            continue;
        for (int j = 0; j < 3; j++)
            vertTriCount[tris[i].indices[j]]++;
    }

    /* Build offset table (prefix sum) */
    int totalAdj = 0;
    for (int v = 0; v < maxVertIdx; v++)
    {
        vertTriOffset[v] = totalAdj;
        totalAdj += vertTriCount[v];
    }

    /* Fill adjacency list */
    int *adjTriangles = malloc(totalAdj * sizeof(int));
    int *fillPos = calloc(maxVertIdx, sizeof(int));
    for (int i = 0; i < numTris; i++)
    {
        if (tris[i].merged)
            continue;
        for (int j = 0; j < 3; j++)
        {
            int v = tris[i].indices[j];
            adjTriangles[vertTriOffset[v] + fillPos[v]] = i;
            fillPos[v]++;
        }
    }
    free(fillPos);

    /* Try to form fans around vertices with 3+ adjacent triangles */
    float extrudeDist = 0.5f;
    int *ringVerts = malloc(MAX_POLY_VERTS * sizeof(int));
    int *orderedTriIdx = malloc(MAX_POLY_VERTS * sizeof(int));

#if 0
    for (int v = 0; v < maxVertIdx; v++)
    {
        if (vertTriCount[v] < 3)
            continue;

        /* Collect un-merged triangles adjacent to this vertex */
        int fanCount = 0;
        int fanTriIdx[MAX_POLY_VERTS];
        for (int a = 0; a < vertTriCount[v] && fanCount < MAX_POLY_VERTS; a++)
        {
            int ti = adjTriangles[vertTriOffset[v] + a];
            if (!tris[ti].merged)
                fanTriIdx[fanCount++] = ti;
        }

        if (fanCount < 3)
            continue;

        /* Try to order the ring vertices into a contiguous chain */
        if (!OrderFanRing(v, tris, fanTriIdx, &fanCount, verts, ringVerts, orderedTriIdx))
            continue;

        /* STRICT VALIDATION: check that the resulting shape is 100% mathematically convex */
        if (!ValidateFanConvexity(v, ringVerts, fanCount, tris, orderedTriIdx, fanCount, verts, extrudeDist))
            continue;

        /* Extrude the fan as a single pyramidal brush */
        bspbrush_t *b = ExtrudeFanToBrush(v, ringVerts, fanCount, tris, orderedTriIdx, fanCount,
                                          verts, extrudeDist, si);
        if (b)
        {
            b->next = hulls_list;
            hulls_list = b;

            /* Mark all fan triangles as consumed */
            for (int f = 0; f < fanCount; f++)
            {
                tris[orderedTriIdx[f]].merged = qtrue;
            }
        }
    }
#endif

    free(ringVerts);
    free(orderedTriIdx);
    free(vertTriCount);
    free(vertTriOffset);
    free(adjTriangles);

    /* 2. Merge coplanar adjacent triangles into polygons (remaining non-fan tris) */
    int remaining = 0;
    for (int i = 0; i < numTris; i++)
    {
        if (!tris[i].merged)
            remaining++;
    }

    int maxPolys = remaining > 0 ? remaining : 1;
    clipPoly_t *polys = calloc(maxPolys, sizeof(clipPoly_t));
    int numPolys = 0;

    for (int i = 0; i < numTris; i++)
    {
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
        do
        {
            absorbed = qfalse;
            for (int j = 0; j < numTris; j++)
            {
                if (tris[j].merged)
                    continue;
                if (TryMergeTriIntoPoly(poly, &tris[j], verts))
                {
                    tris[j].merged = qtrue;
                    absorbed = qtrue;
                }
            }
        } while (absorbed);

        numPolys++;
    }

    int mergedCount = 0;
    for (int i = 0; i < numPolys; i++)
    {
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
    for (int i = 0; i < numPolys; i++)
    {
        bspbrush_t *b = ExtrudePolygonToBrush(&polys[i], verts, extrudeDist, si);
        if (b)
        {
            b->next = hulls_list;
            hulls_list = b;
        }
        else
        {
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
bspbrush_t *CombineBrushes(bspbrush_t *list, bspbrush_t *newBrushes)
{
    if (!newBrushes)
        return list;
    if (!list)
        return newBrushes;
    bspbrush_t *tail = newBrushes;
    while (tail->next)
        tail = tail->next;
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
bspbrush_t *GenerateExtrusionCollision(modelInstance_t *inst, shaderInfo_t *shader)
{
    bspbrush_t *hulls_list = NULL;

    _printf("Instance %s: Running Generic Extrusion (%s)\n", inst->modelName, CategoryString(inst->category));

    if (inst->num_collision_meshes == 0)
    {
        return NULL;
    }

    for (int j = 0; j < inst->num_collision_meshes; j++)
    {
        colMesh_t *colMesh = inst->collision_meshes[j];
        if (!colMesh || colMesh->numTris == 0)
            continue;

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
