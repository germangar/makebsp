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
    
    // Pass 1: Compute inner bodies (insets)
    for (i = e->firstDrawSurf; i < numBaseDrawSurfs; i++)
    {
        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        surfaceChamferEdge_t edges[MAX_CHAMFER_VERTS];
        int numEdges = 0;
        surfaceNeighbor_t *nb;
        
        if (!IsChamferCandidate(dsA)) continue;

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

        if (numEdges > 0)
        {
            globalInsets[i] = malloc(dsA->numVerts * sizeof(drawVert_t));
            memset(globalInsets[i], 0, dsA->numVerts * sizeof(drawVert_t));
            ComputeAllInsets(dsA, edges, numEdges, chamfer_global_width, globalInsets[i]);
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
                        mapDrawSurface_t *strip = AllocDrawSurf();
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
                            strip->verts[innerSlot] = dsA->verts[vIdx];
                            VectorCopy(globalInsets[i][vIdx].xyz, strip->verts[innerSlot].xyz);
                            VectorCopy(faceNormal, strip->verts[innerSlot].normal);
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
            }
            free(globalInsets[i]);
        }
    }
}
