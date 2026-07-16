/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include "qbsp.h"
#include "xatlas_c.h"
#include <stdint.h>

static int s_chamferBaseDrawSurfs = 0;

static qboolean VectorsNearEqual(const vec3_t a, const vec3_t b, float epsilon)
{
    return (fabs(a[0] - b[0]) < epsilon && fabs(a[1] - b[1]) < epsilon && fabs(a[2] - b[2]) < epsilon);
}


typedef struct edgePoint_s
{
    float intercept;
    vec3_t xyz;
    struct edgePoint_s *prev, *next;
} edgePoint_t;

typedef struct edgeLine_s
{
    vec3_t normal1;
    float dist1;

    vec3_t normal2;
    float dist2;

    vec3_t origin;
    vec3_t dir;

    edgePoint_t chain; // unused element of doubly linked list
} edgeLine_t;

#define	MAX_TJ_FACES	1024

surfaceNeighbor_t *surfaceNeighbors[MAX_MAP_DRAW_SURFS_LIMIT];

typedef struct
{
    float length;
    drawVert_t *dv[2];
} originalEdge_t;

#define MAX_ORIGINAL_EDGES 0x10000
originalEdge_t originalEdges[MAX_ORIGINAL_EDGES];
int numOriginalEdges;

#define MAX_EDGE_LINES 0x10000
edgeLine_t edgeLines[MAX_EDGE_LINES];
int numEdgeLines;

int c_degenerateEdges;
int c_addedVerts;
int c_totalVerts;

int c_natural, c_rotate, c_cant;

// these should be whatever epsilon we actually expect,
// plus SNAP_INT_TO_FLOAT
#define LINE_POSITION_EPSILON 0.25
#define POINT_ON_LINE_EPSILON 0.25

/*
====================
InsertPointOnEdge
====================
*/
void InsertPointOnEdge(vec3_t v, edgeLine_t *e)
{
    vec3_t delta;
    float d;
    edgePoint_t *p, *scan;

    VectorSubtract(v, e->origin, delta);
    d = DotProduct(delta, e->dir);

    p = malloc(sizeof(edgePoint_t));
    p->intercept = d;
    VectorCopy(v, p->xyz);

    if (e->chain.next == &e->chain)
    {
        e->chain.next = e->chain.prev = p;
        p->next = p->prev = &e->chain;
        return;
    }

    scan = e->chain.next;
    for (; scan != &e->chain; scan = scan->next)
    {
        d = p->intercept - scan->intercept;
        if (d > -LINE_POSITION_EPSILON && d < LINE_POSITION_EPSILON)
        {
            free(p);
            return; // the point is already set
        }

        if (p->intercept < scan->intercept)
        {
            // insert here
            p->prev = scan->prev;
            p->next = scan;
            scan->prev->next = p;
            scan->prev = p;
            return;
        }
    }

    // add at the end
    p->prev = scan->prev;
    p->next = scan;
    scan->prev->next = p;
    scan->prev = p;
}

/*
====================
AddEdge
====================
*/
int AddEdge(vec3_t v1, vec3_t v2, qboolean createNonAxial)
{
    int i;
    edgeLine_t *e;
    float d;
    vec3_t dir;

    VectorSubtract(v2, v1, dir);
    d = VectorNormalize(dir, dir);
    if (d < 0.1)
    {
        // if we added a 0 length vector, it would make degenerate planes
        c_degenerateEdges++;
        return -1;
    }

    if (!createNonAxial)
    {
        if (fabs(dir[0] + dir[1] + dir[2]) != 1.0)
        {
            if (numOriginalEdges == MAX_ORIGINAL_EDGES)
            {
                Error("MAX_ORIGINAL_EDGES");
            }
            originalEdges[numOriginalEdges].dv[0] = (drawVert_t *)v1;
            originalEdges[numOriginalEdges].dv[1] = (drawVert_t *)v2;
            originalEdges[numOriginalEdges].length = d;
            numOriginalEdges++;
            return -1;
        }
    }

    for (i = 0; i < numEdgeLines; i++)
    {
        e = &edgeLines[i];

        d = DotProduct(v1, e->normal1) - e->dist1;
        if (d < -POINT_ON_LINE_EPSILON || d > POINT_ON_LINE_EPSILON)
        {
            continue;
        }
        d = DotProduct(v1, e->normal2) - e->dist2;
        if (d < -POINT_ON_LINE_EPSILON || d > POINT_ON_LINE_EPSILON)
        {
            continue;
        }

        d = DotProduct(v2, e->normal1) - e->dist1;
        if (d < -POINT_ON_LINE_EPSILON || d > POINT_ON_LINE_EPSILON)
        {
            continue;
        }
        d = DotProduct(v2, e->normal2) - e->dist2;
        if (d < -POINT_ON_LINE_EPSILON || d > POINT_ON_LINE_EPSILON)
        {
            continue;
        }

        // this is the edge
        InsertPointOnEdge(v1, e);
        InsertPointOnEdge(v2, e);
        return i;
    }

    // create a new edge
    if (numEdgeLines >= MAX_EDGE_LINES)
    {
        Error("MAX_EDGE_LINES");
    }

    e = &edgeLines[numEdgeLines];
    numEdgeLines++;

    e->chain.next = e->chain.prev = &e->chain;

    VectorCopy(v1, e->origin);
    VectorCopy(dir, e->dir);

    MakeNormalVectors(e->dir, e->normal1, e->normal2);
    e->dist1 = DotProduct(e->origin, e->normal1);
    e->dist2 = DotProduct(e->origin, e->normal2);

    InsertPointOnEdge(v1, e);
    InsertPointOnEdge(v2, e);

    return numEdgeLines - 1;
}

/*
====================
AddSurfaceEdges
====================
*/
void AddSurfaceEdges(mapDrawSurface_t *ds)
{
    int i;

    for (i = 0; i < ds->numVerts; i++)
    {
        // save the edge number in the lightmap field
        // so we don't need to look it up again
        ds->verts[i].lightmap[0][0] = AddEdge(
            ds->verts[i].xyz, ds->verts[(i + 1) % ds->numVerts].xyz, qfalse);
    }
}

/*
================
ColinearEdge
================
*/
qboolean ColinearEdge(vec3_t v1, vec3_t v2, vec3_t v3)
{
    vec3_t midpoint, dir, offset, on;
    float d;

    VectorSubtract(v2, v1, midpoint);
    VectorSubtract(v3, v1, dir);
    d = VectorNormalize(dir, dir);
    if (d == 0)
    {
        return qfalse; // degenerate
    }

    d = DotProduct(midpoint, dir);
    VectorScale(dir, d, on);
    VectorSubtract(midpoint, on, offset);
    d = VectorLength(offset);

    if (d < 0.1)
    {
        return qtrue;
    }

    return qfalse;
}

