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

/*

  Lightmap allocation has to be done after all flood filling and
  visible surface determination.

*/

int numSortShaders;
mapDrawSurface_t *surfsOnShader[MAX_MAP_SHADERS];
int totalLightmappedShaders = 0;

#define MAX_LIGHTMAPS 2048
#define MAX_LIGHTMAP_WIDTH 1024
#define UV_PRECISION_NUDGE 0.0001f
int *lightmapHeights = NULL;

int numLightmaps = 0;
int c_exactLightmap;

void PrepareNewLightmap(void)
{
    if (numLightmaps >= MAX_LIGHTMAPS)
    {
        Error("MAX_LIGHTMAPS exceeded");
    }
    // Explicitly clear the memory for the new lightmap's heightmap
    memset(&lightmapHeights[numLightmaps * MAX_LIGHTMAP_WIDTH], 0, sizeof(int) * MAX_LIGHTMAP_WIDTH);
    numLightmaps++;
}

/*
===============
AllocLMBlock

returns a texture number and the position inside it
===============
*/
qboolean AllocLMBlock(int lmIndex, int w, int h, int *x, int *y)
{
    int i, j;
    int *allocated = &lightmapHeights[lmIndex * MAX_LIGHTMAP_WIDTH];
    int bestY;

    // Search for the first horizontal run where it fits vertically
    for (i = 0; i <= LIGHTMAP_WIDTH - w; i++)
    {
        bestY = 0;
        for (j = 0; j < w; j++)
        {
            if (allocated[i + j] > bestY)
            {
                bestY = allocated[i + j];
            }
            if (bestY + h > LIGHTMAP_HEIGHT)
            {
                break; // Doesn't fit in this run starting at 'i'
            }
        }

        if (j == w)
        { // Fits!
            *x = i;
            *y = bestY;

            // Update the heightmap
            for (j = 0; j < w; j++)
            {
                allocated[i + j] = bestY + h;
            }
            return qtrue;
        }
    }

    return qfalse;
}

/*
===================
AllocateLightmapForMiscModel
===================
*/
void AllocateLightmapForMiscModel(mapDrawSurface_t *ds)
{
    int i, x, y;
    float ssize;
    float min_s, max_s, min_t, max_t;
    double area3D = 0, areaUV = 0;
    float s, t, scale;
    int w, h;
    drawVert_t *v0, *v1, *v2;

    if (ds->numIndexes < 3)
        return;

    ssize = ds->samplesize;

    // 1. Initial UV bounds
    min_s = min_t = 1000000;
    max_s = max_t = -1000000;
    for (i = 0; i < ds->numVerts; i++)
    {
        s = ds->verts[i].lightmap[0][0];
        t = ds->verts[i].lightmap[0][1];
        if (s < min_s)
            min_s = s;
        if (s > max_s)
            max_s = s;
        if (t < min_t)
            min_t = t;
        if (t > max_t)
            max_t = t;
    }

    // 2. Area Calculation
    for (i = 0; i < ds->numIndexes; i += 3)
    {
        v0 = &ds->verts[ds->indexes[i]];
        v1 = &ds->verts[ds->indexes[i + 1]];
        v2 = &ds->verts[ds->indexes[i + 2]];

        // 3D Area (cross product)
        vec3_t side1, side2, cross;
        VectorSubtract(v1->xyz, v0->xyz, side1);
        VectorSubtract(v2->xyz, v0->xyz, side2);
        CrossProduct(side1, side2, cross);
        area3D += 0.5 * VectorLength(cross);

        // UV Area (2D cross product)
        areaUV +=
            0.5 *
            fabs((v1->lightmap[0][0] - v0->lightmap[0][0]) *
                     (v2->lightmap[0][1] - v0->lightmap[0][1]) -
                 (v2->lightmap[0][0] - v0->lightmap[0][0]) *
                     (v1->lightmap[0][1] - v0->lightmap[0][1]));
    }

    if (areaUV < 0.0001)
    {
        _printf("WARNING: misc_model surface with degenerate UVs (areaUV: %f)\n",
                areaUV);
        return;
    }

    // 3. Scale Determination
    // Target density: 1/ssize^2 luxels per square unit.
    scale = sqrt((area3D / (ssize * ssize)) / areaUV);

    if (ds->patchDerived) {
        _printf("    [DEBUG] Patch area3D: %f, areaUV: %f -> scale: %f\n", area3D, areaUV, scale);
    }

    // Safeguard against extreme scaling
    if (scale < 0.01)
        scale = 0.01;

    // Enforce dynamic minimum lightmap area.
    // This floor is bypassed for planar-derived and patch-derived atomic trisoups
    // because their scale is already correctly computed from physics and their UVs are 
    // densely packed, preventing small bevels or patches from artificially inflating.
    if (!ds->planarDerived && !ds->patchDerived)
    {
        float uvWidth = max_s - min_s;
        float uvHeight = max_t - min_t;
        float uvArea = uvWidth * uvHeight;
        if (uvArea > 0.0001f)
        {
            float minDimension = 64.0f;
            float targetArea = minDimension * minDimension;
            float minScale = sqrt(targetArea / uvArea);
            if (scale < minScale)
            {
                scale = minScale;
            }
        }
    }

    // Final quality knob adjustment
    if (ds->lightmapScale > 0.0f)
        scale *= ds->lightmapScale;

    // Extra boost to ensure small triangles capture at least one texel center
    if (guessUVs)
    {
        scale *= 1.1f;
    }


    // Limit lightmap size and adjust scale proportionally
    // so no UV coordinates fall outside the allocated block.
    w = ceil((max_s - min_s) * scale) + 1;
    h = ceil((max_t - min_t) * scale) + 1;

    if (w > LIGHTMAP_WIDTH - 2 || h > LIGHTMAP_HEIGHT - 2)
    {
        float scaleX = scale;
        float scaleY = scale;

        if (w > LIGHTMAP_WIDTH - 2)
        {
            scaleX = (float)(LIGHTMAP_WIDTH - 3) / (max_s - min_s);
        }
        if (h > LIGHTMAP_HEIGHT - 2)
        {
            scaleY = (float)(LIGHTMAP_HEIGHT - 3) / (max_t - min_t);
        }

        // Use the more restrictive scale to keep aspect ratio
        scale = (scaleX < scaleY) ? scaleX : scaleY;

        w = ceil((max_s - min_s) * scale) + 1;
        h = ceil((max_t - min_t) * scale) + 1;
    }

    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;

    // 4. Allocation (including 1-texel padding on all sides)
    qboolean allocated_success = qfalse;
    for (i = 0; i < numLightmaps; i++)
    {
        if (AllocLMBlock(i, w + 2, h + 2, &x, &y))
        {
            ds->lightmapNum = i;
            allocated_success = qtrue;
            break;
        }
    }

    if (!allocated_success)
    {
        PrepareNewLightmap();
        if (!AllocLMBlock(numLightmaps - 1, w + 2, h + 2, &x, &y))
        {
            Error("misc_model: Lightmap allocation failed");
        }
        ds->lightmapNum = numLightmaps - 1;
    }
    ds->lightmapWidth = w;
    ds->lightmapHeight = h;
    ds->lightmapX = x + 1;
    ds->lightmapY = y + 1;

    for (i = 0; i < ds->numVerts; i++)
    {
        ds->verts[i].lightmap[0][0] =
            (x + 1.0f + 0.5f + (ds->verts[i].lightmap[0][0] - min_s) * scale) /
            LIGHTMAP_WIDTH;
        ds->verts[i].lightmap[0][1] =
            (y + 1.0f + 0.5f + (ds->verts[i].lightmap[0][1] - min_t) * scale) /
            LIGHTMAP_HEIGHT;
    }
}

