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

#ifdef DECIMATE_PLANAR_WITH_MESHLIB
#include "../libs/MeshLib-Lite/MRMeshC/MRMeshC.h"
#endif

static int s_chamferBaseDrawSurfs = 0;

qboolean VectorsNearEqual(const vec3_t a, const vec3_t b, float epsilon)
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

surfaceNeighbor_t **surfaceNeighbors;

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
qboolean IsEdgeSharingCandidate(mapDrawSurface_t *ds)
{
    if (ds->numVerts <= 0 || ds->side == NULL) return qfalse;
    if (ds->patch || ds->miscModel || ds->flareSurface || ds->isDecal) return qfalse;
    if (ds->shaderInfo && (ds->shaderInfo->surfaceFlags & (SURF_NODRAW | SURF_SKY | SURF_NOLIGHTMAP))) return qfalse;
    if (ds->shaderInfo && (ds->shaderInfo->contents & (CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_FOG))) return qfalse;
    return qtrue;
}

static int ClassifySurfaceSide(const mapDrawSurface_t *ds, const vec3_t normal, float dist)
{
    qboolean front = qfalse;
    qboolean back  = qfalse;
    int i;

    for (i = 0; i < ds->numVerts; i++) {
        float d = DotProduct(ds->verts[i].xyz, normal) - dist;
        if      (d >  LINE_POSITION_EPSILON) front = qtrue;
        else if (d < -LINE_POSITION_EPSILON) back  = qtrue;
    }

    if (front && !back) return SIDE_FRONT;
    if (back && !front) return SIDE_BACK;
    if (front &&  back) return SIDE_CROSS;
    return SIDE_ON; // all vertices exactly on the plane (degenerate, treated as CROSS to be safe)
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
    memset(surfaceNeighbors, 0, game->maxMapDrawSurfs * sizeof(surfaceNeighbor_t *));

    for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++)
    {
        dsA = &mapDrawSurfs[i];
        if (!IsEdgeSharingCandidate(dsA)) continue;

        for (j = i + 1; j < numMapDrawSurfs; j++)
        {
            dsB = &mapDrawSurfs[j];
            if (!IsEdgeSharingCandidate(dsB)) continue;

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
                // Reject near-parallel and anti-parallel surfaces
                vec3_t normalA, normalB;
                float distA = mapplanes[dsA->side->planenum].dist;
                float distB = mapplanes[dsB->side->planenum].dist;
                VectorCopy(mapplanes[dsA->side->planenum].normal, normalA);
                VectorCopy(mapplanes[dsB->side->planenum].normal, normalB);

                float dot = DotProduct(normalA, normalB);
                if (dot > 0.866f || dot < -0.866f) continue; // < 30 degrees or anti-parallel

                // Classify where each face's interior sits relative to the other's plane
                int sideA = ClassifySurfaceSide(dsA, normalB, distB);
                int sideB = ClassifySurfaceSide(dsB, normalA, distA);

                // Accept only: Convex (BACK/BACK) or Concave (FRONT/FRONT)
                // Reject: mixed (FRONT/BACK or BACK/FRONT), CROSS, or ON
                qboolean isSymmetric = ((sideA == SIDE_BACK  && sideB == SIDE_BACK)  ||
                                        (sideA == SIDE_FRONT && sideB == SIDE_FRONT));
                if (!isSymmetric) continue;

                qboolean isConcave = (sideA == SIDE_FRONT && sideB == SIDE_FRONT);

                surfaceNeighbor_t *newNbA = malloc(sizeof(surfaceNeighbor_t));
                *newNbA = nbA;
                newNbA->neighborSurfaceNum = j;
                newNbA->isConcave = isConcave;
                newNbA->next = surfaceNeighbors[i];
                surfaceNeighbors[i] = newNbA;

                surfaceNeighbor_t *newNbB = malloc(sizeof(surfaceNeighbor_t));
                *newNbB = nbB;
                newNbB->neighborSurfaceNum = i;
                newNbB->isConcave = isConcave;
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
static void ComputeAllInsets(mapDrawSurface_t *ds, surfaceChamferEdge_t *edges, int numEdges, drawVert_t *insetMap)
{
    int i, j, v;
    vec3_t faceNormal;
    qboolean edgeChamfered[MAX_CHAMFER_VERTS];
    float edgeWidth[MAX_CHAMFER_VERTS];
    vec3_t edgeLineOrigin[MAX_CHAMFER_VERTS];
    vec3_t edgeLineDir[MAX_CHAMFER_VERTS];
    
    memset(edgeChamfered, 0, sizeof(edgeChamfered));
    memset(edgeWidth, 0, sizeof(edgeWidth));
    VectorCopy(mapplanes[ds->side->planenum].normal, faceNormal);

    for (i = 0; i < numEdges; i++) {
        surfaceChamferEdge_t *edge = &edges[i];
        for (j = 0; j < edge->chainLen - 1; j++) {
            edgeChamfered[edge->chainIndices[j]] = qtrue;
            edgeWidth[edge->chainIndices[j]] = edge->width;
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
            VectorMA(ds->verts[v].xyz, edgeWidth[v], insetDir, edgeLineOrigin[v]);
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
                    VectorMA(ds->verts[v].xyz, edgeWidth[v], insetDir, insetMap[v].xyz);
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
                    VectorMA(ds->verts[v].xyz, edgeWidth[prev_v], insetDir, insetMap[v].xyz);
                }
            }
            
            float maxMove = ((edgeWidth[prev_v] > edgeWidth[v]) ? edgeWidth[prev_v] : edgeWidth[v]) * 4.0f;
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

        if (!IsEdgeSharingCandidate(dsA)) continue;

        for (j = e->firstDrawSurf; j < numMapDrawSurfs && !chopped; j++)
        {
            mapDrawSurface_t *dsB;
            vec3_t normalA, normalB;
            float dot;

            if (i == j) continue;
            dsB = &mapDrawSurfs[j];
            if (!IsEdgeSharingCandidate(dsB)) continue;

            // Never slice an opaque surface across its face due to a transparent surface touching it (and vice-versa)
            qboolean transA = (dsA->shaderInfo && (dsA->shaderInfo->contents & CONTENTS_TRANSLUCENT)) ? qtrue : qfalse;
            qboolean transB = (dsB->shaderInfo && (dsB->shaderInfo->contents & CONTENTS_TRANSLUCENT)) ? qtrue : qfalse;
            if (transA != transB) continue;

            VectorCopy(mapplanes[dsA->side->planenum].normal, normalA);
            VectorCopy(mapplanes[dsB->side->planenum].normal, normalB);
            dot = DotProduct(normalA, normalB);
            if (dot > 0.866f || dot < -0.866f) continue;

            // Reject mismatched backside contacts (`FRONT/BACK` or `BACK/FRONT`).
            // Allows valid Convex (`BACK/BACK`), Concave (`FRONT/FRONT`), and spanning (`CROSS/*`) splits.
            {
                float distA = mapplanes[dsA->side->planenum].dist;
                float distB = mapplanes[dsB->side->planenum].dist;
                int sideA = ClassifySurfaceSide(dsA, normalB, distB);
                int sideB = ClassifySurfaceSide(dsB, normalA, distA);

                if ((sideA == SIDE_FRONT && sideB == SIDE_BACK) ||
                    (sideA == SIDE_BACK  && sideB == SIDE_FRONT))
                {
                    continue; // Mismatched backside T-junction contact
                }
            }

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

                // --- NEW LOGIC: Enforce Original Edge Check ---
                // If dsA is already a fragment (split by an earlier chop), do not allow further chops along
                // newly created internal edges (`V0->V1`). Only allow chops along true brush boundary edges.
                if (dsA->parentSurfaceNum != -1 && !IsOriginalBrushEdge(dsA, V0, V1)) {
                    continue;
                }
                // ----------------------------------------------

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

/*
================
ComputeVertexBlendedNormal
For a given vertex vIdx on surface surfIdx, compute the true 3D blended normal by
summing the pristine geometric plane normals of dsA plus every validated chamfer
neighbor that shares this exact vertex.
================
*/
static void ComputeVertexBlendedNormal(int surfIdx, int vIdx, const vec3_t faceNormal, vec3_t out)
{
    surfaceNeighbor_t *scanNb;
    int m;

    VectorCopy(faceNormal, out);

    for (scanNb = surfaceNeighbors[surfIdx]; scanNb; scanNb = scanNb->next) {
        for (m = 0; m < scanNb->sharedChainLen; m++) {
            if (scanNb->sharedChainIndicesA[m] == vIdx) {
                vec3_t n;
                VectorCopy(mapplanes[mapDrawSurfs[scanNb->neighborSurfaceNum].side->planenum].normal, n);
                VectorAdd(out, n, out);
                break;
            }
        }
    }

    VectorNormalize(out, out);
}

/*
================
InsertVertexIntoDrawSurf
================
*/
static void InsertVertexIntoDrawSurf(mapDrawSurface_t *ds, int insertIdx, const drawVert_t *newVert)
{
    drawVert_t *newBuffer = malloc((ds->numVerts + 1) * sizeof(drawVert_t));

    if (insertIdx > 0)
        memcpy(newBuffer, ds->verts, insertIdx * sizeof(drawVert_t));

    newBuffer[insertIdx] = *newVert;

    if (insertIdx < ds->numVerts)
        memcpy(&newBuffer[insertIdx + 1], &ds->verts[insertIdx],
               (ds->numVerts - insertIdx) * sizeof(drawVert_t));

    free(ds->verts);
    ds->verts = newBuffer;
    ds->numVerts++;
}

/*
================
InsertCollinearVertices
================
*/
void InsertCollinearVertices(entity_t *e, float minDot, float maxDot, int targetEntityNum)
{
    int i, j, v, w;

    qprintf("----- InsertCollinearVertices -----\n");

    for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++)
    {
        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        if (targetEntityNum >= 0)
        {
            if (dsA->mapBrush == NULL || dsA->mapBrush->entitynum != targetEntityNum) continue;
        }
        if (!IsEdgeSharingCandidate(dsA)) continue;

        for (j = i + 1; j < numMapDrawSurfs; j++)
        {
            mapDrawSurface_t *dsB = &mapDrawSurfs[j];
            if (targetEntityNum >= 0)
            {
                if (dsB->mapBrush == NULL || dsB->mapBrush->entitynum != targetEntityNum) continue;
            }
            if (!IsEdgeSharingCandidate(dsB)) continue;

            qboolean transA = (dsA->shaderInfo && (dsA->shaderInfo->contents & CONTENTS_TRANSLUCENT)) ? qtrue : qfalse;
            qboolean transB = (dsB->shaderInfo && (dsB->shaderInfo->contents & CONTENTS_TRANSLUCENT)) ? qtrue : qfalse;
            if (transA != transB) continue;

            vec3_t normalA, normalB;
            VectorCopy(mapplanes[dsA->side->planenum].normal, normalA);
            VectorCopy(mapplanes[dsB->side->planenum].normal, normalB);
            float dot = DotProduct(normalA, normalB);
            if (dot < minDot || dot > maxDot) continue;

            // Direction 1: Vertices of dsA -> Edges of dsB
            for (v = 0; v < dsA->numVerts; v++)
            {
                vec3_t vA;
                VectorCopy(dsA->verts[v].xyz, vA);

                qboolean alreadyInB = qfalse;
                int check;
                for (check = 0; check < dsB->numVerts; check++) {
                    if (VectorsNearEqual(vA, dsB->verts[check].xyz, 0.1f)) {
                        alreadyInB = qtrue;
                        break;
                    }
                }
                if (alreadyInB) continue;

                for (w = 0; w < dsB->numVerts; w++)
                {
                    int next_w = (w + 1) % dsB->numVerts;
                    vec3_t W0, W1, edgeDir, toA, perp;
                    float full_len, t, perp_dist;

                    VectorCopy(dsB->verts[w].xyz, W0);
                    VectorCopy(dsB->verts[next_w].xyz, W1);

                    VectorSubtract(W1, W0, edgeDir);
                    full_len = VectorNormalize(edgeDir, edgeDir);
                    if (full_len < 0.1f) continue;

                    VectorSubtract(vA, W0, toA);
                    t = DotProduct(toA, edgeDir);

                    if (t < 0.1f || t > full_len - 0.1f) continue;

                    vec3_t proj;
                    VectorScale(edgeDir, t, proj);
                    VectorSubtract(toA, proj, perp);
                    perp_dist = VectorLength(perp);

                    if (perp_dist > 0.25f) continue;

                    if (!IsOriginalBrushEdge(dsB, W0, W1)) continue;

                    float frac = t / full_len;

                    drawVert_t newVert;
                    memset(&newVert, 0, sizeof(newVert));

                    VectorCopy(vA, newVert.xyz);
                    newVert.st[0] = dsB->verts[w].st[0] + frac * (dsB->verts[next_w].st[0] - dsB->verts[w].st[0]);
                    newVert.st[1] = dsB->verts[w].st[1] + frac * (dsB->verts[next_w].st[1] - dsB->verts[w].st[1]);
                    VectorCopy(dsB->verts[w].normal, newVert.normal);
                    memcpy(newVert.color, dsB->verts[w].color, sizeof(newVert.color));

                    InsertVertexIntoDrawSurf(dsB, w + 1, &newVert);
                    w++;
                    break;
                }
            }

            // Direction 2: Vertices of dsB -> Edges of dsA
            for (w = 0; w < dsB->numVerts; w++)
            {
                vec3_t vB;
                VectorCopy(dsB->verts[w].xyz, vB);

                qboolean alreadyInA = qfalse;
                int check;
                for (check = 0; check < dsA->numVerts; check++) {
                    if (VectorsNearEqual(vB, dsA->verts[check].xyz, 0.1f)) {
                        alreadyInA = qtrue;
                        break;
                    }
                }
                if (alreadyInA) continue;

                for (v = 0; v < dsA->numVerts; v++)
                {
                    int next_v = (v + 1) % dsA->numVerts;
                    vec3_t V0, V1, edgeDir, toB, perp;
                    float full_len, t, perp_dist;

                    VectorCopy(dsA->verts[v].xyz, V0);
                    VectorCopy(dsA->verts[next_v].xyz, V1);

                    VectorSubtract(V1, V0, edgeDir);
                    full_len = VectorNormalize(edgeDir, edgeDir);
                    if (full_len < 0.1f) continue;

                    VectorSubtract(vB, V0, toB);
                    t = DotProduct(toB, edgeDir);
                    if (t < 0.1f || t > full_len - 0.1f) continue;

                    vec3_t proj;
                    VectorScale(edgeDir, t, proj);
                    VectorSubtract(toB, proj, perp);
                    perp_dist = VectorLength(perp);
                    if (perp_dist > 0.25f) continue;

                    if (!IsOriginalBrushEdge(dsA, V0, V1)) continue;

                    float frac = t / full_len;

                    drawVert_t newVert;
                    memset(&newVert, 0, sizeof(newVert));

                    VectorCopy(vB, newVert.xyz);
                    newVert.st[0] = dsA->verts[v].st[0] + frac * (dsA->verts[next_v].st[0] - dsA->verts[v].st[0]);
                    newVert.st[1] = dsA->verts[v].st[1] + frac * (dsA->verts[next_v].st[1] - dsA->verts[v].st[1]);
                    VectorCopy(dsA->verts[v].normal, newVert.normal);
                    memcpy(newVert.color, dsA->verts[v].color, sizeof(newVert.color));

                    InsertVertexIntoDrawSurf(dsA, v + 1, &newVert);
                    v++;
                    break;
                }
            }
        }
    }
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
    drawVert_t **globalInsets = malloc(game->maxMapDrawSurfs * sizeof(drawVert_t*));
    memset(globalInsets, 0, game->maxMapDrawSurfs * sizeof(drawVert_t*));
    
    qprintf("----- ChamferSurfaceEdges (V3: Normal Bending) -----\n");
    InsertCollinearVertices(e, -0.866f, 0.866f, -1);
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
        
        if (!IsEdgeSharingCandidate(dsA)) continue;

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

        for (nb = surfaceNeighbors[i]; nb; nb = nb->next)
        {
            int j = nb->neighborSurfaceNum;
            mapDrawSurface_t *dsB = &mapDrawSurfs[j];
            vec3_t normalA, normalB;
            float dot;
            
            float cw = dsA->chamferConvexWidth >= 0.0f ? dsA->chamferConvexWidth : game->chamferConvexWidth;
            float ccw = dsA->chamferConcaveWidth >= 0.0f ? dsA->chamferConcaveWidth : game->chamferConcaveWidth;
            float target_width = (nb->isConcave && ccw >= 0.0f) ? ccw : cw;
            if (target_width < MIN_CHAMFER_WIDTH) continue;

            float edge_width = target_width;
            if (min_edge < 3.0f * target_width) {
                edge_width = min_edge / 3.0f;
            }
            if (edge_width < MIN_CHAMFER_WIDTH) continue;

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
                            edges[numEdges].width = edge_width;
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
            ComputeAllInsets(dsA, edges, numEdges, globalInsets[i]);
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

        for (nb = surfaceNeighbors[i]; nb; nb = nb->next)
        {
            float cw = dsA->chamferConvexWidth >= 0.0f ? dsA->chamferConvexWidth : game->chamferConvexWidth;
            float ccw = dsA->chamferConcaveWidth >= 0.0f ? dsA->chamferConcaveWidth : game->chamferConcaveWidth;
            float target_width = (nb->isConcave && ccw >= 0.0f) ? ccw : cw;
            if (target_width < MIN_CHAMFER_WIDTH) continue;
            if (min_edge < 3.0f * target_width && (min_edge / 3.0f) < MIN_CHAMFER_WIDTH) continue;

            int j = nb->neighborSurfaceNum;
            mapDrawSurface_t *dsB = &mapDrawSurfs[j];
            vec3_t normalB;
            float dot;
            
            VectorCopy(mapplanes[dsB->side->planenum].normal, normalB);
            dot = DotProduct(faceNormal, normalB);
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
                            ComputeVertexBlendedNormal(i, vIdx, faceNormal, strip->verts[k].normal);

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
    
    free(globalInsets);
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

    float sampleSizeVal = ds->samplesize > 0.0f ? ds->samplesize : (float)game->defaultSampleSize;
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

        if (!IsEdgeSharingCandidate(parent))
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

        parent->numParentIndexes = numOutIndexes;

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

        int *component = malloc(game->maxMapDrawSurfs * sizeof(int));
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

        free(component);
        
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
PointOnSegment

Returns true if point pt is orthogonally within epsilon of line segment a->b
and the projection point lies strictly between a and b.
==================
*/
static qboolean PointOnSegment(const vec3_t pt, const vec3_t a, const vec3_t b, float epsilon)
{
    vec3_t ab, apt;
    VectorSubtract(b, a, ab);
    VectorSubtract(pt, a, apt);
    
    float lenSq = DotProduct(ab, ab);
    if (lenSq < 0.0001f)
        return qfalse; // degenerate edge
        
    float t = DotProduct(apt, ab) / lenSq;
    
    // Calculate 3D margin equivalent to epsilon
    float tMargin = epsilon / sqrt(lenSq);
    
    // Check if projection falls on the segment (allowing epsilon margin)
    if (t < -tMargin || t > 1.0f + tMargin)
        return qfalse;
        
    // Calculate orthogonal distance
    vec3_t proj, diff;
    VectorMA(a, t, ab, proj);
    VectorSubtract(pt, proj, diff);
    
    return DotProduct(diff, diff) <= (epsilon * epsilon);
}

/*
==================
SurfacesTouchLoosely

Checks if dsA and dsB physically touch (even via T-junctions) by checking if 
vertices of one lie exactly on the edges of the other. 
Requires at least 2 contact points to confirm a shared boundary.
==================
*/
static qboolean SurfacesTouchLoosely(const mapDrawSurface_t *dsA, const mapDrawSurface_t *dsB, float epsilon)
{
    int contacts = 0;

    // 1. Check A's vertices against B's edges
    for (int i = 0; i < dsA->numVerts; i++)
    {
        for (int j = 0; j < dsB->numIndexes; j += 3)
        {
            // Check all 3 edges of the triangle
            if (PointOnSegment(dsA->verts[i].xyz, dsB->verts[dsB->indexes[j]].xyz, dsB->verts[dsB->indexes[j+1]].xyz, epsilon) ||
                PointOnSegment(dsA->verts[i].xyz, dsB->verts[dsB->indexes[j+1]].xyz, dsB->verts[dsB->indexes[j+2]].xyz, epsilon) ||
                PointOnSegment(dsA->verts[i].xyz, dsB->verts[dsB->indexes[j+2]].xyz, dsB->verts[dsB->indexes[j]].xyz, epsilon))
            {
                contacts++;
                if (contacts >= 2) return qtrue;
                break; // move to next vertex of A
            }
        }
    }

    // 2. Check B's vertices against A's edges
    for (int i = 0; i < dsB->numVerts; i++)
    {
        for (int j = 0; j < dsA->numIndexes; j += 3)
        {
            if (PointOnSegment(dsB->verts[i].xyz, dsA->verts[dsA->indexes[j]].xyz, dsA->verts[dsA->indexes[j+1]].xyz, epsilon) ||
                PointOnSegment(dsB->verts[i].xyz, dsA->verts[dsA->indexes[j+1]].xyz, dsA->verts[dsA->indexes[j+2]].xyz, epsilon) ||
                PointOnSegment(dsB->verts[i].xyz, dsA->verts[dsA->indexes[j+2]].xyz, dsA->verts[dsA->indexes[j]].xyz, epsilon))
            {
                contacts++;
                if (contacts >= 2) return qtrue;
                break; // move to next vertex of B
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
        vec3_t groupMins, groupMaxs;
        ClearBounds(groupMins, groupMaxs);
        for (int v = 0; v < dsA->numVerts; v++)
            AddPointToBounds(dsA->verts[v].xyz, groupMins, groupMaxs);

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

                if (currDs->smoothgroup[0] || dsB->smoothgroup[0])
                {
                    if (Q_stricmp(currDs->smoothgroup, dsB->smoothgroup) != 0)
                        continue;
                }
                
                float sampleSizeA = currDs->samplesize > 0.0f ? currDs->samplesize : (float)game->defaultSampleSize;
                float sampleSizeB = dsB->samplesize > 0.0f ? dsB->samplesize : (float)game->defaultSampleSize;
                if (fabs(sampleSizeA - sampleSizeB) > 0.001f)
                    continue;
                
                float scaleA = currDs->lightmapScale > 0.0f ? currDs->lightmapScale : 1.0f;
                float scaleB = dsB->lightmapScale > 0.0f ? dsB->lightmapScale : 1.0f;
                if (fabs(scaleA - scaleB) > 0.001f)
                    continue;

                if (currDs->overrideVertexColor != dsB->overrideVertexColor)
                    continue;
                if (currDs->overrideVertexColor && !VectorCompare(currDs->vertexColor, dsB->vertexColor))
                    continue;
                if (currDs->overrideVertexAlpha != dsB->overrideVertexAlpha)
                    continue;
                if (currDs->overrideVertexAlpha && fabs(currDs->vertexAlpha - dsB->vertexAlpha) > 0.001f)
                    continue;

                if (!SurfacesTouchLoosely(currDs, dsB, 0.1f))
                    continue;

                float candidateArea = groupArea + ComputeSurfaceArea3D(dsB);
                float sampleSizeVal = dsA->samplesize > 0.0f ? dsA->samplesize : (float)game->defaultSampleSize;
                float scaleVal = dsA->lightmapScale > 0.0f ? dsA->lightmapScale : 1.0f;
                int limit = LIGHTMAP_WIDTH - 2;

                // 1. Check 60% Area Rule
                float maxArea = (limit * limit) * 0.6f;
                float candidateAreaRes = (candidateArea / (sampleSizeVal * sampleSizeVal)) * (scaleVal * scaleVal);
                if (candidateAreaRes > maxArea)
                    continue;

                // 2. Check Linear Length Rule via Bounding Box
                vec3_t candidateMins, candidateMaxs;
                VectorCopy(groupMins, candidateMins);
                VectorCopy(groupMaxs, candidateMaxs);
                for (int v = 0; v < dsB->numVerts; v++)
                    AddPointToBounds(dsB->verts[v].xyz, candidateMins, candidateMaxs);
                
                vec3_t size;
                VectorSubtract(candidateMaxs, candidateMins, size);
                float maxDimension = size[0];
                if (size[1] > maxDimension) maxDimension = size[1];
                if (size[2] > maxDimension) maxDimension = size[2];

                if ((maxDimension / sampleSizeVal * scaleVal) > limit)
                    continue;

                visited[j] = qtrue;
                group[groupSize++] = j;
                groupArea = candidateArea;
                VectorCopy(candidateMins, groupMins);
                VectorCopy(candidateMaxs, groupMaxs);

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

    free(group);
    free(visited);
    qprintf("%6i adjacent trisoup groups merged\n", numMergedGroups);
}

/*
==================
CleanupSingleTrisoup
==================
*/
static void CleanupSingleTrisoup(mapDrawSurface_t *ds)
{
    if (ds->numVerts < 3 || ds->numIndexes < 9) return;

    // Purge degenerate and duplicate triangles from ds->indexes upfront.
    int cleanNumTris = 0;
    for (int t = 0; t < ds->numIndexes / 3; t++) {
        int i0 = ds->indexes[t*3+0];
        int i1 = ds->indexes[t*3+1];
        int i2 = ds->indexes[t*3+2];
        if (i0 == i1 || i1 == i2 || i2 == i0) continue; // topological degenerate

        // Check for geometric degenerate (zero-area collinear slivers)
        vec3_t e1, e2, cross;
        VectorSubtract(ds->verts[i1].xyz, ds->verts[i0].xyz, e1);
        VectorSubtract(ds->verts[i2].xyz, ds->verts[i0].xyz, e2);
        CrossProduct(e1, e2, cross);
        if (VectorLength(cross) < 0.1f) continue; // geometric degenerate

        // Check against already added clean triangles
        qboolean isDup = qfalse;
        for (int ct = 0; ct < cleanNumTris; ct++) {
            int j0 = ds->indexes[ct*3+0];
            int j1 = ds->indexes[ct*3+1];
            int j2 = ds->indexes[ct*3+2];
            if ((i0 == j0 && i1 == j1 && i2 == j2) ||
                (i0 == j1 && i1 == j2 && i2 == j0) ||
                (i0 == j2 && i1 == j0 && i2 == j1)) {
                isDup = qtrue; break;
            }
        }
        if (isDup) continue;

        ds->indexes[cleanNumTris*3+0] = i0;
        ds->indexes[cleanNumTris*3+1] = i1;
        ds->indexes[cleanNumTris*3+2] = i2;
        cleanNumTris++;
    }
    ds->numIndexes = cleanNumTris * 3;
}

/*
==================
CleanupAllTrisoups
==================
*/
void CleanupAllTrisoups(entity_t *e)
{
    int beforeTris = 0;
    int afterTris = 0;

    for (int i = e->firstDrawSurf; i < numMapDrawSurfs; i++) {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];
        if (!ds->miscModel || ds->numVerts < 3 || ds->numIndexes < 9)
            continue;
        beforeTris += ds->numIndexes / 3;
        CleanupSingleTrisoup(ds);
        afterTris += ds->numIndexes / 3;
    }
    
    if (beforeTris > afterTris) {
        qprintf("%6i duplicate/degenerate triangles purged\n", beforeTris - afterTris);
    }
}

/*
==================
DecimateSingleTrisoup
==================
*/
static void DecimateSingleTrisoup(mapDrawSurface_t *ds)
{
    if (ds->numVerts < 3 || ds->numIndexes < 9) return; // need at least 3 triangles

    int numTris = ds->numIndexes / 3;
    int oldNumVerts = ds->numVerts; // Save original count for clean memory deallocation

    // STEP 1: Precompute geometric face normal for each triangle.
    vec3_t *faceNormals = malloc(numTris * sizeof(vec3_t));
    for (int t = 0; t < numTris; t++) {
        int i0 = ds->indexes[t*3+0];
        int i1 = ds->indexes[t*3+1];
        int i2 = ds->indexes[t*3+2];
        vec3_t e1, e2, cross;
        VectorSubtract(ds->verts[i1].xyz, ds->verts[i0].xyz, e1);
        VectorSubtract(ds->verts[i2].xyz, ds->verts[i0].xyz, e2);
        CrossProduct(e1, e2, cross);
        if (VectorNormalize(cross, cross) < 1e-6f) {
            VectorClear(faceNormals[t]);   // degenerate triangle, normal = (0,0,0)
        } else {
            VectorCopy(cross, faceNormals[t]);
        }
    }

    // STEP 2: Build vertToTris adjacency list in O(numIndexes).
    int *vertToTriCount = calloc(oldNumVerts, sizeof(int));
    for (int i = 0; i < ds->numIndexes; i++)
        vertToTriCount[ds->indexes[i]]++;

    int **vertToTriList = malloc(oldNumVerts * sizeof(int*));
    for (int v = 0; v < oldNumVerts; v++)
        vertToTriList[v] = malloc(vertToTriCount[v] * sizeof(int));

    int *triInsert = calloc(oldNumVerts, sizeof(int));
    for (int t = 0; t < numTris; t++) {
        for (int k = 0; k < 3; k++) {
            int v = ds->indexes[t*3+k];
            vertToTriList[v][triInsert[v]++] = t;
        }
    }
    free(triInsert);

    // STEP 3: Build undirected edge reference count map in O(numIndexes).
    typedef struct { int v0, v1, count; } EdgeRef;
    int maxEdges = numTris * 3;
    EdgeRef *edges = malloc(maxEdges * sizeof(EdgeRef));
    int numEdges = 0;

    for (int t = 0; t < numTris; t++) {
        for (int k = 0; k < 3; k++) {
            int va = ds->indexes[t*3 + k];
            int vb = ds->indexes[t*3 + ((k+1)%3)];
            if (va > vb) { int tmp = va; va = vb; vb = tmp; } // normalize

            int found = 0;
            for (int e = 0; e < numEdges; e++) {
                if (edges[e].v0 == va && edges[e].v1 == vb) {
                    edges[e].count++;
                    found = 1;
                    break;
                }
            }
            if (!found && numEdges < maxEdges) {
                edges[numEdges].v0 = va;
                edges[numEdges].v1 = vb;
                edges[numEdges].count = 1;
                numEdges++;
            }
        }
    }

    // STEP 4: Classify vertices as FREE or LOCKED.
    qboolean *isFree = malloc(oldNumVerts * sizeof(qboolean));
    for (int v = 0; v < oldNumVerts; v++) isFree[v] = qtrue;

    // Lock boundary vertices from the edge table
    for (int e = 0; e < numEdges; e++) {
        if (edges[e].count < 2) {
            isFree[edges[e].v0] = qfalse;
            isFree[edges[e].v1] = qfalse;
        }
    }

    // Lock multi-plane, chamfer, and chamfer-adjacent vertices
    for (int v = 0; v < oldNumVerts; v++) {
        if (!isFree[v]) continue;
        if (vertToTriCount[v] == 0) { isFree[v] = qfalse; continue; }

        vec3_t refN;
        VectorCopy(faceNormals[vertToTriList[v][0]], refN);

        for (int ti = 0; ti < vertToTriCount[v]; ti++) {
            int t = vertToTriList[v][ti];
            // Check all triangles in 1-ring are coplanar to refN
            if (DotProduct(faceNormals[t], refN) < 0.999f) {
                isFree[v] = qfalse; break;
            }
            // Check all vertices connected to this triangle:
            // If any connected vertex normal is not equal to the surface normal refN,
            // the vertex is NOT accepted for removal.
            int *idx = &ds->indexes[t * 3];
            for (int k = 0; k < 3; k++) {
                int u = idx[k];
                if (fabs(DotProduct(ds->verts[u].normal, refN)) < 0.999f) {
                    isFree[v] = qfalse; break;
                }
            }
            if (!isFree[v]) break;
        }
    }

    // STEP 5: Greedy shortest-edge collapse loop.
    qboolean *triAlive = malloc(numTris * sizeof(qboolean));
    for (int t = 0; t < numTris; t++) triAlive[t] = qtrue;

    qboolean collapsedAny = qtrue;
    while (collapsedAny) {
        collapsedAny = qfalse;
        float bestLenSq = 1e30f;
        int bestV = -1, bestU = -1;

        for (int e = 0; e < numEdges; e++) {
            for (int dir = 0; dir < 2; dir++) {
                int v = (dir == 0) ? edges[e].v0 : edges[e].v1;
                int u = (dir == 0) ? edges[e].v1 : edges[e].v0;

                if (!isFree[v]) continue;

                // Gate 2: Link Condition
                int sharedCount = 0;
                for (int tv = 0; tv < vertToTriCount[v]; tv++) {
                    int t = vertToTriList[v][tv];
                    if (!triAlive[t]) continue;
                    int *idx = &ds->indexes[t*3];
                    qboolean hasU = (idx[0]==u || idx[1]==u || idx[2]==u);
                    if (!hasU) continue;
                    for (int k = 0; k < 3; k++) {
                        if (idx[k] != v && idx[k] != u) sharedCount++;
                    }
                }
                if (sharedCount != 2) continue; // non-manifold or boundary

                // Gate 1: No Triangle Flip
                qboolean flipped = qfalse;
                for (int tv = 0; tv < vertToTriCount[v]; tv++) {
                    int t = vertToTriList[v][tv];
                    if (!triAlive[t]) continue;
                    int *idx = &ds->indexes[t*3];
                    if (idx[0]==u || idx[1]==u || idx[2]==u) continue;

                    float *p0 = ds->verts[(idx[0]==v)?u:idx[0]].xyz;
                    float *p1 = ds->verts[(idx[1]==v)?u:idx[1]].xyz;
                    float *p2 = ds->verts[(idx[2]==v)?u:idx[2]].xyz;

                    vec3_t e1, e2, newCross;
                    VectorSubtract(p1, p0, e1);
                    VectorSubtract(p2, p0, e2);
                    CrossProduct(e1, e2, newCross);

                    if (VectorLength(newCross) > 1e-5f) {
                        if (DotProduct(newCross, faceNormals[t]) <= 0.0f) {
                            flipped = qtrue; break;
                        }
                    }
                }
                if (flipped) continue;

                vec3_t diff;
                VectorSubtract(ds->verts[v].xyz, ds->verts[u].xyz, diff);
                float lenSq = DotProduct(diff, diff);
                if (lenSq < bestLenSq) {
                    bestLenSq = lenSq;
                    bestV = v;
                    bestU = u;
                }
            }
        }

        if (bestV == -1) break;

        // Execute Collapse: bestV -> bestU
        for (int i = 0; i < ds->numIndexes; i++) {
            if (ds->indexes[i] == bestV)
                ds->indexes[i] = bestU;
        }

        for (int tv = 0; tv < vertToTriCount[bestV]; tv++) {
            int t = vertToTriList[bestV][tv];
            if (!triAlive[t]) continue;
            int *idx = &ds->indexes[t*3];
            if (idx[0]==idx[1] || idx[1]==idx[2] || idx[2]==idx[0]) {
                triAlive[t] = qfalse;
            } else {
                qboolean already = qfalse;
                for (int tu = 0; tu < vertToTriCount[bestU]; tu++) {
                    if (vertToTriList[bestU][tu] == t) { already = qtrue; break; }
                }
                if (!already) {
                    vertToTriList[bestU] = realloc(vertToTriList[bestU],
                        (vertToTriCount[bestU]+1) * sizeof(int));
                    vertToTriList[bestU][vertToTriCount[bestU]++] = t;
                }
            }
        }

        isFree[bestV] = qfalse;
        vertToTriCount[bestV] = 0;
        collapsedAny = qtrue;
    }

    // STEP 6: Compact index buffer
    int validIndexes = 0;
    int geomDegensRemoved = 0;
    for (int t = 0; t < numTris; t++) {
        if (!triAlive[t]) continue;
        int i0 = ds->indexes[t*3+0];
        int i1 = ds->indexes[t*3+1];
        int i2 = ds->indexes[t*3+2];
        
        // Topological degenerate (two identical indices)
        if (i0 == i1 || i1 == i2 || i0 == i2) continue;

        // Geometric degenerate (aspect-ratio sliver check)
        vec3_t ce1, ce2, ce3, cross;
        VectorSubtract(ds->verts[i1].xyz, ds->verts[i0].xyz, ce1);
        VectorSubtract(ds->verts[i2].xyz, ds->verts[i0].xyz, ce2);
        VectorSubtract(ds->verts[i2].xyz, ds->verts[i1].xyz, ce3);
        
        float len1Sq = DotProduct(ce1, ce1);
        float len2Sq = DotProduct(ce2, ce2);
        float len3Sq = DotProduct(ce3, ce3);
        
        if (len1Sq < 1e-9f || len2Sq < 1e-9f || len3Sq < 1e-9f) { geomDegensRemoved++; continue; }
        
        CrossProduct(ce1, ce2, cross);
        float twiceArea = VectorLength(cross);
        
        float maxEdgeSq = len1Sq;
        if (len2Sq > maxEdgeSq) maxEdgeSq = len2Sq;
        if (len3Sq > maxEdgeSq) maxEdgeSq = len3Sq;
        
        float sliverMetric = twiceArea / maxEdgeSq;
        if (sliverMetric < 0.001f) { geomDegensRemoved++; continue; }

        ds->indexes[validIndexes+0] = i0;
        ds->indexes[validIndexes+1] = i1;
        ds->indexes[validIndexes+2] = i2;
        validIndexes += 3;
    }
    ds->numIndexes = validIndexes;
    if (geomDegensRemoved > 0) {
        qprintf("    Purged %i zero-area sliver triangles in Phase 1\n", geomDegensRemoved);
    }

    // STEP 7: Compact vertex buffer into fresh allocation
    int *remap = malloc(oldNumVerts * sizeof(int));
    for (int v = 0; v < oldNumVerts; v++) remap[v] = -1;

    drawVert_t *newVerts = malloc(oldNumVerts * sizeof(drawVert_t));
    int newNumVerts = 0;

    for (int i = 0; i < ds->numIndexes; i++) {
        int oldV = ds->indexes[i];
        if (remap[oldV] == -1) {
            remap[oldV] = newNumVerts;
            newVerts[newNumVerts++] = ds->verts[oldV];
        }
        ds->indexes[i] = remap[oldV];
    }

    free(ds->verts);
    ds->verts = newVerts;
    ds->numVerts = newNumVerts;

    // Cleanup
    free(remap);
    free(triAlive);
    for (int v = 0; v < oldNumVerts; v++)
        free(vertToTriList[v]);
    free(vertToTriList);
    free(vertToTriCount);
    free(edges);
    free(faceNormals);
    free(isFree);
}

/*
==================
DecimateCollinearBoundaries (Phase 2: Collinear Boundary Decimation)
==================
*/
static void DecimateCollinearBoundaries(mapDrawSurface_t *ds)
{
    if (ds->numVerts < 3 || ds->numIndexes < 9) return;

    int numVerts = ds->numVerts;   // post-Phase-1 compacted vertex count
    int numTris  = ds->numIndexes / 3;

    // STEP A: Precompute face normals from current post-Phase-1 geometry
    vec3_t *faceNormals = malloc(numTris * sizeof(vec3_t));
    for (int t = 0; t < numTris; t++) {
        int i0 = ds->indexes[t*3+0];
        int i1 = ds->indexes[t*3+1];
        int i2 = ds->indexes[t*3+2];
        vec3_t e1, e2, cross;
        VectorSubtract(ds->verts[i1].xyz, ds->verts[i0].xyz, e1);
        VectorSubtract(ds->verts[i2].xyz, ds->verts[i0].xyz, e2);
        CrossProduct(e1, e2, cross);
        if (VectorNormalize(cross, cross) < 1e-6f) {
            VectorClear(faceNormals[t]);
        } else {
            VectorCopy(cross, faceNormals[t]);
        }
    }

    // STEP B: Build vertToTriList from post-Phase-1 ds->indexes
    int *vertToTriCount = calloc(numVerts, sizeof(int));
    for (int i = 0; i < ds->numIndexes; i++)
        vertToTriCount[ds->indexes[i]]++;

    int **vertToTriList = malloc(numVerts * sizeof(int*));
    for (int v = 0; v < numVerts; v++)
        vertToTriList[v] = malloc(vertToTriCount[v] * sizeof(int));

    int *triInsert = calloc(numVerts, sizeof(int));
    for (int t = 0; t < numTris; t++) {
        for (int k = 0; k < 3; k++) {
            int v = ds->indexes[t*3+k];
            vertToTriList[v][triInsert[v]++] = t;
        }
    }
    free(triInsert);

    // STEP C: Alive flags
    qboolean *triAlive  = malloc(numTris  * sizeof(qboolean));
    qboolean *vertAlive = malloc(numVerts * sizeof(qboolean));
    for (int t = 0; t < numTris;  t++) triAlive[t]  = qtrue;
    for (int v = 0; v < numVerts; v++) vertAlive[v] = qtrue;

    // STEP D: Build initial boundary graph ONCE
    int *boundaryCount = calloc(numVerts, sizeof(int));
    int *bN1 = malloc(numVerts * sizeof(int));
    int *bN2 = malloc(numVerts * sizeof(int));
    for (int v = 0; v < numVerts; v++) { bN1[v] = -1; bN2[v] = -1; }

    typedef struct { int v0, v1, count; } BEdge;
    int maxEdges = numTris * 3;
    BEdge *edgeTable = malloc(maxEdges * sizeof(BEdge));
    int numEdges = 0;

    for (int t = 0; t < numTris; t++) {
        for (int k = 0; k < 3; k++) {
            int va = ds->indexes[t*3 + k];
            int vb = ds->indexes[t*3 + ((k+1)%3)];
            if (va > vb) { int tmp = va; va = vb; vb = tmp; }

            int found = 0;
            for (int e = 0; e < numEdges; e++) {
                if (edgeTable[e].v0 == va && edgeTable[e].v1 == vb) {
                    edgeTable[e].count++;
                    found = 1;
                    break;
                }
            }
            if (!found && numEdges < maxEdges) {
                edgeTable[numEdges].v0 = va;
                edgeTable[numEdges].v1 = vb;
                edgeTable[numEdges].count = 1;
                numEdges++;
            }
        }
    }

    for (int e = 0; e < numEdges; e++) {
        if (edgeTable[e].count != 1) continue;
        int va = edgeTable[e].v0;
        int vb = edgeTable[e].v1;
        if      (bN1[va] == -1) bN1[va] = vb;
        else if (bN2[va] == -1) bN2[va] = vb;
        boundaryCount[va]++;
        if      (bN1[vb] == -1) bN1[vb] = va;
        else if (bN2[vb] == -1) bN2[vb] = va;
        boundaryCount[vb]++;
    }
    free(edgeTable);

    // STEP E: Greedy shortest-edge collapse loop
    qboolean collapsedAny = qtrue;
    while (collapsedAny) {
        collapsedAny = qfalse;
        float bestLenSq = 1e30f;
        int bestV = -1, bestU = -1;

        for (int v = 0; v < numVerts; v++) {
            if (!vertAlive[v]) continue;
            if (boundaryCount[v] != 2) continue;
            int n1 = bN1[v], n2 = bN2[v];
            if (n1 < 0 || n2 < 0 || !vertAlive[n1] || !vertAlive[n2]) continue;

            int firstTri = -1;
            for (int tv = 0; tv < vertToTriCount[v]; tv++) {
                if (triAlive[vertToTriList[v][tv]]) {
                    firstTri = vertToTriList[v][tv];
                    break;
                }
            }
            if (firstTri == -1) continue;

            vec3_t refN;
            VectorCopy(faceNormals[firstTri], refN);

            qboolean locked = qfalse;
            for (int tv = 0; tv < vertToTriCount[v] && !locked; tv++) {
                int t = vertToTriList[v][tv];
                if (!triAlive[t]) continue;
                if (DotProduct(faceNormals[t], refN) < 0.999f) {
                    locked = qtrue; break;
                }
                int *idx = &ds->indexes[t*3];
                for (int k = 0; k < 3 && !locked; k++) {
                    if (fabs(DotProduct(ds->verts[idx[k]].normal, refN)) < 0.999f)
                        locked = qtrue;
                }
            }
            if (locked) continue;

            vec3_t d1, d2;
            VectorSubtract(ds->verts[n1].xyz, ds->verts[v].xyz, d1);
            VectorSubtract(ds->verts[n2].xyz, ds->verts[v].xyz, d2);
            if (VectorNormalize(d1, d1) < 1e-6f) continue;
            if (VectorNormalize(d2, d2) < 1e-6f) continue;
            if (DotProduct(d1, d2) > -0.9999f) continue; // Corner or bent -> skip

            int candidates[2] = { n1, n2 };
            for (int ci = 0; ci < 2; ci++) {
                int u = candidates[ci];
                if (!vertAlive[u]) continue;

                // Gate 2: Link Condition (count unique mutual neighbors w of v and u)
                int linkCount = 0;
                for (int tv = 0; tv < vertToTriCount[v]; tv++) {
                    int t = vertToTriList[v][tv];
                    if (!triAlive[t]) continue;
                    int *idx = &ds->indexes[t*3];
                    for (int k = 0; k < 3; k++) {
                        int w = idx[k];
                        if (w == v || w == u) continue;

                        // Check if w already processed in an earlier triangle/index of v
                        qboolean alreadyProcessed = qfalse;
                        for (int tv2 = 0; tv2 <= tv && !alreadyProcessed; tv2++) {
                            int t2 = vertToTriList[v][tv2];
                            if (!triAlive[t2]) continue;
                            int *idx2 = &ds->indexes[t2*3];
                            int maxK = (tv2 == tv) ? k : 3;
                            for (int k2 = 0; k2 < maxK; k2++) {
                                if (idx2[k2] == w) { alreadyProcessed = qtrue; break; }
                            }
                        }
                        if (alreadyProcessed) continue;

                        qboolean isNeighborOfU = qfalse;
                        for (int tu = 0; tu < vertToTriCount[u]; tu++) {
                            int t2 = vertToTriList[u][tu];
                            if (!triAlive[t2]) continue;
                            int *idx2 = &ds->indexes[t2*3];
                            if (idx2[0]==w || idx2[1]==w || idx2[2]==w) {
                                isNeighborOfU = qtrue; break;
                            }
                        }
                        if (isNeighborOfU) linkCount++;
                    }
                }
                if (linkCount != 1) continue;

                // Gate 1: Triangle Flip Test
                qboolean flipped = qfalse;
                for (int tv = 0; tv < vertToTriCount[v] && !flipped; tv++) {
                    int t = vertToTriList[v][tv];
                    if (!triAlive[t]) continue;
                    int *idx = &ds->indexes[t*3];
                    if (idx[0]==u || idx[1]==u || idx[2]==u) continue;

                    float *p0 = ds->verts[(idx[0]==v) ? u : idx[0]].xyz;
                    float *p1 = ds->verts[(idx[1]==v) ? u : idx[1]].xyz;
                    float *p2 = ds->verts[(idx[2]==v) ? u : idx[2]].xyz;
                    vec3_t e1, e2, newCross;
                    VectorSubtract(p1, p0, e1);
                    VectorSubtract(p2, p0, e2);
                    CrossProduct(e1, e2, newCross);

                    if (VectorLength(newCross) > 1e-5f &&
                        DotProduct(newCross, faceNormals[t]) <= 0.0f)
                        flipped = qtrue;
                }
                if (flipped) continue;

                vec3_t diff;
                VectorSubtract(ds->verts[v].xyz, ds->verts[u].xyz, diff);
                float lenSq = DotProduct(diff, diff);
                if (lenSq < bestLenSq) {
                    bestLenSq = lenSq;
                    bestV = v;
                    bestU = u;
                }
            }
        }

        if (bestV == -1) break;

        // EXECUTE COLLAPSE: bestV -> bestU
        vertAlive[bestV] = qfalse;
        for (int i = 0; i < ds->numIndexes; i++) {
            if (ds->indexes[i] == bestV) ds->indexes[i] = bestU;
        }

        for (int tv = 0; tv < vertToTriCount[bestV]; tv++) {
            int t = vertToTriList[bestV][tv];
            if (!triAlive[t]) continue;
            int *idx = &ds->indexes[t*3];
            if (idx[0]==idx[1] || idx[1]==idx[2] || idx[2]==idx[0]) {
                triAlive[t] = qfalse;
            } else {
                qboolean already = qfalse;
                for (int tu = 0; tu < vertToTriCount[bestU]; tu++)
                    if (vertToTriList[bestU][tu] == t) { already = qtrue; break; }
                if (!already) {
                    vertToTriList[bestU] = realloc(vertToTriList[bestU],
                        (vertToTriCount[bestU]+1) * sizeof(int));
                    vertToTriList[bestU][vertToTriCount[bestU]++] = t;
                }
            }
        }
        vertToTriCount[bestV] = 0;

        int bPrev = (bN1[bestV] == bestU) ? bN2[bestV] : bN1[bestV];
        if      (bN1[bestU] == bestV) bN1[bestU] = bPrev;
        else if (bN2[bestU] == bestV) bN2[bestU] = bPrev;

        if (bPrev >= 0) {
            if      (bN1[bPrev] == bestV) bN1[bPrev] = bestU;
            else if (bN2[bPrev] == bestV) bN2[bPrev] = bestU;
        }

        boundaryCount[bestV] = 0;
        bN1[bestV] = -1;
        bN2[bestV] = -1;

        collapsedAny = qtrue;
    }

    // STEP F: Compact index buffer
    int validIndexes = 0;
    int geomDegensRemoved = 0;
    for (int t = 0; t < numTris; t++) {
        if (!triAlive[t]) continue;
        int i0 = ds->indexes[t*3+0];
        int i1 = ds->indexes[t*3+1];
        int i2 = ds->indexes[t*3+2];
        
        // Topological degenerate
        if (i0 == i1 || i1 == i2 || i0 == i2) continue;

        // Geometric degenerate (aspect-ratio sliver check)
        vec3_t ce1, ce2, ce3, cross;
        VectorSubtract(ds->verts[i1].xyz, ds->verts[i0].xyz, ce1);
        VectorSubtract(ds->verts[i2].xyz, ds->verts[i0].xyz, ce2);
        VectorSubtract(ds->verts[i2].xyz, ds->verts[i1].xyz, ce3);
        
        float len1Sq = DotProduct(ce1, ce1);
        float len2Sq = DotProduct(ce2, ce2);
        float len3Sq = DotProduct(ce3, ce3);
        
        if (len1Sq < 1e-9f || len2Sq < 1e-9f || len3Sq < 1e-9f) { geomDegensRemoved++; continue; }
        
        CrossProduct(ce1, ce2, cross);
        float twiceArea = VectorLength(cross);
        
        float maxEdgeSq = len1Sq;
        if (len2Sq > maxEdgeSq) maxEdgeSq = len2Sq;
        if (len3Sq > maxEdgeSq) maxEdgeSq = len3Sq;
        
        float sliverMetric = twiceArea / maxEdgeSq;
        if (sliverMetric < 0.001f) { geomDegensRemoved++; continue; }

        ds->indexes[validIndexes+0] = i0;
        ds->indexes[validIndexes+1] = i1;
        ds->indexes[validIndexes+2] = i2;
        validIndexes += 3;
    }
    ds->numIndexes = validIndexes;
    if (geomDegensRemoved > 0) {
        qprintf("    Purged %i zero-area sliver triangles in Phase 2\n", geomDegensRemoved);
    }

    // STEP G: Compact vertex buffer
    int *remap = malloc(numVerts * sizeof(int));
    for (int v = 0; v < numVerts; v++) remap[v] = -1;

    drawVert_t *newVerts = malloc(numVerts * sizeof(drawVert_t));
    int newNumVerts = 0;

    for (int i = 0; i < ds->numIndexes; i++) {
        int oldV = ds->indexes[i];
        if (remap[oldV] == -1) {
            remap[oldV] = newNumVerts;
            newVerts[newNumVerts++] = ds->verts[oldV];
        }
        ds->indexes[i] = remap[oldV];
    }

    free(ds->verts);
    ds->verts    = newVerts;
    ds->numVerts = newNumVerts;

    // Cleanup
    free(remap);
    free(triAlive);
    free(vertAlive);
    free(vertToTriCount);
    for (int v = 0; v < numVerts; v++) free(vertToTriList[v]);
    free(vertToTriList);
    free(boundaryCount);
    free(bN1);
    free(bN2);
    free(faceNormals);
}

/*
==================
DecimateAllTrisoups
==================
*/
void DecimateAllTrisoups(entity_t *e, qboolean onlyPlanar)
{
    int numDecimated = 0;
    int totalVertsRemoved = 0;
    int totalTrisRemoved = 0;

    qprintf("----- DecimateAllTrisoups -----\n");

    int totalP1Verts = 0, totalP2Verts = 0;
    int totalP1Tris = 0, totalP2Tris = 0;

    for (int i = e->firstDrawSurf; i < numMapDrawSurfs; i++) {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];

        if (!ds->miscModel || ds->numVerts < 3 || ds->numIndexes < 9)
            continue;

        if (onlyPlanar && !ds->isPlanar && !ds->planarDerived)
            continue;

        int oldVerts = ds->numVerts;
        int oldTris  = ds->numIndexes / 3;

        DecimateSingleTrisoup(ds);
        int p1VertsRemoved = oldVerts - ds->numVerts;
        int p1TrisRemoved  = oldTris  - (ds->numIndexes / 3);
        
        int midVerts = ds->numVerts;
        int midTris  = ds->numIndexes / 3;
        
        DecimateCollinearBoundaries(ds);
        int p2VertsRemoved = midVerts - ds->numVerts;
        int p2TrisRemoved  = midTris  - (ds->numIndexes / 3);

        int vertsRemoved = p1VertsRemoved + p2VertsRemoved;
        int trisRemoved  = p1TrisRemoved + p2TrisRemoved;

        if (vertsRemoved > 0 || trisRemoved > 0)
            numDecimated++;

        totalVertsRemoved += vertsRemoved;
        totalTrisRemoved  += trisRemoved;
        totalP1Verts += p1VertsRemoved;
        totalP2Verts += p2VertsRemoved;
        totalP1Tris += p1TrisRemoved;
        totalP2Tris += p2TrisRemoved;
    }

    qprintf("%6i trisoups decimated\n",    numDecimated);
    qprintf("%6i vertices removed (%i in Phase 1 / %i in Phase 2)\n",      totalVertsRemoved, totalP1Verts, totalP2Verts);
    qprintf("%6i triangles removed (%i in Phase 1 / %i in Phase 2)\n",     totalTrisRemoved, totalP1Tris, totalP2Tris);
}

#ifdef DECIMATE_PLANAR_WITH_MESHLIB
/*
==================
DecimateSurfaceWithMeshLib

Re-triangulates and simplifies planar sub-meshes using MeshLib's QEM, while locking boundary vertices.
==================
*/
qboolean DecimateSurfaceWithMeshLib(mapDrawSurface_t *ds)
{
    // == GUARD ==
    if (!ds->planarDerived || !ds->miscModel) return qfalse;
    if (ds->numParentIndexes < 3) return qfalse;       // No parent face to decimate
    if (ds->numIndexes < ds->numParentIndexes) return qfalse; // Sanity

    int numParentTris = ds->numParentIndexes / 3;

    // == STEP 1: COLLECT UNIQUE VERTICES in the parent face sub-mesh ==
    int *globalToLocal = malloc(ds->numVerts * sizeof(int));
    memset(globalToLocal, -1, ds->numVerts * sizeof(int));

    int *localToGlobal = malloc(ds->numVerts * sizeof(int));

    MRVector3f *mrVerts = malloc(ds->numVerts * sizeof(MRVector3f)); // upper bound
    MRVector2f *mrUVs   = malloc(ds->numVerts * sizeof(MRVector2f)); // upper bound
    int numLocalVerts = 0;

    MRThreeVertIds *mrTris = malloc(numParentTris * sizeof(MRThreeVertIds));

    for (int t = 0; t < numParentTris; t++) {
        for (int c = 0; c < 3; c++) {
            int gIdx = ds->indexes[t*3 + c];
            if (globalToLocal[gIdx] == -1) {
                globalToLocal[gIdx] = numLocalVerts;
                localToGlobal[numLocalVerts] = gIdx;
                mrVerts[numLocalVerts].x = ds->verts[gIdx].xyz[0];
                mrVerts[numLocalVerts].y = ds->verts[gIdx].xyz[1];
                mrVerts[numLocalVerts].z = ds->verts[gIdx].xyz[2];
                mrUVs[numLocalVerts].x = ds->verts[gIdx].st[0];
                mrUVs[numLocalVerts].y = ds->verts[gIdx].st[1];
                numLocalVerts++;
            }
            mrTris[t][c].id = globalToLocal[gIdx];
        }
    }

    // == STEP 2: BUILD MRMesh ==
    MRMesh *mesh = mrMeshFromTriangles(mrVerts, numLocalVerts, mrTris, numParentTris);
    free(mrTris);

    if (!mesh) {
        free(localToGlobal); free(globalToLocal); free(mrVerts); free(mrUVs);
        return qfalse;
    }

    // == STEP 3: PREPARE ATTRIBUTES (UVs) ==
    MRMeshAttributes attrs;
    memset(&attrs, 0, sizeof(MRMeshAttributes));
    attrs.uvCoords = mrUVs;
    attrs.numUvs   = numLocalVerts;

    // == STEP 4: CONFIGURE DECIMATION ==
    MRDecimateSettings settings = mrDecimateSettingsNew();
    settings.strategy              = MRDecimateStrategyMinimizeError;
    settings.maxError              = 0.0f;      // Zero: only topological changes (no vertex moves)
    settings.stabilizer            = 0.001f;    // Helps on perfectly flat planar meshes
    settings.optimizeVertexPos     = false;     // Lock vertex positions absolutely
    settings.touchBdVerts          = false;     // LOCK boundary verts (chamfer crease)
    settings.touchNearBdEdges      = false;     // Also lock edges adjacent to boundary
    settings.maxAngleChange        = 0.0f;      // Allow edge flips for Delaunay quality
    settings.packMesh              = false;

    // == STEP 5: DECIMATE ==
    MRDecimateResult result = mrMeshDecimateWithAttributes(mesh, &attrs, &settings);

    free(mrVerts); // MeshLib made its own copy

    // == STEP 6: EXTRACT RESULTS ==
    const MRVector3f *newPts = mrMeshPoints(mesh);
    size_t newNumPts = mrMeshPointsNum(mesh);

    MRTriangulation *tri = mrMeshGetTriangulation(mesh);

    int newNumTris = 0;
    for (size_t fi = 0; fi < tri->size; fi++) {
        if (tri->data[fi][0].id >= 0) newNumTris++;
    }

    int numChamferIndexes = ds->numIndexes - ds->numParentIndexes;
    int *oldChamferIndexes = malloc(numChamferIndexes * sizeof(int));
    memcpy(oldChamferIndexes, &ds->indexes[ds->numParentIndexes], numChamferIndexes * sizeof(int));

    int *chamferLocalToGlobal = malloc(ds->numVerts * sizeof(int));
    int *chamferGlobalToNew = malloc(ds->numVerts * sizeof(int));
    memset(chamferGlobalToNew, -1, ds->numVerts * sizeof(int));
    int numChamferVerts = 0;

    for (int i = 0; i < numChamferIndexes; i++) {
        int gIdx = oldChamferIndexes[i];
        if (chamferGlobalToNew[gIdx] == -1) {
            chamferGlobalToNew[gIdx] = (int)newNumPts + numChamferVerts;
            chamferLocalToGlobal[numChamferVerts] = gIdx;
            numChamferVerts++;
        }
    }

    int totalNewVerts = (int)newNumPts + numChamferVerts;
    int totalNewIndexes = newNumTris * 3 + numChamferIndexes;

    drawVert_t *newVerts = malloc(totalNewVerts * sizeof(drawVert_t));
    int *newIndexes = malloc(totalNewIndexes * sizeof(int));

    for (size_t v = 0; v < newNumPts; v++) {
        newVerts[v] = ds->verts[localToGlobal[v]];
        
        newVerts[v].xyz[0] = newPts[v].x;
        newVerts[v].xyz[1] = newPts[v].y;
        newVerts[v].xyz[2] = newPts[v].z;
        newVerts[v].st[0] = attrs.uvCoords[v].x;
        newVerts[v].st[1] = attrs.uvCoords[v].y;
    }

    for (int v = 0; v < numChamferVerts; v++) {
        newVerts[(int)newNumPts + v] = ds->verts[chamferLocalToGlobal[v]];
    }

    int idxOut = 0;
    for (size_t fi = 0; fi < tri->size; fi++) {
        if (tri->data[fi][0].id < 0) continue;
        newIndexes[idxOut++] = tri->data[fi][0].id;
        newIndexes[idxOut++] = tri->data[fi][1].id;
        newIndexes[idxOut++] = tri->data[fi][2].id;
    }

    int newNumParentIndexes = idxOut;
    for (int i = 0; i < numChamferIndexes; i++) {
        int gIdx = oldChamferIndexes[i];
        newIndexes[idxOut++] = chamferGlobalToNew[gIdx];
    }

    free(ds->verts);
    free(ds->indexes);

    ds->verts = newVerts;
    ds->numVerts = totalNewVerts;
    ds->indexes = newIndexes;
    ds->numIndexes = totalNewIndexes;
    ds->numParentIndexes = newNumParentIndexes;

    free(localToGlobal);
    free(globalToLocal);
    free(oldChamferIndexes);
    free(chamferLocalToGlobal);
    free(chamferGlobalToNew);
    free(attrs.uvCoords);
    mrTriangulationFree(tri);
    mrMeshFree(mesh);

    return qtrue;
}
#endif

/*
==================
GenerateTrisoupUVs
==================
*/
void GenerateTrisoupUVs(entity_t *e)
{
    qprintf("----- GenerateTrisoupUVs -----\n");
    for (int i = e->firstDrawSurf; i < numMapDrawSurfs; i++) {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];
        if (!ds->miscModel || ds->numVerts <= 0 || ds->numIndexes <= 0)
            continue;
        GenerateAtomicUVsWithXAtlas(ds);
    }
}