/*
====================
AddPatchEdges

Add colinear border edges, which will fix some classes of patch to
brush tjunctions
====================
*/
void AddPatchEdges(mapDrawSurface_t *ds)
{
    int i;
    float *v1, *v2, *v3;

    for (i = 0; i < ds->patchWidth - 2; i += 2)
    {
        v1 = ds->verts[i].xyz;
        v2 = ds->verts[i + 1].xyz;
        v3 = ds->verts[i + 2].xyz;

        // if v2 is the midpoint of v1 to v3, add an edge from v1 to v3
        if (ColinearEdge(v1, v2, v3))
        {
            AddEdge(v1, v3, qfalse);
        }

        v1 = ds->verts[(ds->patchHeight - 1) * ds->patchWidth + i].xyz;
        v2 = ds->verts[(ds->patchHeight - 1) * ds->patchWidth + i + 1].xyz;
        v3 = ds->verts[(ds->patchHeight - 1) * ds->patchWidth + i + 2].xyz;

        // if v2 is on the v1 to v3 line, add an edge from v1 to v3
        if (ColinearEdge(v1, v2, v3))
        {
            AddEdge(v1, v3, qfalse);
        }
    }

    for (i = 0; i < ds->patchHeight - 2; i += 2)
    {
        v1 = ds->verts[i * ds->patchWidth].xyz;
        v2 = ds->verts[(i + 1) * ds->patchWidth].xyz;
        v3 = ds->verts[(i + 2) * ds->patchWidth].xyz;

        // if v2 is the midpoint of v1 to v3, add an edge from v1 to v3
        if (ColinearEdge(v1, v2, v3))
        {
            AddEdge(v1, v3, qfalse);
        }

        v1 = ds->verts[(ds->patchWidth - 1) + i * ds->patchWidth].xyz;
        v2 = ds->verts[(ds->patchWidth - 1) + (i + 1) * ds->patchWidth].xyz;
        v3 = ds->verts[(ds->patchWidth - 1) + (i + 2) * ds->patchWidth].xyz;

        // if v2 is the midpoint of v1 to v3, add an edge from v1 to v3
        if (ColinearEdge(v1, v2, v3))
        {
            AddEdge(v1, v3, qfalse);
        }
    }
}

/*
====================
FixSurfaceJunctions
====================
*/
#define MAX_TJUNCTION_VERTS 1024
void FixSurfaceJunctions(mapDrawSurface_t *ds)
{
    int i, j, k;
    edgeLine_t *e;
    edgePoint_t *p;
    int counts[MAX_TJUNCTION_VERTS];
    int originals[MAX_TJUNCTION_VERTS];
    drawVert_t verts[MAX_TJUNCTION_VERTS], *v1, *v2;
    int numVerts;
    float start, end, frac;
    vec3_t delta;

    numVerts = 0;
    for (i = 0; i < ds->numVerts; i++)
    {
        counts[i] = 0;

        // copy first vert
        if (numVerts == MAX_SURFACE_VERTS)
        {
            Error("MAX_SURFACE_VERTS");
        }
        verts[numVerts] = ds->verts[i];
        originals[numVerts] = i;
        numVerts++;

        // check to see if there are any t junctions before the next vert
        v1 = &ds->verts[i];
        v2 = &ds->verts[(i + 1) % ds->numVerts];

        j = (int)ds->verts[i].lightmap[0][0];
        if (j == -1)
        {
            continue; // degenerate edge
        }
        e = &edgeLines[j];

        VectorSubtract(v1->xyz, e->origin, delta);
        start = DotProduct(delta, e->dir);

        VectorSubtract(v2->xyz, e->origin, delta);
        end = DotProduct(delta, e->dir);

        if (start < end)
        {
            p = e->chain.next;
        }
        else
        {
            p = e->chain.prev;
        }

        for (; p != &e->chain;)
        {
            if (start < end)
            {
                if (p->intercept > end - ON_EPSILON)
                {
                    break;
                }
            }
            else
            {
                if (p->intercept < end + ON_EPSILON)
                {
                    break;
                }
            }

            if ((start < end && p->intercept > start + ON_EPSILON) ||
                (start > end && p->intercept < start - ON_EPSILON))
            {
                // insert this point
                if (numVerts == MAX_SURFACE_VERTS)
                {
                    Error("MAX_SURFACE_VERTS");
                }

                // take the exact intercept point
                VectorCopy(p->xyz, verts[numVerts].xyz);

                // copy the normal
                VectorCopy(v1->normal, verts[numVerts].normal);

                // interpolate the texture coordinates
                frac = (p->intercept - start) / (end - start);
                for (j = 0; j < 2; j++)
                {
                    verts[numVerts].st[j] = v1->st[j] + frac * (v2->st[j] - v1->st[j]);
                }
                originals[numVerts] = i;
                numVerts++;
                counts[i]++;
            }

            if (start < end)
            {
                p = p->next;
            }
            else
            {
                p = p->prev;
            }
        }
    }

    c_addedVerts += numVerts - ds->numVerts;
    c_totalVerts += numVerts;

    // FIXME: check to see if the entire surface degenerated
    // after snapping

    // rotate the points so that the initial vertex is between
    // two non-subdivided edges
    for (i = 0; i < numVerts; i++)
    {
        if (originals[(i + 1) % numVerts] == originals[i])
        {
            continue;
        }
        j = (i + numVerts - 1) % numVerts;
        k = (i + numVerts - 2) % numVerts;
        if (originals[j] == originals[k])
        {
            continue;
        }
        break;
    }

    if (i == 0)
    {
        // fine the way it is
        c_natural++;

        ds->numVerts = numVerts;
        ds->verts = malloc(numVerts * sizeof(*ds->verts));
        memcpy(ds->verts, verts, numVerts * sizeof(*ds->verts));

        return;
    }
    if (i == numVerts)
    {
        // create a vertex in the middle to start the fan
        c_cant++;

        /*
                        memset ( &verts[numVerts], 0, sizeof( verts[numVerts] ) );
                        for ( i = 0 ; i < numVerts ; i++ ) {
                                for ( j = 0 ; j < 10 ; j++ ) {
                                        verts[numVerts].xyz[j] += verts[i].xyz[j];
                                }
                        }
                        for ( j = 0 ; j < 10 ; j++ ) {
                                verts[numVerts].xyz[j] /= numVerts;
                        }

                        i = numVerts;
                        numVerts++;
        */
    }
    else
    {
        // just rotate the vertexes
        c_rotate++;
    }

    ds->numVerts = numVerts;
    ds->verts = malloc(numVerts * sizeof(*ds->verts));

    for (j = 0; j < ds->numVerts; j++)
    {
        ds->verts[j] = verts[(j + i) % ds->numVerts];
    }
}

/*
================
EdgeCompare
================
*/
int EdgeCompare(const void *elem1, const void *elem2)
{
    float d1, d2;

    d1 = ((originalEdge_t *)elem1)->length;
    d2 = ((originalEdge_t *)elem2)->length;

    if (d1 < d2)
    {
        return -1;
    }
    if (d2 > d1)
    {
        return 1;
    }
    return 0;
}