static qboolean CheckPatchPlanar(mapDrawSurface_t *ds, vec3_t outNormal) {
    int numVerts = ds->patchWidth * ds->patchHeight;
    if (numVerts < 3) return qfalse;

    vec3_t p0, p1, p2, n;
    VectorCopy(ds->verts[0].xyz, p0);

    qboolean found = qfalse;
    int i, j;
    for (i = 1; i < numVerts - 1; i++) {
        for (j = i + 1; j < numVerts; j++) {
            VectorSubtract(ds->verts[i].xyz, p0, p1);
            VectorSubtract(ds->verts[j].xyz, p0, p2);
            CrossProduct(p1, p2, n);
            if (VectorNormalize(n, n) > 0.001f) {
                found = qtrue;
                break;
            }
        }
        if (found) break;
    }

    if (!found) return qfalse;

    float dist = DotProduct(p0, n);
    float maxDist = 0.0f;
    for (i = 0; i < numVerts; i++) {
        float d = DotProduct(ds->verts[i].xyz, n);
        float dev = fabs(d - dist);
        if (dev > maxDist) maxDist = dev;
    }

    if (maxDist <= 0.1f) {
        if (outNormal) VectorCopy(n, outNormal);
        return qtrue;
    }
    return qfalse;
}

static void AllocateLightmapForPlanarPatch(mapDrawSurface_t *ds, vec3_t planeNormal)
{
    vec3_t mins, maxs, size, delta;
    int i;
    drawVert_t *verts;
    int w, h;
    int x, y, ssize;
    int axis;
    vec3_t vecs[2];
    float s, t;
    vec3_t origin;
    float d;
    vec3_t absNormal;

    ssize = (int)ds->samplesize;
    verts = ds->verts;
    int numVerts = ds->patchWidth * ds->patchHeight;

    ClearBounds(mins, maxs);
    for (i = 0; i < numVerts; i++)
    {
        AddPointToBounds(verts[i].xyz, mins, maxs);
    }

    for (i = 0; i < 3; i++)
    {
        mins[i] = ssize * floor(mins[i] / ssize);
        maxs[i] = ssize * ceil(maxs[i] / ssize);
        size[i] = (maxs[i] - mins[i]) / ssize + 1;
    }

    memset(vecs, 0, sizeof(vecs));

    absNormal[0] = fabs(planeNormal[0]);
    absNormal[1] = fabs(planeNormal[1]);
    absNormal[2] = fabs(planeNormal[2]);

    if (absNormal[0] >= absNormal[1] && absNormal[0] >= absNormal[2])
    {
        w = size[1];
        h = size[2];
        axis = 0;
        vecs[0][1] = 1.0 / ssize;
        vecs[1][2] = 1.0 / ssize;
    }
    else if (absNormal[1] >= absNormal[0] && absNormal[1] >= absNormal[2])
    {
        w = size[0];
        h = size[2];
        axis = 1;
        vecs[0][0] = 1.0 / ssize;
        vecs[1][2] = 1.0 / ssize;
    }
    else
    {
        w = size[0];
        h = size[1];
        axis = 2;
        vecs[0][0] = 1.0 / ssize;
        vecs[1][1] = 1.0 / ssize;
    }

    if (!planeNormal[axis])
    {
        Error("Chose a 0 valued axis");
    }

    if (w > LIGHTMAP_WIDTH - 2)
    {
        VectorScale(vecs[0], (float)(LIGHTMAP_WIDTH - 2) / w, vecs[0]);
        w = LIGHTMAP_WIDTH - 2;
    }

    if (h > LIGHTMAP_HEIGHT - 2)
    {
        VectorScale(vecs[1], (float)(LIGHTMAP_HEIGHT - 2) / h, vecs[1]);
        h = LIGHTMAP_HEIGHT - 2;
    }

    c_exactLightmap += (w + 2) * (h + 2);

    qboolean allocated = qfalse;
    for (i = 0; i < numLightmaps; i++)
    {
        if (AllocLMBlock(i, w + 2, h + 2, &x, &y))
        {
            ds->lightmapNum = i;
            allocated = qtrue;
            break;
        }
    }

    if (!allocated)
    {
        PrepareNewLightmap();
        if (!AllocLMBlock(numLightmaps - 1, w + 2, h + 2, &x, &y))
        {
            Error("Entity %i, brush %i: Planar patch lightmap allocation failed",
                  ds->mapBrush->entitynum, ds->mapBrush->brushnum);
        }
        ds->lightmapNum = numLightmaps - 1;
    }

    ds->lightmapWidth = w;
    ds->lightmapHeight = h;
    ds->lightmapX = x + 1;
    ds->lightmapY = y + 1;

    x = ds->lightmapX;
    y = ds->lightmapY;

    for (i = 0; i < numVerts; i++)
    {
        VectorSubtract(verts[i].xyz, mins, delta);
        s = DotProduct(delta, vecs[0]) + x + 0.5f;
        t = DotProduct(delta, vecs[1]) + y + 0.5f;

        verts[i].lightmap[0][0] = s / LIGHTMAP_WIDTH;
        if (s <= (float)x + 0.5001f)
            verts[i].lightmap[0][0] -= UV_PRECISION_NUDGE;
        if (s >= (float)(x + w) - 0.5001f)
            verts[i].lightmap[0][0] += UV_PRECISION_NUDGE;

        verts[i].lightmap[0][1] = t / LIGHTMAP_HEIGHT;
        if (t <= (float)y + 0.5001f)
            verts[i].lightmap[0][1] -= UV_PRECISION_NUDGE;
        if (t >= (float)(y + h) - 0.5001f)
            verts[i].lightmap[0][1] += UV_PRECISION_NUDGE;
    }

    float planeDist = DotProduct(verts[0].xyz, planeNormal);

    d = DotProduct(mins, planeNormal) - planeDist;
    d /= planeNormal[axis];
    VectorCopy(mins, origin);
    origin[axis] -= d;

    for (i = 0; i < 2; i++)
    {
        vec3_t normalized;
        float len;

        len = VectorNormalize(vecs[i], normalized);
        VectorScale(normalized, (1.0 / len), vecs[i]);
        d = DotProduct(vecs[i], planeNormal);
        d /= planeNormal[axis];
        vecs[i][axis] -= d;
    }

    VectorCopy(origin, ds->lightmapOrigin);
    VectorCopy(vecs[0], ds->lightmapVecs[0]);
    VectorCopy(vecs[1], ds->lightmapVecs[1]);
    VectorCopy(planeNormal, ds->lightmapVecs[2]);
}

