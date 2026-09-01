#include "qbsp.h"
#include "model_collision.h"

/* =====================================================================
   Optimized extrusion with coplanar merging and axial-snapped normals
   ===================================================================== */

#define COPLANAR_NORMAL_EPSILON 0.001
#define COPLANAR_DIST_EPSILON 0.1
#define MERGE_POINT_EPSILON 0.1
#define MAX_POLY_VERTS 64 /* safety cap for merged polygon vertex count */

#define EXTRUSION_DISTANCE 0.5f      /* Depth (in world units) to extrude collision prisms */
#define EXTRUDE_DEFLATE_OFFSET 0.125f /* Inward vertex deflation along smoothed normals */

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
                                         float extrudeDist, shaderInfo_t *si,
                                         vec3_t *neighborNormals)
{
    si = GetCollisionShaderInfo(si);
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

    /* Use actual face normal for extrusion direction */
    vec3_t extrudeDir;
    VectorCopy(poly->normal, extrudeDir);

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

    /* Back face: offset all polygon points along -normal by extrudeDist from the front face */
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

    /* Sides 2..N+1: edge planes (dihedral miter with neighbor if available, else perpendicular) */
    qboolean planesOk = qtrue;
    for (int i = 0; i < N; i++)
    {
        int next = (i + 1) % N;
        vec3_t edgeMid, bisect, miterBackPt;
        qboolean hasNeighbor = (neighborNormals != NULL && VectorLength(neighborNormals[i]) > 0.1f);

        VectorAdd(pts[i], pts[next], edgeMid);
        VectorScale(edgeMid, 0.5f, edgeMid);

        if (hasNeighbor)
        {
            VectorAdd(poly->normal, neighborNormals[i], bisect);
            if (VectorNormalize(bisect, bisect) < 0.1f)
            {
                VectorCopy(poly->normal, bisect);
            }
        }
        else
        {
            VectorCopy(poly->normal, bisect);
        }

        VectorMA(edgeMid, -extrudeDist, bisect, miterBackPt);

        b->sides[2 + i].planenum = MapPlaneFromPoints(pts[i], miterBackPt, pts[next]);
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

    b = AddBevelsToBrush(b);

    return b;
}

/*
====================
ExtrudeTrianglesToBrushes

Optimized version:
1. Coplanar merging — adjacent coplanar triangles are merged into convex polygons
2. Individual extrusion — polygons and remaining triangles are extruded into prisms
====================
*/
bspbrush_t *ExtrudeTrianglesToBrushes(colMesh_t *mesh, shaderInfo_t *si)
{
    si = GetCollisionShaderInfo(si);
    bspbrush_t *hulls_list = NULL;
    int numTris = mesh->numTris;
    int numVerts = mesh->numVerts;
    unsigned int *indices = (unsigned int *)mesh->tris;
    float *verts = NULL;
    vec3_t *vertNormals = NULL;
    clipTri_t *tris = NULL;
    int validTris = 0;

    if (numTris <= 0 || numVerts <= 0)
        return NULL;

    /* 0. Copy vertices and deflate inward along smoothed vertex normals */
    verts = malloc(numVerts * 3 * sizeof(float));
    memcpy(verts, mesh->verts, numVerts * 3 * sizeof(float));

    vertNormals = calloc(numVerts, sizeof(vec3_t));

    /* Pass 1: Accumulate triangle face normals into each vertex */
    for (int i = 0; i < numTris; i++)
    {
        int base = i * 3;
        int i0 = indices[base + 0];
        int i1 = indices[base + 1];
        int i2 = indices[base + 2];
        vec3_t p0, p1, p2, t1, t2, normal;

        VectorCopy((float*)&mesh->verts[i0], p0);
        VectorCopy((float*)&mesh->verts[i1], p1);
        VectorCopy((float*)&mesh->verts[i2], p2);

        VectorSubtract(p2, p0, t1);
        VectorSubtract(p1, p0, t2);
        CrossProduct(t1, t2, normal);

        if (VectorNormalize(normal, normal) > 0.0001f)
        {
            VectorAdd(vertNormals[i0], normal, vertNormals[i0]);
            VectorAdd(vertNormals[i1], normal, vertNormals[i1]);
            VectorAdd(vertNormals[i2], normal, vertNormals[i2]);
        }
    }

    /* Pass 2: Merge normals for coincident vertices (UV seams, split normals) */
    for (int vA = 0; vA < numVerts; vA++)
    {
        for (int vB = vA + 1; vB < numVerts; vB++)
        {
            if (PointsMatch((float*)mesh->verts, vA, vB))
            {
                vec3_t combined;
                VectorAdd(vertNormals[vA], vertNormals[vB], combined);
                VectorCopy(combined, vertNormals[vA]);
                VectorCopy(combined, vertNormals[vB]);
            }
        }
    }

    /* Pass 3: Normalize and deflate vertices inward */
    if (EXTRUDE_DEFLATE_OFFSET > 0.0f)
    {
        for (int v = 0; v < numVerts; v++)
        {
            if (VectorNormalize(vertNormals[v], vertNormals[v]) > 0.0001f)
            {
                VectorMA((float*)&mesh->verts[v], -EXTRUDE_DEFLATE_OFFSET, vertNormals[v], &verts[v * 3]);
            }
        }
    }

    free(vertNormals);

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
    tris = calloc(numTris, sizeof(clipTri_t));
    validTris = 0;

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

    /* 2. Merge coplanar adjacent triangles into polygons */
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

    /* 3. Build neighbor normal table for dihedral edge mitering */
    {
        vec3_t (*neighborNormals)[MAX_POLY_VERTS] = calloc(numPolys, sizeof(vec3_t[MAX_POLY_VERTS]));
        int pA, pB, eA, eB, nA, nB, v0A, v1A, v0B, v1B;
        int failedPolys = 0;
        int i;

        for (pA = 0; pA < numPolys; pA++)
        {
            nA = polys[pA].numVerts;
            for (eA = 0; eA < nA; eA++)
            {
                if (VectorLength(neighborNormals[pA][eA]) > 0.1f)
                    continue; /* Already matched */

                v0A = polys[pA].verts[eA];
                v1A = polys[pA].verts[(eA + 1) % nA];

                for (pB = pA + 1; pB < numPolys; pB++)
                {
                    nB = polys[pB].numVerts;
                    for (eB = 0; eB < nB; eB++)
                    {
                        v0B = polys[pB].verts[eB];
                        v1B = polys[pB].verts[(eB + 1) % nB];

                        /* Shared manifold edge has opposite winding */
                        if (PointsMatch(verts, v0A, v1B) && PointsMatch(verts, v1A, v0B))
                        {
                            VectorCopy(polys[pB].normal, neighborNormals[pA][eA]);
                            VectorCopy(polys[pA].normal, neighborNormals[pB][eB]);
                            break;
                        }
                    }
                    if (VectorLength(neighborNormals[pA][eA]) > 0.1f)
                        break;
                }
            }
        }

        /* 4. Extrude each remaining polygon into a brush */
        for (i = 0; i < numPolys; i++)
        {
            bspbrush_t *b = ExtrudePolygonToBrush(&polys[i], verts, EXTRUSION_DISTANCE, si, neighborNormals[i]);
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

        free(neighborNormals);
    }

    free(verts);
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