/*
================
FixTJunctions

Call after the surface list has been pruned, but before lightmap allocation
================
*/
void FixTJunctions(entity_t *ent)
{
    int i;
    mapDrawSurface_t *ds;
    int axialEdgeLines;
    originalEdge_t *e;

    qprintf("----- FixTJunctions -----\n");

    numEdgeLines = 0;
    numOriginalEdges = 0;

    // add all the edges
    // this actually creates axial edges, but it
    // only creates originalEdge_t structures
    // for non-axial edges
    for (i = ent->firstDrawSurf; i < numMapDrawSurfs; i++)
    {
        ds = &mapDrawSurfs[i];
        if (!ds->shaderInfo)
        {
            continue;
        }
        if (ds->patch)
        {
            AddPatchEdges(ds);
        }
        else if (ds->shaderInfo->autosprite || ds->shaderInfo->notjunc ||
                 ds->miscModel)
        {
            // miscModels don't add tjunctions
        }
        else
        {
            AddSurfaceEdges(ds);
        }
    }

    axialEdgeLines = numEdgeLines;

    // sort the non-axial edges by length
    qsort(originalEdges, numOriginalEdges, sizeof(originalEdges[0]), EdgeCompare);

    // add the non-axial edges, longest first
    // this gives the most accurate edge description
    for (i = 0; i < numOriginalEdges; i++)
    {
        e = &originalEdges[i];
        e->dv[0]->lightmap[0][0] = AddEdge(e->dv[0]->xyz, e->dv[1]->xyz, qtrue);
    }

    qprintf("%6i axial edge lines\n", axialEdgeLines);
    qprintf("%6i non-axial edge lines\n", numEdgeLines - axialEdgeLines);
    qprintf("%6i degenerate edges\n", c_degenerateEdges);

    // insert any needed vertexes
    for (i = ent->firstDrawSurf; i < numMapDrawSurfs; i++)
    {
        ds = &mapDrawSurfs[i];
        if (ds->patch)
        {
            continue;
        }
        if (ds->shaderInfo->autosprite || ds->shaderInfo->notjunc ||
            ds->miscModel)
        {
            continue;
        }

        FixSurfaceJunctions(ds);
    }

    qprintf("%6i verts added for tjunctions\n", c_addedVerts);
    qprintf("%6i total verts\n", c_totalVerts);
    qprintf("%6i naturally ordered\n", c_natural);
    qprintf("%6i rotated orders\n", c_rotate);
    qprintf("%6i can't order\n", c_cant);
}
static qboolean IsChamferCandidate(mapDrawSurface_t *ds)
{
    if (ds->numVerts <= 0 || ds->side == NULL) return qfalse;
    if (ds->patch || ds->miscModel || ds->flareSurface || ds->isDecal) return qfalse;
    if (ds->shaderInfo && (ds->shaderInfo->surfaceFlags & (SURF_NODRAW | SURF_SKY | SURF_NOLIGHTMAP))) return qfalse;
    if (ds->shaderInfo && (ds->shaderInfo->contents & (CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_FOG))) return qfalse;
    return qtrue;
}

/*
================
BuildSurfaceAdjacencyGraph
================
*/
void BuildSurfaceAdjacencyGraph(entity_t *e)
{
    int i, j, v, w;
    mapDrawSurface_t *dsA, *dsB;

    qprintf("----- BuildSurfaceAdjacencyGraph -----\n");
    memset(surfaceNeighbors, 0, sizeof(surfaceNeighbors));

    for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++)
    {
        dsA = &mapDrawSurfs[i];
        if (!IsChamferCandidate(dsA)) continue;

        for (j = i + 1; j < numMapDrawSurfs; j++)
        {
            dsB = &mapDrawSurfs[j];
            if (!IsChamferCandidate(dsB)) continue;

            // Only register chamfer neighbors if their opacity/translucency matches
            qboolean transA = (dsA->shaderInfo && (dsA->shaderInfo->contents & CONTENTS_TRANSLUCENT)) ? qtrue : qfalse;
            qboolean transB = (dsB->shaderInfo && (dsB->shaderInfo->contents & CONTENTS_TRANSLUCENT)) ? qtrue : qfalse;
            if (transA != transB) continue;

            // Find shared vertices
            surfaceNeighbor_t nbA = {0}, nbB = {0};
            
            for (v = 0; v < dsA->numVerts; v++)
            {
                for (w = 0; w < dsB->numVerts; w++)
                {
                    if (VectorCompare(dsA->verts[v].xyz, dsB->verts[w].xyz))
                    {
                        if (nbA.sharedChainLen < MAX_CHAMFER_VERTS) {
                            nbA.sharedChainIndicesA[nbA.sharedChainLen] = v;
                            nbA.sharedChainIndicesB[nbA.sharedChainLen] = w;
                            nbA.sharedChainLen++;
                        }
                        
                        if (nbB.sharedChainLen < MAX_CHAMFER_VERTS) {
                            nbB.sharedChainIndicesA[nbB.sharedChainLen] = w;
                            nbB.sharedChainIndicesB[nbB.sharedChainLen] = v;
                            nbB.sharedChainLen++;
                        }
                        break;
                    }
                }
            }

            if (nbA.sharedChainLen >= 2)
            {
                surfaceNeighbor_t *newNbA = malloc(sizeof(surfaceNeighbor_t));
                *newNbA = nbA;
                newNbA->neighborSurfaceNum = j;
                newNbA->next = surfaceNeighbors[i];
                surfaceNeighbors[i] = newNbA;

                surfaceNeighbor_t *newNbB = malloc(sizeof(surfaceNeighbor_t));
                *newNbB = nbB;
                newNbB->neighborSurfaceNum = i;
                newNbB->next = surfaceNeighbors[j];
                surfaceNeighbors[j] = newNbB;
            }
        }
    }
}