void AllocateLightmapForPatch(mapDrawSurface_t *ds)
{
    int i, j;
    drawVert_t *verts;
    int w, h;
    int x, y, ssize;
    float s, t;
    mesh_t srcMesh, *subdiv;
    float maxLengthS, maxLengthT, lengthS, lengthT;
    vec3_t delta;
    float S_basis, T_basis;

    verts = ds->verts;
    ssize = (int)ds->samplesize;

    /* Step 1: Temporarily tessellate the patch to measure its physical arc lengths.
       We use SubdivideMesh + PutMeshOnCurve (same as the lighting tessellation path)
       but immediately discard the result after measuring. */
    srcMesh.width  = ds->patchWidth;
    srcMesh.height = ds->patchHeight;
    srcMesh.verts  = verts;

    if (IsMeshPlanar(&srcMesh))
    {
        /* Old b013afa4ecdb7a5a75e55613c2d216e168302162 logic strictly for planar patches */
        int widthtable[1024], heighttable[1024];
        mesh_t *subdividedMesh, *tempMesh, *newmesh;
        
        newmesh = SubdivideMesh(srcMesh, 8, 999);
        PutMeshOnCurve(*newmesh);
        tempMesh = RemoveLinearMeshColumnsRows(newmesh);
        FreeMesh(newmesh);

        subdividedMesh = SubdivideMeshQuads(tempMesh, ssize, LIGHTMAP_WIDTH - 2,
                                            widthtable, heighttable);

        w = subdividedMesh->width;
        h = subdividedMesh->height;

        FreeMesh(subdividedMesh);

        /* Step 3: Allocate the lightmap block (1-texel padding on all sides). */
        c_exactLightmap += (w + 2) * (h + 2);

        qboolean allocated_patch_success = qfalse;
        for (i = 0; i < numLightmaps; i++)
        {
            if (AllocLMBlock(i, w + 2, h + 2, &x, &y))
            {
                ds->lightmapNum = i;
                allocated_patch_success = qtrue;
                break;
            }
        }

        if (!allocated_patch_success)
        {
            PrepareNewLightmap();
            if (!AllocLMBlock(numLightmaps - 1, w + 2, h + 2, &x, &y))
            {
                Error("Entity %i, brush %i: Patch lightmap allocation failed",
                      ds->mapBrush->entitynum, ds->mapBrush->brushnum);
            }
            ds->lightmapNum = numLightmaps - 1;
        }

        ds->lightmapWidth = w;
        ds->lightmapHeight = h;
        ds->lightmapX = x + 1;
        ds->lightmapY = y + 1;

        x = ds->lightmapX;
        y = ds->lightmapY;

        for (i = 0; i < ds->patchWidth; i++)
        {
            int k_w;
            for (k_w = 0; k_w < w; k_w++)
            {
                if (originalWidths[k_w] >= i)
                {
                    break;
                }
            }
            if (k_w >= w)
                k_w = w - 1;
            s = x + k_w + 0.5f;
            for (j = 0; j < ds->patchHeight; j++)
            {
                int k_h;
                for (k_h = 0; k_h < h; k_h++)
                {
                    if (originalHeights[k_h] >= j)
                    {
                        break;
                    }
                }
                if (k_h >= h)
                    k_h = h - 1;
                t = y + k_h + 0.5f;
                verts[i + j * ds->patchWidth].lightmap[0][0] = s / (float)LIGHTMAP_WIDTH;
                verts[i + j * ds->patchWidth].lightmap[0][1] = t / (float)LIGHTMAP_HEIGHT;
            }
        }

        /* precision nudge pass: shift UVs slightly outward to prevent float point inaccuracies */
        for (i = 0; i < ds->patchWidth * ds->patchHeight; i++)
        {
            float *uv = verts[i].lightmap[0];
            if (uv[0] <= (float)x / LIGHTMAP_WIDTH + 0.50001f / LIGHTMAP_WIDTH)
                uv[0] -= UV_PRECISION_NUDGE;
            if (uv[0] >= (float)(x + w) / LIGHTMAP_WIDTH - 0.50001f / LIGHTMAP_WIDTH)
                uv[0] += UV_PRECISION_NUDGE;
            if (uv[1] <= (float)y / LIGHTMAP_HEIGHT + 0.50001f / LIGHTMAP_HEIGHT)
                uv[1] -= UV_PRECISION_NUDGE;
            if (uv[1] >= (float)(y + h) / LIGHTMAP_HEIGHT - 0.50001f / LIGHTMAP_HEIGHT)
                uv[1] += UV_PRECISION_NUDGE;
        }
        return;
    }

    subdiv = SubdivideMesh(srcMesh, 8, 999);
    PutMeshOnCurve(*subdiv);

    /* Measure maximum segment length for each column/row (q3map2 widthTable/heightTable logic) */
    float *widthTable = malloc(subdiv->width * sizeof(float));
    float *heightTable = malloc(subdiv->height * sizeof(float));
    memset(widthTable, 0, subdiv->width * sizeof(float));
    memset(heightTable, 0, subdiv->height * sizeof(float));

    for (j = 0; j < subdiv->height; j++)
    {
        for (i = 0; i < subdiv->width; i++)
        {
            if (i + 1 < subdiv->width)
            {
                VectorSubtract(subdiv->verts[j * subdiv->width + i + 1].xyz,
                               subdiv->verts[j * subdiv->width + i].xyz, delta);
                float len = VectorLength(delta);
                if (len > widthTable[i])
                    widthTable[i] = len;
            }
            if (j + 1 < subdiv->height)
            {
                VectorSubtract(subdiv->verts[(j + 1) * subdiv->width + i].xyz,
                               subdiv->verts[j * subdiv->width + i].xyz, delta);
                float len = VectorLength(delta);
                if (len > heightTable[j])
                    heightTable[j] = len;
            }
        }
    }

    /* Sum the maximum segments to get the total lightmap dimension */
    maxLengthS = 0;
    for (i = 0; i < subdiv->width - 1; i++)
        maxLengthS += widthTable[i];

    maxLengthT = 0;
    for (j = 0; j < subdiv->height - 1; j++)
        maxLengthT += heightTable[j];

    free(widthTable);
    free(heightTable);

    FreeMesh(subdiv);

    /* Step 2: Compute lightmap dimensions from physical arc lengths.
       W_lm = ceil(maxLengthS / ssize) + 1
       H_lm = ceil(maxLengthT / ssize) + 1
       The +1 ensures both endpoints of the arc have a dedicated texel. */
    w = (int)ceil(maxLengthS / ssize) + 1;
    h = (int)ceil(maxLengthT / ssize) + 1;

    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > LIGHTMAP_WIDTH  - 2) w = LIGHTMAP_WIDTH  - 2;
    if (h > LIGHTMAP_HEIGHT - 2) h = LIGHTMAP_HEIGHT - 2;

    /* Step 3: Allocate the lightmap block (1-texel padding on all sides). */
    c_exactLightmap += (w + 2) * (h + 2);

    qboolean allocated_patch_success = qfalse;
    for (i = 0; i < numLightmaps; i++)
    {
        if (AllocLMBlock(i, w + 2, h + 2, &x, &y))
        {
            ds->lightmapNum = i;
            allocated_patch_success = qtrue;
            break;
        }
    }

    if (!allocated_patch_success)
    {
        PrepareNewLightmap();
        if (!AllocLMBlock(numLightmaps - 1, w + 2, h + 2, &x, &y))
        {
            Error("Entity %i, brush %i: Patch lightmap allocation failed",
                  ds->mapBrush->entitynum, ds->mapBrush->brushnum);
        }
        ds->lightmapNum = numLightmaps - 1;
    }

    ds->lightmapWidth  = w;
    ds->lightmapHeight = h;
    ds->lightmapX = x + 1;
    ds->lightmapY = y + 1;

    x = ds->lightmapX;
    y = ds->lightmapY;

    /* Step 4: Assign UV coordinates to the coarse control points using a
       uniform linear basis.
         S_basis = (W_lm - 1) / (patchWidth  - 1)
         T_basis = (H_lm - 1) / (patchHeight - 1)
       At col=0:           s = x + 0.5  (center of first texel)
       At col=patchWidth-1: s = x + W_lm - 0.5 (center of last texel)
       This guarantees a perfect 0.5-texel boundary on both sides. */
    S_basis = (ds->patchWidth  > 1) ? (float)(w - 1) / (float)(ds->patchWidth  - 1) : 0.0f;
    T_basis = (ds->patchHeight > 1) ? (float)(h - 1) / (float)(ds->patchHeight - 1) : 0.0f;

    for (j = 0; j < ds->patchHeight; j++)
    {
        for (i = 0; i < ds->patchWidth; i++)
        {
            s = x + i * S_basis + 0.5f;
            t = y + j * T_basis + 0.5f;
            verts[j * ds->patchWidth + i].lightmap[0][0] = s / (float)LIGHTMAP_WIDTH;
            verts[j * ds->patchWidth + i].lightmap[0][1] = t / (float)LIGHTMAP_HEIGHT;
        }
    }
}

