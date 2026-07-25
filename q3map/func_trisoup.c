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

#define MAX_SHARED_FACES 64

/*
==================
PromoteBrushesToAtomicTrisoups
==================
*/
static void PromoteBrushesToAtomicTrisoups(entity_t *e)
{
    int i;
    int entNum = e - entities;
    for (i = 0; i < numMapDrawSurfs; i++)
    {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];
        if (ds->mapBrush == NULL || ds->mapBrush->entitynum != entNum) continue;
        if (!IsEdgeSharingCandidate(ds)) continue;

        // Triangulate flat N-gon into a CCW triangle fan
        int numTris = ds->numVerts - 2;
        ds->numIndexes = numTris * 3;
        ds->indexes = malloc(ds->numIndexes * sizeof(int));
        
        int idx = 0;
        for (int k = 1; k <= ds->numVerts - 2; k++)
        {
            ds->indexes[idx++] = 0;
            ds->indexes[idx++] = k;
            ds->indexes[idx++] = k + 1;
        }

        ds->miscModel = qtrue;
        ds->planarDerived = qtrue;
    }
}

/*
==================
SmoothTrisoupNormalsByShadeAngle
==================
*/
static void SmoothTrisoupNormalsByShadeAngle(entity_t *e)
{
    int entNum = e - entities;
    float shadeAngle = FloatForKey(e, "shadeangle");
    if (shadeAngle <= 0.0f) shadeAngle = 46.0f; 
    float thresholdDot = cos(shadeAngle * Q_PI / 180.0f);
 
    vec3_t *faceNormals = malloc(numMapDrawSurfs * sizeof(vec3_t));
    for (int i = 0; i < numMapDrawSurfs; i++) {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];
        if (ds->mapBrush != NULL && ds->mapBrush->entitynum == entNum && IsEdgeSharingCandidate(ds))
            VectorCopy(mapplanes[ds->side->planenum].normal, faceNormals[i]);
    }
 
    for (int i = 0; i < numMapDrawSurfs; i++)
    {
        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        if (dsA->mapBrush == NULL || dsA->mapBrush->entitynum != entNum) continue;
        if (!IsEdgeSharingCandidate(dsA)) continue;
 
        for (int v = 0; v < dsA->numVerts; v++)
        {
            vec3_t targetXYZ;
            VectorCopy(dsA->verts[v].xyz, targetXYZ);
 
            // Collect all faces sharing this exact vertex coordinate
            int sharedSurfs[MAX_SHARED_FACES];
            int sharedCount = 0;
            for (int j = 0; j < numMapDrawSurfs && sharedCount < MAX_SHARED_FACES; j++)
            {
                mapDrawSurface_t *dsB = &mapDrawSurfs[j];
                if (dsB->mapBrush == NULL || dsB->mapBrush->entitynum != entNum) continue;
                if (!IsEdgeSharingCandidate(dsB)) continue;

                for (int w = 0; w < dsB->numVerts; w++) {
                    if (VectorsNearEqual(targetXYZ, dsB->verts[w].xyz, 0.1f)) {
                        sharedSurfs[sharedCount++] = j;
                        break; 
                    }
                }
            }

            if (sharedCount >= MAX_SHARED_FACES) {
                _printf("WARNING: SmoothTrisoupNormals hit MAX_SHARED_FACES limit at vertex!\n");
            }

            // Build adjacency matrix based on shadeangle threshold
            qboolean adjacent[MAX_SHARED_FACES][MAX_SHARED_FACES] = {0};
            for (int a = 0; a < sharedCount; a++) {
                for (int b = 0; b < sharedCount; b++) {
                    vec3_t nA, nB;
                    VectorCopy(faceNormals[sharedSurfs[a]], nA);
                    VectorCopy(faceNormals[sharedSurfs[b]], nB);
                    if (DotProduct(nA, nB) >= thresholdDot) {
                        adjacent[a][b] = qtrue;
                    }
                }
            }

            // Find connected component containing face 'i'
            qboolean visited[MAX_SHARED_FACES] = {0};
            int queue[MAX_SHARED_FACES];
            int qhead = 0, qtail = 0;

            int rootIdx = 0;
            for (int a = 0; a < sharedCount; a++) {
                if (sharedSurfs[a] == i) { rootIdx = a; break; }
            }

            queue[qtail++] = rootIdx;
            visited[rootIdx] = qtrue;

            while (qhead < qtail) {
                int curr = queue[qhead++];
                for (int a = 0; a < sharedCount; a++) {
                    if (adjacent[curr][a] && !visited[a]) {
                        visited[a] = qtrue;
                        queue[qtail++] = a;
                    }
                }
            }

            // Sum and normalize face normals in connected component
            vec3_t blendedNormal = {0};
            for (int a = 0; a < sharedCount; a++) {
                if (visited[a]) {
                    VectorAdd(blendedNormal, faceNormals[sharedSurfs[a]], blendedNormal);
                }
            }
            VectorNormalize(blendedNormal, dsA->verts[v].normal);
        }
    }

    free(faceNormals);
}

/*
==================
ProcessFuncTrisoup
==================
*/
void ProcessFuncTrisoup(entity_t *e)
{
    int entNum = e - entities;
    _printf("----- ProcessFuncTrisoup -----\n");

    // 1. Sew up collinear vertices across all angles (-1.0 to 1.0) along original brush edges
    InsertCollinearVertices(&entities[0], -1.0f, 1.0f, entNum);

    // 2. Smooth normals using Transitive Closure sets BEFORE merging
    SmoothTrisoupNormalsByShadeAngle(e);

    // 3. Promote standard brush faces to atomic trisoups (triangulate N-gon to CCW fan)
    PromoteBrushesToAtomicTrisoups(e);
}