/*
================
ComputeAllInsets
================
*/
static void ComputeAllInsets(mapDrawSurface_t *ds, surfaceChamferEdge_t *edges, int numEdges, float chamferWidth, drawVert_t *insetMap)
{
    int i, j, v;
    vec3_t faceNormal;
    qboolean edgeChamfered[MAX_CHAMFER_VERTS];
    vec3_t edgeLineOrigin[MAX_CHAMFER_VERTS];
    vec3_t edgeLineDir[MAX_CHAMFER_VERTS];
    
    memset(edgeChamfered, 0, sizeof(edgeChamfered));
    VectorCopy(mapplanes[ds->side->planenum].normal, faceNormal);

    for (i = 0; i < numEdges; i++) {
        surfaceChamferEdge_t *edge = &edges[i];
        for (j = 0; j < edge->chainLen - 1; j++) {
            edgeChamfered[edge->chainIndices[j]] = qtrue;
        }
    }

    for (v = 0; v < ds->numVerts; v++) {
        int next_v = (v + 1) % ds->numVerts;
        vec3_t edgeDir;
        VectorSubtract(ds->verts[next_v].xyz, ds->verts[v].xyz, edgeDir);
        VectorNormalize(edgeDir, edgeDir);
        
        vec3_t insetDir;
#if DEBUG_SHOW_CHAMFERS
        CrossProduct(faceNormal, edgeDir, insetDir);
#else
        CrossProduct(edgeDir, faceNormal, insetDir);
#endif
        VectorNormalize(insetDir, insetDir);
        
        VectorCopy(ds->verts[v].xyz, edgeLineOrigin[v]);
        if (edgeChamfered[v]) {
            VectorMA(ds->verts[v].xyz, chamferWidth, insetDir, edgeLineOrigin[v]);
        }
        VectorCopy(edgeDir, edgeLineDir[v]);
    }

    for (v = 0; v < ds->numVerts; v++) {
        int prev_v = (v - 1 + ds->numVerts) % ds->numVerts;
        int next_v = (v + 1) % ds->numVerts;
        
        if (edgeChamfered[prev_v] || edgeChamfered[v]) {
            vec3_t p1, d1, p2, d2;
            VectorCopy(edgeLineOrigin[prev_v], p1);
            VectorCopy(edgeLineDir[prev_v], d1);
            VectorCopy(edgeLineOrigin[v], p2);
            VectorCopy(edgeLineDir[v], d2);
            
            float a = DotProduct(d1, d1);
            float b = DotProduct(d1, d2);
            float c = DotProduct(d2, d2);
            vec3_t w0;
            VectorSubtract(p1, p2, w0);
            float d = DotProduct(d1, w0);
            float e = DotProduct(d2, w0);
            
            float det = a * c - b * b;
            float dist = 0.0f;
            if (fabs(det) > 0.001f) {
                dist = (b * e - c * d) / det;
                VectorMA(p1, dist, d1, insetMap[v].xyz);
            }
            else {
                if (edgeChamfered[v]) {
                    vec3_t edgeDir, insetDir;
                    VectorSubtract(ds->verts[next_v].xyz, ds->verts[v].xyz, edgeDir);
                    VectorNormalize(edgeDir, edgeDir);
#if DEBUG_SHOW_CHAMFERS
                    CrossProduct(faceNormal, edgeDir, insetDir);
#else
                    CrossProduct(edgeDir, faceNormal, insetDir);
#endif
                    VectorNormalize(insetDir, insetDir);
                    VectorMA(ds->verts[v].xyz, chamferWidth, insetDir, insetMap[v].xyz);
                } else {
                    vec3_t edgeDir, insetDir;
                    VectorSubtract(ds->verts[v].xyz, ds->verts[prev_v].xyz, edgeDir);
                    VectorNormalize(edgeDir, edgeDir);
#if DEBUG_SHOW_CHAMFERS
                    CrossProduct(faceNormal, edgeDir, insetDir);
#else
                    CrossProduct(edgeDir, faceNormal, insetDir);
#endif
                    VectorNormalize(insetDir, insetDir);
                    VectorMA(ds->verts[v].xyz, chamferWidth, insetDir, insetMap[v].xyz);
                }
            }
            
            float maxMove = chamferWidth * 4.0f;
            vec3_t diff_corner;
            VectorSubtract(insetMap[v].xyz, ds->verts[v].xyz, diff_corner);
            if (VectorLength(diff_corner) > maxMove) {
                VectorNormalize(diff_corner, diff_corner);
                VectorMA(ds->verts[v].xyz, maxMove, diff_corner, insetMap[v].xyz);
            }
        } 
        else {
            VectorCopy(ds->verts[v].xyz, insetMap[v].xyz);
        }
    }
}

static void ShiftVertexUV(mapDrawSurface_t *ds, drawVert_t *dv, const vec3_t old_xyz, const vec3_t new_xyz)
{
    side_t *s = ds->side;
    shaderInfo_t *si = ds->shaderInfo;
    if (!s || !si) return;

    vec3_t delta;
    VectorSubtract(new_xyz, old_xyz, delta);
    if (VectorLength(delta) < 0.001f) return;

    if (g_bBrushPrimit == BPRIMIT_OLDBRUSHES)
    {
        if (si->width && si->height) {
            dv->st[0] += DotProduct(s->vecs[0], delta) / si->width;
            dv->st[1] += DotProduct(s->vecs[1], delta) / si->height;
        }
    }
    else
    {
        vec3_t texX, texY;
        ComputeAxisBase(mapplanes[s->planenum].normal, texX, texY);
        float dx = DotProduct(delta, texX);
        float dy = DotProduct(delta, texY);
        dv->st[0] += s->texMat[0][0] * dx + s->texMat[0][1] * dy;
        dv->st[1] += s->texMat[1][0] * dx + s->texMat[1][1] * dy;
    }
}

/*
================
ChopTjunctions
================
*/
void ChopTjunctions(entity_t *e)
{
    int i, j, v, w;
    int numChopped = 0;

    qprintf("----- ChopTjunctions -----\n");

    for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++)
    {
        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        qboolean chopped = qfalse;

        if (!IsChamferCandidate(dsA)) continue;

        for (j = e->firstDrawSurf; j < numMapDrawSurfs && !chopped; j++)
        {
            mapDrawSurface_t *dsB;
            vec3_t normalA, normalB;
            float dot;

            if (i == j) continue;
            dsB = &mapDrawSurfs[j];
            if (!IsChamferCandidate(dsB)) continue;

            // Never slice an opaque surface across its face due to a transparent surface touching it (and vice-versa)
            qboolean transA = (dsA->shaderInfo && (dsA->shaderInfo->contents & CONTENTS_TRANSLUCENT)) ? qtrue : qfalse;
            qboolean transB = (dsB->shaderInfo && (dsB->shaderInfo->contents & CONTENTS_TRANSLUCENT)) ? qtrue : qfalse;
            if (transA != transB) continue;

            VectorCopy(mapplanes[dsA->side->planenum].normal, normalA);
            VectorCopy(mapplanes[dsB->side->planenum].normal, normalB);
            dot = DotProduct(normalA, normalB);
            if (dot > 0.866f || dot < -0.866f) continue;

            for (v = 0; v < dsA->numVerts && !chopped; v++)
            {
                int next_v = (v + 1) % dsA->numVerts;
                vec3_t V0, V1, edgeDir;
                float full_len;

                VectorCopy(dsA->verts[v].xyz,      V0);
                VectorCopy(dsA->verts[next_v].xyz, V1);

                VectorSubtract(V1, V0, edgeDir);
                full_len = VectorNormalize(edgeDir, edgeDir);
                if (full_len < 0.1f) continue;

                for (w = 0; w < dsB->numVerts && !chopped; w++)
                {
                    vec3_t V_B, toB, proj, perp, splitNormal;
                    float t, perp_dist, splitDist;
                    winding_t *w_in, *front, *back;

                    VectorCopy(dsB->verts[w].xyz, V_B);

                    VectorSubtract(V_B, V0, toB);
                    t = DotProduct(toB, edgeDir);

                    VectorScale(edgeDir, t, proj);
                    VectorSubtract(toB, proj, perp);
                    perp_dist = VectorLength(perp);

                    if (perp_dist > POINT_ON_LINE_EPSILON) continue;
                    if (t < ON_EPSILON) continue;
                    if (t > full_len - ON_EPSILON) continue;

                    VectorCopy(edgeDir, splitNormal);
                    splitDist = DotProduct(V_B, splitNormal);

                    w_in = WindingFromDrawSurf(dsA);
                    front = NULL;
                    back = NULL;
                    ClipWindingEpsilon(w_in, splitNormal, splitDist, ON_EPSILON,
                                       &front, &back);
                    FreeWinding(w_in);

                    if (!front || front->numpoints < 3 || WindingArea(front) < 1.0f ||
                        !back  || back->numpoints  < 3 || WindingArea(back) < 1.0f)
                    {
                        if (front) FreeWinding(front);
                        if (back)  FreeWinding(back);
                        continue;
                    }

                    int parentIdx = (dsA->parentSurfaceNum != -1) ? dsA->parentSurfaceNum : i;
                    mapDrawSurface_t *dsFront;
                    mapDrawSurface_t *dsBack;

                    dsA->numVerts = 0;

                    dsFront = DrawSurfaceForSide(dsA->mapBrush, dsA->side, front);
                    dsBack  = DrawSurfaceForSide(dsA->mapBrush, dsA->side, back);
                    dsFront->parentSurfaceNum = parentIdx;
                    dsBack->parentSurfaceNum  = parentIdx;
                    FreeWinding(front);
                    FreeWinding(back);

                    numChopped++;
                    chopped = qtrue;
                }
            }
        }

        if (chopped)
        {
            i--;
        }
    }

    qprintf("%6i surfaces chopped for T-junctions\n", numChopped);
}