/*
===================
AllocateLightmapForSurface

===================
*/
// #define	LIGHTMAP_BLOCK	16
void AllocateLightmapForSurface(mapDrawSurface_t *ds)
{
    vec3_t mins, maxs, size, delta;
    int i;
    drawVert_t *verts;
    int w, h;
    int x, y, ssize;
    int axis;
    vec3_t vecs[2];
    float s, t;
    vec3_t origin;
    plane_t *plane;
    float d;
    vec3_t planeNormal;

    if (ds->patch)
    {
        vec3_t planeNormal;
        if (CheckPatchPlanar(ds, planeNormal))
        {
            AllocateLightmapForPlanarPatch(ds, planeNormal);
        }
        else
        {
            AllocateLightmapForPatch(ds);
        }
        return;
    }

    ssize = (int)ds->samplesize;

    plane = &mapplanes[ds->side->planenum];

    // bound the surface
    ClearBounds(mins, maxs);
    verts = ds->verts;
    for (i = 0; i < ds->numVerts; i++)
    {
        AddPointToBounds(verts[i].xyz, mins, maxs);
    }

    // round to the lightmap resolution
    for (i = 0; i < 3; i++)
    {
        mins[i] = ssize * floor(mins[i] / ssize);
        maxs[i] = ssize * ceil(maxs[i] / ssize);
        size[i] = (maxs[i] - mins[i]) / ssize + 1;
    }

    // the two largest axis will be the lightmap size
    memset(vecs, 0, sizeof(vecs));

    planeNormal[0] = fabs(plane->normal[0]);
    planeNormal[1] = fabs(plane->normal[1]);
    planeNormal[2] = fabs(plane->normal[2]);

    if (planeNormal[0] >= planeNormal[1] && planeNormal[0] >= planeNormal[2])
    {
        w = size[1];
        h = size[2];
        axis = 0;
        vecs[0][1] = 1.0 / ssize;
        vecs[1][2] = 1.0 / ssize;
    }
    else if (planeNormal[1] >= planeNormal[0] &&
             planeNormal[1] >= planeNormal[2])
    {
        w = size[0];
        h = size[2];
        axis = 1;
        vecs[0][0] = 1.0 / ssize;
        vecs[1][2] = 1.0 / ssize;
    }
    else
    {
        w = size[0];
        h = size[1];
        axis = 2;
        vecs[0][0] = 1.0 / ssize;
        vecs[1][1] = 1.0 / ssize;
    }

    if (!plane->normal[axis])
    {
        Error("Chose a 0 valued axis");
    }

    if (w > LIGHTMAP_WIDTH - 2)
    {
        VectorScale(vecs[0], (float)(LIGHTMAP_WIDTH - 2) / w, vecs[0]);
        w = LIGHTMAP_WIDTH - 2;
    }

    if (h > LIGHTMAP_HEIGHT - 2)
    {
        VectorScale(vecs[1], (float)(LIGHTMAP_HEIGHT - 2) / h, vecs[1]);
        h = LIGHTMAP_HEIGHT - 2;
    }

    // allocate the lightmap (including 1-texel padding on all sides)
    c_exactLightmap += (w + 2) * (h + 2);

    qboolean allocated_surf_success = qfalse;
    for (i = 0; i < numLightmaps; i++)
    {
        if (AllocLMBlock(i, w + 2, h + 2, &x, &y))
        {
            ds->lightmapNum = i;
            allocated_surf_success = qtrue;
            break;
        }
    }

    if (!allocated_surf_success)
    {
        PrepareNewLightmap();
        if (!AllocLMBlock(numLightmaps - 1, w + 2, h + 2, &x, &y))
        {
            Error("Entity %i, brush %i: Surface lightmap allocation failed",
                  ds->mapBrush->entitynum, ds->mapBrush->brushnum);
        }
        ds->lightmapNum = numLightmaps - 1;
    }

    // set the lightmap texture coordinates in the drawVerts
    // we add 1 to x and y to account for the padding gutter
    ds->lightmapWidth = w;
    ds->lightmapHeight = h;
    ds->lightmapX = x + 1;
    ds->lightmapY = y + 1;

    x = ds->lightmapX;
    y = ds->lightmapY;

    for (i = 0; i < ds->numVerts; i++)
    {
        VectorSubtract(verts[i].xyz, mins, delta);
        s = DotProduct(delta, vecs[0]) + x + 0.5f;
        t = DotProduct(delta, vecs[1]) + y + 0.5f;

        verts[i].lightmap[0][0] = s / LIGHTMAP_WIDTH;
        if (s <= (float)x + 0.5001f)
            verts[i].lightmap[0][0] -= UV_PRECISION_NUDGE;
        if (s >= (float)(x + w) - 0.5001f)
            verts[i].lightmap[0][0] += UV_PRECISION_NUDGE;

        verts[i].lightmap[0][1] = t / LIGHTMAP_HEIGHT;
        if (t <= (float)y + 0.5001f)
            verts[i].lightmap[0][1] -= UV_PRECISION_NUDGE;
        if (t >= (float)(y + h) - 0.5001f)
            verts[i].lightmap[0][1] += UV_PRECISION_NUDGE;
    }

    // calculate the world coordinates of the lightmap samples

    // project mins onto plane to get origin
    d = DotProduct(mins, plane->normal) - plane->dist;
    d /= plane->normal[axis];
    VectorCopy(mins, origin);
    origin[axis] -= d;

    // project stepped lightmap blocks and subtract to get planevecs
    for (i = 0; i < 2; i++)
    {
        vec3_t normalized;
        float len;

        len = VectorNormalize(vecs[i], normalized);
        VectorScale(normalized, (1.0 / len), vecs[i]);
        d = DotProduct(vecs[i], plane->normal);
        d /= plane->normal[axis];
        vecs[i][axis] -= d;
    }

    VectorCopy(origin, ds->lightmapOrigin);
    VectorCopy(vecs[0], ds->lightmapVecs[0]);
    VectorCopy(vecs[1], ds->lightmapVecs[1]);
    VectorCopy(plane->normal, ds->lightmapVecs[2]);
}

