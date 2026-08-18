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
#include "model_collision.h"

/*
==================
PromotePatchesToTrisoups
==================
*/
static void PromotePatchesToTrisoups(entity_t *e)
{
    int entNum = e - entities;

    // Read 'subdivide' key with fallback aliases and a default of 8.0
    float subdivide = FloatForKey(e, "subdivide");
    if (subdivide <= 0.0f) subdivide = FloatForKey(e, "subdivisions");
    if (subdivide <= 0.0f) subdivide = 8.0f;

    // Only scan surfaces that existed before this function was called.
    int origCount = numMapDrawSurfs;

    for (int i = 0; i < origCount; i++)
    {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];

        // Only process patches belonging to this func_trisoup entity
        if (!ds->patch || ds->numVerts <= 0) continue;
        if (ds->entityNum != entNum) continue;

        shaderInfo_t *si = ds->shaderInfo;
        if (!si) continue;

        // If the shader is nodraw/sky/nolightmap, skip the whole surface.
        if (si->surfaceFlags & (SURF_NODRAW | SURF_SKY | SURF_NOLIGHTMAP)) continue;

        // 1. Build source mesh from the patch drawsurf's control points
        mesh_t srcMesh;
        srcMesh.width  = ds->patchWidth;
        srcMesh.height = ds->patchHeight;
        srcMesh.verts  = ds->verts;

        // 2. Exact 3-stage Bezier tessellation (mirrors SubdividePatchForLighting)
        mesh_t *subMesh = SubdivideMesh(srcMesh, subdivide, 999.0f);
        PutMeshOnCurve(*subMesh);
        mesh_t *tess = RemoveLinearMeshColumnsRows(subMesh);
        FreeMesh(subMesh);

        // 3. Degenerate guard
        if (tess->width < 2 || tess->height < 2) {
            FreeMesh(tess);
            ds->numVerts = 0; // suppress original too
            continue;
        }

        int W = tess->width;
        int H = tess->height;
        int numVerts = W * H;
        int numQuads = (W - 1) * (H - 1);
        int numIndexes = numQuads * 6;

        // 4. Allocate new Trisoup drawsurf
        mapDrawSurface_t *newDs = AllocDrawSurf();

        newDs->entityNum    = entNum;
        newDs->shaderInfo   = si;
        newDs->miscModel    = qtrue;
        newDs->patch        = qfalse;
        newDs->patchDerived = qtrue;
        newDs->planarDerived = qfalse;
        newDs->isPlanar      = qfalse;
        newDs->mapBrush      = NULL;
        newDs->side          = NULL;
        newDs->fogNum        = -1;
        newDs->lightmapNum   = -1;

        // Inherit lighting sidecar metadata
        newDs->samplesize        = ds->samplesize;
        newDs->lightmapScale     = ds->lightmapScale;
        newDs->smoothingRadius   = ds->smoothingRadius;
        newDs->superSampleRadius = ds->superSampleRadius;
        newDs->upscale           = ds->upscale;
        newDs->castShadows       = ds->castShadows;
        newDs->gridAmbientScale  = ds->gridAmbientScale;
        newDs->gridDirectScale   = ds->gridDirectScale;
        newDs->lightValue        = ds->lightValue;
        VectorCopy(ds->lightColor, newDs->lightColor);
        newDs->backsplashFraction = ds->backsplashFraction;
        newDs->lightSubdivide    = ds->lightSubdivide;
        newDs->noDeluxeInfluence = ds->noDeluxeInfluence;
        newDs->noDeluxeInfluenceBacksplash = ds->noDeluxeInfluenceBacksplash;
        newDs->overrideVertexColor = ds->overrideVertexColor;
        VectorCopy(ds->vertexColor, newDs->vertexColor);
        newDs->isHalo            = ds->isHalo;
        newDs->cutoff            = ds->cutoff;
        newDs->fadeout           = ds->fadeout;
        newDs->hasAttenuationOverride = ds->hasAttenuationOverride;
        newDs->attenuationModel  = ds->attenuationModel;
        strncpy(newDs->decalgroup, ds->decalgroup, sizeof(newDs->decalgroup) - 1);

        // 5. Copy tessellated vertex data
        newDs->numVerts = numVerts;
        newDs->verts = malloc(numVerts * sizeof(drawVert_t));
        memcpy(newDs->verts, tess->verts, numVerts * sizeof(drawVert_t));

        // 6. Flag internal vs boundary vertices using lightmap[0][0]
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                qboolean isBoundary = (x == 0 || x == W - 1 || y == 0 || y == H - 1);
                // Populate true UV coordinates (grid relative) so AllocateLightmapForMiscModel
                // can properly calculate the areaUV and assign a proportional lightmap.
                newDs->verts[y * W + x].lightmap[0][0] = (float)x;
                newDs->verts[y * W + x].lightmap[0][1] = (float)y;
                // Use vertex color alpha channel as the smoothing boundary sentinel instead
                newDs->verts[y * W + x].color[0][3] = isBoundary ? 0 : 255;
            }
        }

        // 7. Generate triangle indices with the exact Clockwise grid winding 
        // that the Q3 engine uses in tr_curve.c for MST_PATCH tristrips.
        newDs->numIndexes = numIndexes;
        newDs->indexes = malloc(numIndexes * sizeof(int));
        int idx = 0;
        for (int y = 0; y < H - 1; y++) {
            for (int x = 0; x < W - 1; x++) {
                int v0 = y * W + x;
                int v1 = y * W + (x + 1);
                int v2 = (y + 1) * W + (x + 1);
                int v3 = (y + 1) * W + x;
                
                // Match engine's (i+1, j) then (i, j) tristrip winding pattern
                // Tri 1
                newDs->indexes[idx++] = v0;
                newDs->indexes[idx++] = v3;
                newDs->indexes[idx++] = v2;
                // Tri 2
                newDs->indexes[idx++] = v0;
                newDs->indexes[idx++] = v2;
                newDs->indexes[idx++] = v1;
            }
        }

        FreeMesh(tess);

        // 8. Handle the original patch's collision/visibility role
        qboolean isSolid = (si->contents & CONTENTS_SOLID) != 0;
        if (isSolid) {
            ds->shaderInfo = GetCollisionShaderInfo(si);
        } else {
            ds->numVerts = 0;
        }

        _printf("  patch -> trisoup: %s (%dx%d ctrl -> %dx%d tess, %d tris)\n",
                si->shader, ds->patchWidth, ds->patchHeight, W, H, numQuads * 2);
    }
}

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
 
    vec3_t *faceNormals = calloc(numMapDrawSurfs, sizeof(vec3_t));
    for (int i = 0; i < numMapDrawSurfs; i++) {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];
        if (ds->mapBrush != NULL && ds->mapBrush->entitynum == entNum && IsEdgeSharingCandidate(ds))
            VectorCopy(mapplanes[ds->side->planenum].normal, faceNormals[i]);
    }
 
    for (int i = 0; i < numMapDrawSurfs; i++)
    {
        mapDrawSurface_t *dsA = &mapDrawSurfs[i];
        
        qboolean isBrushFace  = (dsA->mapBrush && dsA->mapBrush->entitynum == entNum && IsEdgeSharingCandidate(dsA));
        qboolean isPatchTri   = (dsA->patchDerived && dsA->entityNum == entNum);
        if (!isBrushFace && !isPatchTri) continue;
 
        for (int v = 0; v < dsA->numVerts; v++)
        {
            if (isPatchTri && dsA->verts[v].color[0][3] > 128) continue;

            vec3_t targetXYZ;
            VectorCopy(dsA->verts[v].xyz, targetXYZ);
 
            // Collect all faces sharing this exact vertex coordinate
            vec3_t contactNormals[MAX_SHARED_FACES];
            int sharedSurfs[MAX_SHARED_FACES];
            int contactCount = 0;

            for (int j = 0; j < numMapDrawSurfs && contactCount < MAX_SHARED_FACES; j++)
            {
                mapDrawSurface_t *dsB = &mapDrawSurfs[j];
                qboolean bIsBrush = (dsB->mapBrush && dsB->mapBrush->entitynum == entNum && IsEdgeSharingCandidate(dsB));
                qboolean bIsPatch = (dsB->patchDerived && dsB->entityNum == entNum);
                if (!bIsBrush && !bIsPatch) continue;

                for (int w = 0; w < dsB->numVerts; w++) {
                    if (VectorsNearEqual(targetXYZ, dsB->verts[w].xyz, 0.1f)) {
                        vec3_t n;
                        if (bIsBrush) {
                            VectorCopy(faceNormals[j], n);
                        } else {
                            VectorCopy(dsB->verts[w].normal, n);
                        }
                        VectorCopy(n, contactNormals[contactCount]);
                        sharedSurfs[contactCount] = j;
                        contactCount++;
                        break; 
                    }
                }
            }

            if (contactCount >= MAX_SHARED_FACES) {
                _printf("WARNING: SmoothTrisoupNormals hit MAX_SHARED_FACES limit at vertex!\n");
            }

            // Build adjacency matrix based on shadeangle threshold
            qboolean adjacent[MAX_SHARED_FACES][MAX_SHARED_FACES] = {0};
            for (int a = 0; a < contactCount; a++) {
                for (int b = 0; b < contactCount; b++) {
                    if (DotProduct(contactNormals[a], contactNormals[b]) >= thresholdDot) {
                        adjacent[a][b] = qtrue;
                    }
                }
            }

            // Find connected component containing face 'i'
            qboolean visited[MAX_SHARED_FACES] = {0};
            int queue[MAX_SHARED_FACES];
            int qhead = 0, qtail = 0;

            int rootIdx = 0;
            for (int a = 0; a < contactCount; a++) {
                if (sharedSurfs[a] == i) { rootIdx = a; break; }
            }

            queue[qtail++] = rootIdx;
            visited[rootIdx] = qtrue;

            while (qhead < qtail) {
                int curr = queue[qhead++];
                for (int a = 0; a < contactCount; a++) {
                    if (adjacent[curr][a] && !visited[a]) {
                        visited[a] = qtrue;
                        queue[qtail++] = a;
                    }
                }
            }

            // Sum and normalize face normals in connected component
            vec3_t blendedNormal = {0};
            for (int a = 0; a < contactCount; a++) {
                if (visited[a]) {
                    VectorAdd(blendedNormal, contactNormals[a], blendedNormal);
                }
            }

            if (VectorLength(blendedNormal) > 1e-4f) {
                VectorNormalize(blendedNormal, dsA->verts[v].normal);
            }
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

    // 1. Tessellate Bezier patches into trisoups FIRST
    PromotePatchesToTrisoups(e);

    // 2. Sew up collinear vertices across all angles (-1.0 to 1.0) along original brush edges
    InsertCollinearVertices(&entities[0], -1.0f, 1.0f, entNum);

    // 3. Smooth normals using Transitive Closure sets
    SmoothTrisoupNormalsByShadeAngle(e);

    // 4. Promote standard brush faces to atomic trisoups (triangulate N-gon to CCW fan)
    PromoteBrushesToAtomicTrisoups(e);
}