static qboolean IsOriginalBrushEdge(mapDrawSurface_t *ds, vec3_t v1, vec3_t v2)
{
    int i;
    bspbrush_t *brush = ds->mapBrush;
    if (!brush) return qfalse;

    for (i = 0; i < brush->numsides; i++)
    {
        side_t *side = &brush->sides[i];
        plane_t *plane = &mapplanes[side->planenum];

        if (side == ds->side) continue;

        float d1 = DotProduct(v1, plane->normal) - plane->dist;
        float d2 = DotProduct(v2, plane->normal) - plane->dist;

        if (fabs(d1) < 0.1f && fabs(d2) < 0.1f)
        {
            return qtrue;
        }
    }
    return qfalse;
}

/*
================
ChamferSurfaceEdges
================
*/
void ChamferSurfaceEdges(entity_t *e)
{
    int i;
    int numBaseDrawSurfs;
    drawVert_t *globalInsets[MAX_MAP_DRAW_SURFS_LIMIT];
    
    qprintf("----- ChamferSurfaceEdges (V3: Normal Bending) -----\n");
    memset(globalInsets, 0, sizeof(globalInsets));
    BuildSurfaceAdjacencyGraph(e);
    numBaseDrawSurfs = numMapDrawSurfs;
    s_chamferBaseDrawSurfs = numBaseDrawSurfs;
    
    // Pass 1: Compute inner bodies (insets)
    for (i = e->firstDrawSurf; i < numBaseDrawSurfs; i++)
    {
        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        surfaceChamferEdge_t edges[MAX_CHAMFER_VERTS];
        int numEdges = 0;
        surfaceNeighbor_t *nb;
        
        if (!IsChamferCandidate(dsA)) continue;

        // ----------------------------------------------------
        // ADAPTIVE CHAMFER WIDTH & SAFEGUARDS (10x Ratio)
        // ----------------------------------------------------
        float min_edge = 999999.0f;
        for (int v = 0; v < dsA->numVerts; v++) {
            int next_v = (v + 1) % dsA->numVerts;
            vec3_t edgeDir;
            VectorSubtract(dsA->verts[next_v].xyz, dsA->verts[v].xyz, edgeDir);
            float len = VectorLength(edgeDir);
            if (len < min_edge) {
                min_edge = len;
            }
        }

#define MIN_CHAMFER_WIDTH 0.5f
        float surface_chamfer_width = chamfer_global_width;
        float required_space = 4.0f * chamfer_global_width;

        if (min_edge < required_space) {
            // Scale down to maintain the 4x ratio
            surface_chamfer_width = min_edge / 4.0f;
        }

        if (surface_chamfer_width < MIN_CHAMFER_WIDTH) {
            // Surface is too small to safely chamfer, leave it entirely original
            continue;
        }
        // ----------------------------------------------------

        for (nb = surfaceNeighbors[i]; nb; nb = nb->next)
        {
            int j = nb->neighborSurfaceNum;
            mapDrawSurface_t *dsB = &mapDrawSurfs[j];
            vec3_t normalA, normalB;
            float dot;
            
            VectorCopy(mapplanes[dsA->side->planenum].normal, normalA);
            VectorCopy(mapplanes[dsB->side->planenum].normal, normalB);
            
            dot = DotProduct(normalA, normalB);
            if (dot > 0.866f || dot < -0.866f) continue;

            qboolean isShared[MAX_CHAMFER_VERTS];
            memset(isShared, 0, sizeof(isShared));
            for (int k = 0; k < nb->sharedChainLen; k++) {
                isShared[nb->sharedChainIndicesA[k]] = qtrue;
            }
            
            for (int v = 0; v < dsA->numVerts; v++) {
                int prev_v = (v - 1 + dsA->numVerts) % dsA->numVerts;
                if (isShared[v] && !isShared[prev_v]) {
                    int curr = v;
                    int chainLen = 0;
                    while (isShared[curr] && chainLen < dsA->numVerts) {
                        edges[numEdges].chainIndices[chainLen++] = curr;
                        curr = (curr + 1) % dsA->numVerts;
                    }
                    if (chainLen >= 2 && numEdges < MAX_CHAMFER_VERTS) {
                        vec3_t vStart, vEnd;
                        VectorCopy(dsA->verts[edges[numEdges].chainIndices[0]].xyz, vStart);
                        VectorCopy(dsA->verts[edges[numEdges].chainIndices[chainLen - 1]].xyz, vEnd);

                        if (IsOriginalBrushEdge(dsA, vStart, vEnd)) {
                            edges[numEdges].chainLen = chainLen;
                            
                            // Compute blended normal for this edge
                            vec3_t blended;
                            VectorAdd(normalA, normalB, blended);
                            VectorNormalize(blended, blended);
                            VectorCopy(blended, edges[numEdges].blendedNormal);
                            
                            numEdges++;
                        }
                    }
                }
            }
        }

        if (numEdges > 0)
        {
            globalInsets[i] = malloc(dsA->numVerts * sizeof(drawVert_t));
            memcpy(globalInsets[i], dsA->verts, dsA->numVerts * sizeof(drawVert_t));
            ComputeAllInsets(dsA, edges, numEdges, surface_chamfer_width, globalInsets[i]);
            for (int v = 0; v < dsA->numVerts; v++) {
                ShiftVertexUV(dsA, &globalInsets[i][v], dsA->verts[v].xyz, globalInsets[i][v].xyz);
            }
        }
    }

    // Pass 2: Generate normal-bending strips on the flat faces
    for (i = e->firstDrawSurf; i < numBaseDrawSurfs; i++)
    {
        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        surfaceNeighbor_t *nb;
        vec3_t faceNormal;
        
        if (!globalInsets[i]) continue;
        VectorCopy(mapplanes[dsA->side->planenum].normal, faceNormal);

        for (nb = surfaceNeighbors[i]; nb; nb = nb->next)
        {
            int j = nb->neighborSurfaceNum;
            mapDrawSurface_t *dsB = &mapDrawSurfs[j];
            vec3_t normalB;
            float dot;
            
            VectorCopy(mapplanes[dsB->side->planenum].normal, normalB);
            dot = DotProduct(faceNormal, normalB);
            if (dot > 0.866f || dot < -0.866f) continue;

            vec3_t blendedNormal;
            VectorAdd(faceNormal, normalB, blendedNormal);
            VectorNormalize(blendedNormal, blendedNormal);

            qboolean isShared[MAX_CHAMFER_VERTS];
            memset(isShared, 0, sizeof(isShared));
            for (int k = 0; k < nb->sharedChainLen; k++) {
                isShared[nb->sharedChainIndicesA[k]] = qtrue;
            }
            
            for (int v = 0; v < dsA->numVerts; v++) {
                int prev_v = (v - 1 + dsA->numVerts) % dsA->numVerts;
                if (isShared[v] && !isShared[prev_v]) {
                    int curr = v;
                    int chainLen = 0;
                    int chain[MAX_CHAMFER_VERTS];
                    
                    while (isShared[curr] && chainLen < dsA->numVerts) {
                        chain[chainLen++] = curr;
                        curr = (curr + 1) % dsA->numVerts;
                    }
                    
                    if (chainLen >= 2) {
                        vec3_t vStart, vEnd;
                        VectorCopy(dsA->verts[chain[0]].xyz, vStart);
                        VectorCopy(dsA->verts[chain[chainLen - 1]].xyz, vEnd);

                        if (IsOriginalBrushEdge(dsA, vStart, vEnd)) {
                            dsA->parentSurfaceNum = i;

                            mapDrawSurface_t *strip = AllocDrawSurf();
                            strip->parentSurfaceNum = i;
                            strip->shaderInfo = dsA->shaderInfo;
                            strip->mapBrush = dsA->mapBrush;
                            strip->side = dsA->side;
                            strip->planeNum = dsA->planeNum;
                            strip->samplesize = dsA->samplesize;
                            strip->lightmapScale = dsA->lightmapScale;

                        // Vertices are laid out in perimeter (winding) order so that
                        // SurfaceAsTristrip can auto-triangulate them correctly:
                        //   [0 .. chainLen-1]         = outer verts, forward order
                        //   [chainLen .. 2*chainLen-1] = inner verts, REVERSE order
                        // This gives a closed clockwise perimeter: OuterA->OuterB->InnerB->InnerA
                        strip->numVerts = chainLen * 2;
                        strip->verts = malloc(strip->numVerts * sizeof(drawVert_t));
                        strip->numIndexes = 0; // SurfaceAsTristrip generates indexes at emit time

                        for (int k = 0; k < chainLen; k++) {
                            int vIdx = chain[k];

                            // Outer edge: forward order [0..chainLen-1]
                            strip->verts[k] = dsA->verts[vIdx];
                            VectorCopy(blendedNormal, strip->verts[k].normal);

                            // Inner edge: reverse order [chainLen..2*chainLen-1]
                            // Reversing closes the perimeter: last outer -> first inner
                            int innerSlot = chainLen + (chainLen - 1 - k);
                            strip->verts[innerSlot] = globalInsets[i][vIdx];
                            VectorCopy(faceNormal, strip->verts[innerSlot].normal);
                        }
                    }
                }
            }
        }
    }
}

    // Pass 3: Shrink Inner Bodies (Replaces original geometry with the inset geometry)
    for (i = e->firstDrawSurf; i < numBaseDrawSurfs; i++)
    {
        if (globalInsets[i]) {
            mapDrawSurface_t *ds = &mapDrawSurfs[i];
            
            for (int v = 0; v < ds->numVerts; v++) {
                VectorCopy(globalInsets[i][v].xyz, ds->verts[v].xyz);
                ds->verts[v].st[0] = globalInsets[i][v].st[0];
                ds->verts[v].st[1] = globalInsets[i][v].st[1];
            }
            ds->parentSurfaceNum = -1;
            free(globalInsets[i]);
        }
    }
}