// =========================================================================================
// ===== REPACK PASS (MAXRECTS) =====
// =========================================================================================

typedef struct {
    mapDrawSurface_t *ds;
    int area;
} sortSurf_t;

typedef struct {
    int x, y, w, h;
} lmRect_t;

typedef struct {
    lmRect_t *freeRects;
    int numFreeRects;
    int capFreeRects;
} maxRectPage_t;

static maxRectPage_t *lmPages = NULL;
static int numLMPages = 0;

static void InitMaxRectPage(maxRectPage_t *page) {
    page->capFreeRects  = 64;
    page->numFreeRects  = 1;
    page->freeRects     = malloc(page->capFreeRects * sizeof(lmRect_t));
    page->freeRects[0].x = 0;
    page->freeRects[0].y = 0;
    page->freeRects[0].w = LIGHTMAP_WIDTH;
    page->freeRects[0].h = LIGHTMAP_HEIGHT;
}

static void FreeMaxRectPage(maxRectPage_t *page) {
    if (page->freeRects) free(page->freeRects);
    page->freeRects = NULL;
    page->numFreeRects = page->capFreeRects = 0;
}

static void PushFreeRect(maxRectPage_t *page, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (page->numFreeRects == page->capFreeRects) {
        page->capFreeRects *= 2;
        page->freeRects = realloc(page->freeRects, page->capFreeRects * sizeof(lmRect_t));
    }
    page->freeRects[page->numFreeRects].x = x;
    page->freeRects[page->numFreeRects].y = y;
    page->freeRects[page->numFreeRects].w = w;
    page->freeRects[page->numFreeRects].h = h;
    page->numFreeRects++;
}