/*
=============================================================================
ATOMIC UNIT TRISOUP MERGING (PHASE 1)
=============================================================================
*/

typedef struct
{
    int numVerts;
    drawVert_t *verts;
} weldBuf_t;

static int WeldMergedVertex(weldBuf_t *buf, int maxVerts, const drawVert_t *v)
{
    for (int i = 0; i < buf->numVerts; i++)
    {
        if (VectorsNearEqual(buf->verts[i].xyz, v->xyz, 0.01f) &&
            VectorsNearEqual(buf->verts[i].normal, v->normal, 0.05f) &&
            fabs(buf->verts[i].st[0] - v->st[0]) < 0.001f &&
            fabs(buf->verts[i].st[1] - v->st[1]) < 0.001f)
        {
            return i;
        }
    }
    if (buf->numVerts >= maxVerts)
    {
        return buf->numVerts - 1;
    }
    buf->verts[buf->numVerts] = *v;
    return buf->numVerts++;
}

static void GenerateAtomicUVsWithXAtlas(mapDrawSurface_t *ds)
{
    if (ds->numVerts < 3 || ds->numIndexes < 3)
        return;

    float *positions = malloc(ds->numVerts * 3 * sizeof(float));
    for (int i = 0; i < ds->numVerts; i++)
    {
        positions[i * 3 + 0] = ds->verts[i].xyz[0];
        positions[i * 3 + 1] = ds->verts[i].xyz[1];
        positions[i * 3 + 2] = ds->verts[i].xyz[2];
    }

    uint32_t *indices = malloc(ds->numIndexes * sizeof(uint32_t));
    for (int i = 0; i < ds->numIndexes; i++)
    {
        indices[i] = (uint32_t)ds->indexes[i];
    }

    xatlasMeshDecl decl;
    xatlasMeshDeclInit(&decl);
    decl.vertexPositionData = positions;
    decl.vertexPositionStride = sizeof(float) * 3;
    decl.vertexCount = (uint32_t)ds->numVerts;
    decl.indexData = indices;
    decl.indexCount = (uint32_t)ds->numIndexes;
    decl.indexFormat = xatlasIndexFormat_UInt32;

    xatlasAtlas *atlas = xatlasCreate();
    if (!atlas)
    {
        free(positions);
        free(indices);
        return;
    }

    if (xatlasAddMesh(atlas, &decl, 1) != xatlasAddMeshError_Success)
    {
        xatlasDestroy(atlas);
        free(positions);
        free(indices);
        return;
    }

    xatlasAddMeshJoin(atlas);

    xatlasChartOptions chartOpts;
    xatlasChartOptionsInit(&chartOpts);
    xatlasComputeCharts(atlas, &chartOpts);

    float area3D = 0.0f;
    for (int i = 0; i < ds->numIndexes; i += 3)
    {
        vec3_t s1, s2, cross;
        VectorSubtract(ds->verts[ds->indexes[i + 1]].xyz, ds->verts[ds->indexes[i]].xyz, s1);
        VectorSubtract(ds->verts[ds->indexes[i + 2]].xyz, ds->verts[ds->indexes[i]].xyz, s2);
        CrossProduct(s1, s2, cross);
        area3D += 0.5f * VectorLength(cross);
    }

    float sampleSizeVal = ds->samplesize > 0.0f ? ds->samplesize : (float)samplesize;
    float scaleVal = ds->lightmapScale > 0.0f ? ds->lightmapScale : 1.0f;
    int targetRes = (int)ceil(sqrt(area3D) / sampleSizeVal * scaleVal);
    if (targetRes > LIGHTMAP_WIDTH - 2)
        targetRes = LIGHTMAP_WIDTH - 2;
    if (targetRes < 16)
        targetRes = 16;

    xatlasPackOptions packOpts;
    xatlasPackOptionsInit(&packOpts);
    packOpts.padding = 2;
    packOpts.texelsPerUnit = 0.0f;
    packOpts.resolution = targetRes;
    xatlasPackCharts(atlas, &packOpts);

    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0)
    {
        xatlasDestroy(atlas);
        free(positions);
        free(indices);
        return;
    }

    xatlasMesh *xm = &atlas->meshes[0];

    // If xatlas split any vertices at UV seams, rebuild ds->verts
    if (1)
    {
        drawVert_t *newVerts = malloc(xm->vertexCount * sizeof(drawVert_t));
        for (uint32_t i = 0; i < xm->vertexCount; i++)
        {
            newVerts[i] = ds->verts[xm->vertexArray[i].xref];
        }
        free(ds->verts);
        ds->verts = newVerts;
        ds->numVerts = (int)xm->vertexCount;

        ds->numIndexes = (int)xm->indexCount;
        for (int i = 0; i < ds->numIndexes; i++)
        {
            ds->indexes[i] = (int)xm->indexArray[i];
        }
    }

    for (uint32_t i = 0; i < xm->vertexCount; i++)
    {
        ds->verts[i].lightmap[0][0] = xm->vertexArray[i].uv[0] / (float)atlas->width;
        ds->verts[i].lightmap[0][1] = xm->vertexArray[i].uv[1] / (float)atlas->height;
    }

    xatlasDestroy(atlas);
    free(positions);
    free(indices);
}

/*
==================
MergeChamferStripsIntoParents

Merges each inset planar fragment (parentSurfaceNum == -1) with its own
immediate child chamfer strips into an atomic MST_TRIANGLE_SOUP mesh.
==================
*/
void MergeChamferStripsIntoParents(entity_t *e)
{
    int i, j, k;
    int c_mergedParents = 0;
    int c_mergedStrips = 0;
    int numSurfsAtStart = numMapDrawSurfs;

    qprintf("----- MergeChamferStripsIntoParents -----\n");

    for (i = e->firstDrawSurf; i < s_chamferBaseDrawSurfs; i++)
    {
        mapDrawSurface_t *parent = &mapDrawSurfs[i];
        if (parent->numVerts < 3)
            continue;

        if (!IsChamferCandidate(parent))
            continue;



        int maxVerts = 8192;
        int maxIndexes = 32768;

        weldBuf_t welded;
        welded.numVerts = 0;
        welded.verts = malloc(maxVerts * sizeof(drawVert_t));

        int *outIndexes = malloc(maxIndexes * sizeof(int));
        int numOutIndexes = 0;

        // Step 1: Triangulate parent fragment N-gon as CCW fan from vertex 0
        for (k = 1; k <= parent->numVerts - 2; k++)
        {
            if (numOutIndexes + 3 > maxIndexes)
                break;
            outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, &parent->verts[0]);
            outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, &parent->verts[k]);
            outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, &parent->verts[k + 1]);
        }

        // Step 2: Triangulate each child chamfer strip and weld vertices
        for (j = s_chamferBaseDrawSurfs; j < numSurfsAtStart; j++)
        {
            mapDrawSurface_t *strip = &mapDrawSurfs[j];
            if (strip->parentSurfaceNum != i || strip->numVerts < 4)
                continue;

            int chainLen = strip->numVerts / 2;
            for (k = 0; k <= chainLen - 2; k++)
            {
                if (numOutIndexes + 6 > maxIndexes)
                    break;

                drawVert_t *OuterA = &strip->verts[k];
                drawVert_t *OuterB = &strip->verts[k + 1];
                drawVert_t *InnerB = &strip->verts[2 * chainLen - 2 - k];
                drawVert_t *InnerA = &strip->verts[2 * chainLen - 1 - k];

                // Tri 1 (CCW): OuterA -> OuterB -> InnerB
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, OuterA);
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, OuterB);
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, InnerB);

                // Tri 2 (CCW): OuterA -> InnerB -> InnerA
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, OuterA);
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, InnerB);
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, InnerA);
            }

            strip->numVerts = 0;
            if (strip->verts)
            {
                free(strip->verts);
                strip->verts = NULL;
            }
            if (strip->indexes)
            {
                free(strip->indexes);
                strip->indexes = NULL;
                strip->numIndexes = 0;
            }
            c_mergedStrips++;
        }

        // Step 3: Promote parent to atomic Trisoup (miscModel = qtrue)
        if (parent->verts)
            free(parent->verts);
        if (parent->indexes)
            free(parent->indexes);

        parent->verts = welded.verts;
        parent->numVerts = welded.numVerts;
        parent->indexes = outIndexes;
        parent->numIndexes = numOutIndexes;
        parent->miscModel = qtrue;
        parent->planarDerived = qtrue;

        c_mergedParents++;
    }

    qprintf("%6i atomic parents merged\n", c_mergedParents);
    qprintf("%6i child strips absorbed\n", c_mergedStrips);
}


/*
==================
MergeParentedTrisoups

Groups parented atomic trisoups (derived from planar surfaces) that share the same
plane and shader into unified meshes.
==================
*/
void MergeParentedTrisoups(entity_t *e)
{
    int i, j;
    int numSurfsAtStart = numMapDrawSurfs;
    int numMergedGroups = 0;
    qboolean *visited = malloc(numSurfsAtStart * sizeof(qboolean));
    memset(visited, 0, numSurfsAtStart * sizeof(qboolean));

    qprintf("----- MergeParentedTrisoups -----\n");

    for (i = e->firstDrawSurf; i < numSurfsAtStart; i++)
    {
        if (visited[i])
            continue;

        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        if (!dsA->planarDerived || !dsA->miscModel || dsA->numVerts <= 0)
            continue;

        int component[MAX_MAP_DRAW_SURFS_LIMIT];
        int numComponent = 0;
        
        component[numComponent++] = i;
        visited[i] = qtrue;

        for (int head = 0; head < numComponent; head++)
        {
            int currIdx = component[head];
            mapDrawSurface_t *currDs = &mapDrawSurfs[currIdx];

            for (j = i + 1; j < numSurfsAtStart; j++)
            {
                if (visited[j])
                    continue;

                mapDrawSurface_t *dsB = &mapDrawSurfs[j];
                if (!dsB->planarDerived || !dsB->miscModel || dsB->numVerts <= 0)
                    continue;

                if (currDs->planeNum != dsB->planeNum || currDs->shaderInfo != dsB->shaderInfo)
                    continue;

                qboolean canMerge = qfalse;
                if (currDs->side && dsB->side && currDs->side == dsB->side && currDs->mapBrush == dsB->mapBrush)
                {
                    canMerge = qtrue;
                }
                else
                {
                    int parentA = (currDs->parentSurfaceNum != -1) ? currDs->parentSurfaceNum : currIdx;
                    int parentB = (dsB->parentSurfaceNum != -1) ? dsB->parentSurfaceNum : j;
                    if (parentA == parentB || parentA == j || parentB == currIdx)
                    {
                        canMerge = qtrue;
                    }
                }

                if (canMerge)
                {
                    visited[j] = qtrue;
                    component[numComponent++] = j;
                }
            }
        }

        if (numComponent > 1)
        {
            int maxVerts = 32768;
            int maxIndexes = 65536;

            weldBuf_t welded;
            welded.numVerts = 0;
            welded.verts = malloc(maxVerts * sizeof(drawVert_t));

            int *outIndexes = malloc(maxIndexes * sizeof(int));
            int numOutIndexes = 0;

            for (int c = 0; c < numComponent; c++)
            {
                mapDrawSurface_t *ds = &mapDrawSurfs[component[c]];
                for (int k = 0; k < ds->numIndexes; k++)
                {
                    if (numOutIndexes >= maxIndexes)
                        break;
                    outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, &ds->verts[ds->indexes[k]]);
                }

                if (ds != dsA)
                {
                    ds->numVerts = 0;
                    if (ds->verts) free(ds->verts);
                    if (ds->indexes) free(ds->indexes);
                    ds->verts = NULL;
                    ds->indexes = NULL;
                    ds->numIndexes = 0;
                }
            }

            if (dsA->verts) free(dsA->verts);
            if (dsA->indexes) free(dsA->indexes);

            dsA->verts = welded.verts;
            dsA->numVerts = welded.numVerts;
            dsA->indexes = outIndexes;
            dsA->numIndexes = numOutIndexes;

            numMergedGroups++;
        }
        
        dsA->isPlanar = qtrue;
    }

    free(visited);
    qprintf("%6i multi-surface trisoups merged\n", numMergedGroups);
}