static qboolean SplitFreeNode(maxRectPage_t *page, const lmRect_t *freeNode, const lmRect_t *used) {
    if (used->x >= freeNode->x + freeNode->w || used->x + used->w <= freeNode->x ||
        used->y >= freeNode->y + freeNode->h || used->y + used->h <= freeNode->y)
        return qfalse;

    if (used->x > freeNode->x)
        PushFreeRect(page, freeNode->x, freeNode->y, used->x - freeNode->x, freeNode->h);
    if (used->x + used->w < freeNode->x + freeNode->w)
        PushFreeRect(page, used->x + used->w, freeNode->y, (freeNode->x + freeNode->w) - (used->x + used->w), freeNode->h);
    if (used->y > freeNode->y)
        PushFreeRect(page, freeNode->x, freeNode->y, freeNode->w, used->y - freeNode->y);
    if (used->y + used->h < freeNode->y + freeNode->h)
        PushFreeRect(page, freeNode->x, used->y + used->h, freeNode->w, (freeNode->y + freeNode->h) - (used->y + used->h));

    return qtrue;
}

static void PruneFreeList(maxRectPage_t *page) {
    int i, j;
    for (i = page->numFreeRects - 1; i >= 0; i--) {
        for (j = 0; j < page->numFreeRects; j++) {
            if (i == j) continue;
            if (page->freeRects[j].x <= page->freeRects[i].x && page->freeRects[j].y <= page->freeRects[i].y &&
                page->freeRects[j].x + page->freeRects[j].w >= page->freeRects[i].x + page->freeRects[i].w &&
                page->freeRects[j].y + page->freeRects[j].h >= page->freeRects[i].y + page->freeRects[i].h)
            {
                page->freeRects[i] = page->freeRects[--page->numFreeRects];
                break;
            }
        }
    }
}