/*
==================
ComputeSurfaceArea3D
==================
*/
static float ComputeSurfaceArea3D(const mapDrawSurface_t *ds)
{
    float area3D = 0.0f;
    for (int i = 0; i < ds->numIndexes; i += 3)
    {
        vec3_t s1, s2, cross;
        VectorSubtract(ds->verts[ds->indexes[i + 1]].xyz, ds->verts[ds->indexes[i]].xyz, s1);
        VectorSubtract(ds->verts[ds->indexes[i + 2]].xyz, ds->verts[ds->indexes[i]].xyz, s2);
        CrossProduct(s1, s2, cross);
        area3D += 0.5f * VectorLength(cross);
    }
    return area3D;
}

/*
==================
SurfacesShareEdge
==================
*/
static qboolean SurfacesShareEdge(const mapDrawSurface_t *dsA, const mapDrawSurface_t *dsB, float epsilon)
{
    int sharedVerts = 0;
    float epsSq = epsilon * epsilon;

    for (int i = 0; i < dsA->numVerts; i++)
    {
        for (int j = 0; j < dsB->numVerts; j++)
        {
            vec3_t diff;
            VectorSubtract(dsA->verts[i].xyz, dsB->verts[j].xyz, diff);
            if (DotProduct(diff, diff) <= epsSq)
            {
                sharedVerts++;
                if (sharedVerts >= 2)
                    return qtrue;
                break;
            }
        }
    }
    return qfalse;
}

/*
==================
MergeAdjacentTrisoups

Global optimization pass that merges adjacent MST_TRIANGLE_SOUP surfaces sharing
the same shader and compatible properties, respecting lightmap size budgets.
Finally generates lightmap UVs for all surviving trisoups.
==================
*/
void MergeAdjacentTrisoups(entity_t *e)
{
    int i, j;
    int numSurfsAtStart = numMapDrawSurfs;
    int numMergedGroups = 0;
    qboolean *visited = malloc(numSurfsAtStart * sizeof(qboolean));
    memset(visited, 0, numSurfsAtStart * sizeof(qboolean));

    int *group = malloc(numSurfsAtStart * sizeof(int));

    qprintf("----- MergeAdjacentTrisoups -----\n");

    for (i = e->firstDrawSurf; i < numSurfsAtStart; i++)
    {
        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        if (visited[i])
            continue;
        if (!dsA->miscModel || dsA->numVerts <= 0 || dsA->numIndexes <= 0)
            continue;

        visited[i] = qtrue;
        int groupSize = 0;
        group[groupSize++] = i;

        float groupArea = ComputeSurfaceArea3D(dsA);
        qboolean allPlanar = dsA->isPlanar;

        for (int head = 0; head < groupSize; head++)
        {
            mapDrawSurface_t *currDs = &mapDrawSurfs[group[head]];

            for (j = e->firstDrawSurf; j < numSurfsAtStart; j++)
            {
                if (visited[j])
                    continue;

                mapDrawSurface_t *dsB = &mapDrawSurfs[j];
                if (!dsB->miscModel || dsB->numVerts <= 0 || dsB->numIndexes <= 0)
                    continue;

                if (currDs->shaderInfo != dsB->shaderInfo)
                    continue;
                if (currDs->fogNum != dsB->fogNum)
                    continue;
                if (fabs(currDs->samplesize - dsB->samplesize) > 0.001f)
                    continue;
                if (fabs(currDs->lightmapScale - dsB->lightmapScale) > 0.001f)
                    continue;

                if (!SurfacesShareEdge(currDs, dsB, 0.01f))
                    continue;

                float candidateArea = groupArea + ComputeSurfaceArea3D(dsB);
                float sampleSizeVal = dsA->samplesize > 0.0f ? dsA->samplesize : (float)samplesize;
                float scaleVal = dsA->lightmapScale > 0.0f ? dsA->lightmapScale : 1.0f;
                int targetRes = (int)ceil(sqrt(candidateArea) / sampleSizeVal * scaleVal);

                int limit = LIGHTMAP_WIDTH - 2;
                if (!dsA->enforceSampleSize)
                    limit *= 2;

                if (targetRes > limit)
                    continue;

                visited[j] = qtrue;
                group[groupSize++] = j;
                groupArea = candidateArea;

                if (!dsB->isPlanar || dsB->planeNum != dsA->planeNum)
                    allPlanar = qfalse;
            }
        }

        if (groupSize > 1)
        {
            int totalVerts = 0;
            int totalIndexes = 0;
            for (int g = 0; g < groupSize; g++)
            {
                totalVerts += mapDrawSurfs[group[g]].numVerts;
                totalIndexes += mapDrawSurfs[group[g]].numIndexes;
            }

            weldBuf_t welded;
            welded.numVerts = 0;
            welded.verts = malloc(totalVerts * sizeof(drawVert_t));

            int *outIndexes = malloc(totalIndexes * sizeof(int));
            int numOutIndexes = 0;

            for (int g = 0; g < groupSize; g++)
            {
                mapDrawSurface_t *ds = &mapDrawSurfs[group[g]];
                for (int idx = 0; idx < ds->numIndexes; idx++)
                {
                    outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, totalVerts, &ds->verts[ds->indexes[idx]]);
                }

                if (g > 0)
                {
                    if (ds->verts) free(ds->verts);
                    if (ds->indexes) free(ds->indexes);
                    ds->verts = NULL;
                    ds->indexes = NULL;
                    ds->numVerts = 0;
                    ds->numIndexes = 0;
                }
            }

            if (dsA->verts) free(dsA->verts);
            if (dsA->indexes) free(dsA->indexes);

            dsA->verts = welded.verts;
            dsA->numVerts = welded.numVerts;
            dsA->indexes = outIndexes;
            dsA->numIndexes = numOutIndexes;

            dsA->isPlanar = allPlanar;
            numMergedGroups++;
        }
    }

    for (i = e->firstDrawSurf; i < numSurfsAtStart; i++)
    {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];
        if (!ds->miscModel || ds->numVerts <= 0 || ds->numIndexes <= 0)
            continue;
        GenerateAtomicUVsWithXAtlas(ds);
    }

    free(group);
    free(visited);
    qprintf("%6i adjacent trisoup groups merged\n", numMergedGroups);
}