static qboolean AllocMaxRectBlock(int w, int h, int *outX, int *outY, int *outLM) {
    int bestArea = 999999999;
    int bestPage = -1;
    lmRect_t bestNode;
    int p, i, origCount;
    maxRectPage_t *page;

    bestNode.x = bestNode.y = bestNode.w = bestNode.h = 0;

    for (p = 0; p < numLMPages; p++) {
        for (i = 0; i < lmPages[p].numFreeRects; i++) {
            lmRect_t *f = &lmPages[p].freeRects[i];
            if (f->w >= w && f->h >= h) {
                int areaFit = (f->w * f->h) - (w * h);
                if (areaFit < bestArea) {
                    bestArea = areaFit;
                    bestPage = p;
                    bestNode.x = f->x;
                    bestNode.y = f->y;
                    bestNode.w = w;
                    bestNode.h = h;
                }
            }
        }
    }

    if (bestPage == -1) {
        if (numLMPages >= MAX_LIGHTMAPS)
            Error("RepackLightmaps: MAX_LIGHTMAPS exceeded during repack");

        InitMaxRectPage(&lmPages[numLMPages]);
        bestPage = numLMPages++;
        bestNode.x = 0;
        bestNode.y = 0;
        bestNode.w = w;
        bestNode.h = h;
    }

    page = &lmPages[bestPage];
    origCount = page->numFreeRects;
    for (i = origCount - 1; i >= 0; i--) {
        lmRect_t old = page->freeRects[i];
        if (SplitFreeNode(page, &old, &bestNode)) {
            page->freeRects[i] = page->freeRects[--page->numFreeRects];
        }
    }
    PruneFreeList(page);

    *outX = bestNode.x;
    *outY = bestNode.y;
    *outLM = bestPage;
    return qtrue;
}

static int CompareSortSurfAreaDesc(const void *a, const void *b) {
    const sortSurf_t *sa = (const sortSurf_t *)a;
    const sortSurf_t *sb = (const sortSurf_t *)b;
    return sb->area - sa->area;
}

static void RepackLightmapsEntity(entity_t *e) {
    int i, v, p;
    int sortCount = 0;
    sortSurf_t *sortArray;
    int oldNumLightmaps = numLightmaps;

    if (numMapDrawSurfs - e->firstDrawSurf <= 0)
        return;

    sortArray = malloc((numMapDrawSurfs - e->firstDrawSurf) * sizeof(sortSurf_t));

    for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++) {
        mapDrawSurface_t *ds = &mapDrawSurfs[i];
        if (ds->lightmapNum >= 0 && ds->lightmapWidth > 0 && ds->numVerts > 0) {
            sortArray[sortCount].ds   = ds;
            sortArray[sortCount].area = (ds->lightmapWidth + 2) * (ds->lightmapHeight + 2);
            sortCount++;
        }
    }

    if (sortCount == 0) {
        free(sortArray);
        return;
    }

    qsort(sortArray, sortCount, sizeof(sortSurf_t), CompareSortSurfAreaDesc);

    // Do NOT free lightmapHeights here; Skyline needs it for the next entity's initial allocation
    
    if (!lmPages) {
        lmPages = calloc(MAX_LIGHTMAPS, sizeof(maxRectPage_t));
        numLMPages = 0;
    }

    for (i = 0; i < sortCount; i++) {
        mapDrawSurface_t *ds = sortArray[i].ds;
        int newX, newY, newLM;
        float du, dv;
        int new_lightmapX, new_lightmapY;

        AllocMaxRectBlock(ds->lightmapWidth + 2, ds->lightmapHeight + 2, &newX, &newY, &newLM);

        new_lightmapX = newX + 1;
        new_lightmapY = newY + 1;

        du = (float)(new_lightmapX - ds->lightmapX) / (float)LIGHTMAP_WIDTH;
        dv = (float)(new_lightmapY - ds->lightmapY) / (float)LIGHTMAP_HEIGHT;

        for (v = 0; v < ds->numVerts; v++) {
            ds->verts[v].lightmap[0][0] += du;
            ds->verts[v].lightmap[0][1] += dv;
        }

        ds->lightmapNum = newLM;
        ds->lightmapX   = new_lightmapX;
        ds->lightmapY   = new_lightmapY;
    }

    numLightmaps  = numLMPages;
    numLightBytes = numLightmaps * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3;
    if (lightBytes) Q_Free(lightBytes);
    lightBytes = Q_Alloc(numLightBytes);
    if (!lightBytes && numLightBytes > 0)
        Error("RepackLightmaps: failed to reallocate lightBytes");
    if (lightBytes) memset(lightBytes, 0, numLightBytes);

    free(sortArray);
    // DO NOT free lmPages here; it persists across entities and is freed in FreeLightmaps

    qprintf("--- Lightmap Repacking ---\n");
    qprintf("Pages reduced: %d -> %d\n", oldNumLightmaps, numLightmaps);
}

/*
===================
AllocateLightmaps
===================
*/
void AllocateLightmaps(entity_t *e)
{
    int i, j;
    mapDrawSurface_t *ds;
    shaderInfo_t *si;

    qprintf("--- AllocateLightmaps ---\n");
    _printf("Lightmap image size: %d\n", game->lightmapSize);

    if (!lightmapHeights)
    {
        lightmapHeights = calloc(MAX_LIGHTMAPS * MAX_LIGHTMAP_WIDTH, sizeof(int));
        numLightmaps = 0;
        PrepareNewLightmap(); // Start with the first lightmap
    }

    // sort all surfaces by shader so common shaders will usually
    // be in the same lightmap
    numSortShaders = 0;

    for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++)
    {
        ds = &mapDrawSurfs[i];
        if (!ds->numVerts)
        {
            continue; // leftover from a surface subdivision
        }

        // Permanently snap samplesize to a power of 2 for planar surfaces and patches.
        // These allocators project a world-space grid via floor/ceil math that requires
        // exact powers of 2 to prevent lightmap seams between adjacent faces.
        // Trisoups (miscModel) intentionally bypass this to retain exact float precision
        // for xatlas UV packing.
        if (!ds->miscModel && ds->samplesize > 0.0f && (ds->side || ds->patch))
            ds->samplesize = SnapToNearestPowerOfTwo(ds->samplesize);

        if (!ds->patch && !ds->miscModel && ds->side)
        {
            VectorCopy(mapplanes[ds->side->planenum].normal, ds->lightmapVecs[2]);
        }

        // search for this shader
        for (j = 0; j < numSortShaders; j++)
        {
            if (ds->shaderInfo == surfsOnShader[j]->shaderInfo)
            {
                ds->nextOnShader = surfsOnShader[j];
                surfsOnShader[j] = ds;
                break;
            }
        }
        if (j == numSortShaders)
        {
            if (numSortShaders >= MAX_MAP_SHADERS)
            {
                Error("MAX_MAP_SHADERS");
            }
            surfsOnShader[j] = ds;
            numSortShaders++;
        }
    }
    qprintf("%5i unique shaders\n", numSortShaders);

    // for each shader, allocate lightmaps for each surface

    //	numLightmaps = 0;
    //	PrepareNewLightmap();

    for (i = 0; i < numSortShaders; i++)
    {
        si = surfsOnShader[i]->shaderInfo;

        for (ds = surfsOnShader[i]; ds; ds = ds->nextOnShader)
        {
            // some surfaces don't need lightmaps allocated for them
            if (si->surfaceFlags & SURF_NOLIGHTMAP)
            {
                ds->lightmapNum = -1;
                if (ds->miscModel)
                    _printf("TriSoup surface skipped (SURF_NOLIGHTMAP): shader %s\n",
                            si->shader);
            }
            else if (si->surfaceFlags & SURF_POINTLIGHT)
            {
                ds->lightmapNum = -3;
                if (ds->miscModel)
                    _printf("TriSoup surface skipped (SURF_POINTLIGHT): shader %s\n",
                            si->shader);
            }
            else
            {
                if (ds->miscModel)
                {
                    AllocateLightmapForMiscModel(ds);
                }
                else
                {
                    AllocateLightmapForSurface(ds);
                }
            }
        }
    }
    extern int totalLightmappedShaders;
    totalLightmappedShaders += numSortShaders;

    RepackLightmapsEntity(e);

    qprintf("%7i exact lightmap texels\n", c_exactLightmap);
    qprintf("%7i block lightmap texels\n", numLightBytes);
}

void FreeLightmaps(void)
{
    int p;
    if (lightmapHeights)
    {
        free(lightmapHeights);
        lightmapHeights = NULL;
    }
    
    if (lmPages)
    {
        for (p = 0; p < numLMPages; p++) {
            FreeMaxRectPage(&lmPages[p]);
        }
        free(lmPages);
        lmPages = NULL;
    }
}
