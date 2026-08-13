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

mapDrawSurface_t *mapDrawSurfs;
int numMapDrawSurfs;
extraSurface_t *drawExtraSurfaces;

/*
=============================================================================

DRAWSURF CONSTRUCTION

=============================================================================
*/

/*
=================
AllocDrawSurf
=================
*/

float SnapToNearestPowerOfTwo(float value)
{
    if (value <= 1.0f) return 1.0f;

    // Find the largest power of 2 that is <= value
    int lower = 1;
    while ((lower << 1) <= (int)value)
        lower <<= 1;
    int upper = lower << 1;

    // Rule: if value is under 4.0, always ceil (prevents dangerously tiny grids)
    // Exception: if value already exactly equals a power of 2, return it as-is.
    if (value < 4.0f)
        return (value > (float)lower) ? (float)upper : (float)lower;

    // Rule: round to the nearest power of 2. Ties round up.
    return ((value - (float)lower) < ((float)upper - value)) ? (float)lower : (float)upper;
}

/*
=================
AllocDrawSurf
=================
*/
mapDrawSurface_t *AllocDrawSurf(void)
{
    mapDrawSurface_t *ds;

    if (numMapDrawSurfs >= game->maxMapDrawSurfs)
    {
        Error("MAX_MAP_DRAW_SURFS (%d)", game->maxMapDrawSurfs);
    }

    ds = &mapDrawSurfs[numMapDrawSurfs];
    numMapDrawSurfs++;

    memset(ds, 0, sizeof(*ds));

    ds->samplesize = game->defaultSampleSize;
    ds->smoothingRadius = -1.0f;
    ds->lightValue = -1.0f;
    VectorSet(ds->lightColor, -1.0f, -1.0f, -1.0f);
    ds->backsplashFraction = -1.0f;
    ds->lightSubdivide = -1.0f;
    ds->superSampleRadius = -1.0f;
    ds->upscale = 0;
    ds->enforceSampleSize = game->enforceSampleSize;
    ds->noDeluxeInfluence = -1;
    ds->noDeluxeInfluenceBacksplash = -1;
    ds->gridAmbientScale = -1.0f;
    ds->gridDirectScale = -1.0f;
    ds->castShadows = -1;
    ds->parentSurfaceNum = -1;
    ds->chamferConvexWidth = -1.0f;
    ds->chamferConcaveWidth = -1.0f;

    return ds;
}

/*
=================
DrawSurfaceForSide
=================
*/
#define SNAP_FLOAT_TO_INT 8
#define SNAP_INT_TO_FLOAT (1.0 / SNAP_FLOAT_TO_INT)

/*
=================
ResolveSurfaceExtraProperties
=================
*/
static void ResolveSurfaceExtraProperties(mapDrawSurface_t *ds, epair_t *epairs)
{
    // Resolve smoothing radius
    float globalSmooth = game->defaultSmoothRadius;
    const char *wsSmooth = ValueForKey(&entities[0], "smooth");
    if (wsSmooth[0])
        globalSmooth = atof(wsSmooth);

    if (ds->shaderInfo && ds->shaderInfo->minSmoothRadius >= 0.0f && ds->shaderInfo->minSmoothRadius > globalSmooth)
        ds->smoothingRadius = ds->shaderInfo->minSmoothRadius;

    const char *radiusStr = ValueForEpair(epairs, "smooth");
    if (radiusStr[0])
        ds->smoothingRadius = atof(radiusStr);

    // Resolve lighting overrides
    const char *type = ValueForEpair(epairs, "type");
    if (!Q_stricmp(type, "surface") || !Q_stricmp(type, "surfacelight"))
    {
        const char *lightStr = ValueForEpair(epairs, "light");
        if (lightStr[0])
            ds->lightValue = atof(lightStr);

        const char *colorStr = ValueForEpair(epairs, "color");
        if (colorStr[0])
            ParseColor(colorStr, ds->lightColor);

        const char *bsStr = ValueForEpair(epairs, "backsplash");
        if (bsStr[0])
            ds->backsplashFraction = atof(bsStr) * 0.01f; // Convert percentage to fraction

        const char *subdivideStr = ValueForEpair(epairs, "subdivide");
        if (subdivideStr[0])
            ds->lightSubdivide = atof(subdivideStr);

        const char *cutoffStr = ValueForEpair(epairs, "cutoff");
        if (cutoffStr[0])
            ds->cutoff = atof(cutoffStr);

        const char *fadeoutStr = ValueForEpair(epairs, "fadeout");
        if (fadeoutStr[0])
            ds->fadeout = atof(fadeoutStr);

        const char *nodeluxeStr = ValueForEpair(epairs, "nodeluxe");
        if (nodeluxeStr[0])
            ds->noDeluxeInfluence = atoi(nodeluxeStr);

        const char *bsNodeluxeStr = ValueForEpair(epairs, "backsplash_nodeluxe");
        if (bsNodeluxeStr[0])
            ds->noDeluxeInfluenceBacksplash = atoi(bsNodeluxeStr);

        const char *gridAmbientScaleStr = ValueForEpair(epairs, "grid_ambientscale");
        if (gridAmbientScaleStr[0])
            ds->gridAmbientScale = atof(gridAmbientScaleStr);

        const char *gridDirectScaleStr = ValueForEpair(epairs, "grid_directscale");
        if (gridDirectScaleStr[0])
            ds->gridDirectScale = atof(gridDirectScaleStr);

        const char *attStr = ValueForEpair(epairs, "attenuation");
        if (attStr[0])
        {
            ds->hasAttenuationOverride = qtrue;
            if (!Q_stricmp(attStr, "soft"))
                ds->attenuationModel = ATTENUATION_INVERSE;
            else if (!Q_stricmp(attStr, "linear"))
                ds->attenuationModel = ATTENUATION_LINEAR;
            else if (!Q_stricmp(attStr, "standard"))
                ds->attenuationModel = ATTENUATION_INVERSE_SQUARE;
            else if (!Q_stricmp(attStr, "unreal"))
                ds->attenuationModel = ATTENUATION_UNREAL;
            else if (!Q_stricmp(attStr, "smoothstep"))
                ds->attenuationModel = ATTENUATION_SMOOTHSTEP;
            else
            {
                _printf("WARNING: Unknown attenuation mode '%s'\n", attStr);
                ds->hasAttenuationOverride = qfalse;
            }
        }
    }
    // Resolve vertexcolor override (func_group, misc_model, etc)
    const char *vcolStr = ValueForEpair(epairs, "vertexcolor");
    if (vcolStr[0])
    {
        ds->overrideVertexColor = 1;
        ParseColor(vcolStr, ds->vertexColor);
    }

    // Resolve upscale
    const char *upscaleStr = ValueForEpair(epairs, "upscale");
    if (upscaleStr[0])
        ds->upscale = atoi(upscaleStr);

    // Resolve supersample
    const char *ssStr = ValueForEpair(epairs, "supersample");
    if (ssStr[0])
    {
        float ssVal = atof(ssStr);
        if (ssVal < 0.0f)
            ds->superSampleRadius = 0.0f;
        else
            ds->superSampleRadius = ssVal;
    }

    // Resolve enforceSampleSize override
    const char *enforceStr = ValueForEpair(epairs, "enforcesamplesize");
    if (enforceStr[0])
        ds->enforceSampleSize = atoi(enforceStr);

    // Resolve chamfer width overrides
    const char *chamferWStr = ValueForEpair(epairs, "chamfer_convexwidth");
    if (chamferWStr[0])
        ds->chamferConvexWidth = atof(chamferWStr);

    const char *chamferCStr = ValueForEpair(epairs, "chamfer_concavewidth");
    if (chamferCStr[0])
        ds->chamferConcaveWidth = atof(chamferCStr);
}

/*
=================
DrawSurfaceForSide
=================
*/
mapDrawSurface_t *DrawSurfaceForSide(bspbrush_t *b, side_t *s, winding_t *w)
{
    mapDrawSurface_t *ds;
    int i, j;
    shaderInfo_t *si;
    drawVert_t *dv;
    float mins[2], maxs[2];

    // brush primitive :
    // axis base
    vec3_t texX, texY;
    vec_t x, y;

    if (w->numpoints > 64)
    {
        Error("DrawSurfaceForSide: w->numpoints = %i", w->numpoints);
    }

    si = s->shaderInfo;

    ds = AllocDrawSurf();

    ds->shaderInfo = si;
    ds->mapBrush = b;
    ds->side = s;
    ds->fogNum = -1;

    memset(ds->decalgroup, 0, sizeof(ds->decalgroup));
    strncpy(ds->decalgroup, ValueForEpair(b->epairs, "decalgroup"), sizeof(ds->decalgroup) - 1);

    // Resolve sample size hierarchy
    if (si && si->lightmapSampleSize > 0)
    {
        ds->samplesize = si->lightmapSampleSize;
    }
    const char *ent_sample_str = ValueForEpair(b->epairs, "samplesize");
    if (!ent_sample_str[0])
        ent_sample_str = ValueForEpair(b->epairs, "lightmapsamplesize");
    
    if (ent_sample_str[0])
    {
        float ent_sample = atof(ent_sample_str);
        if (ent_sample > 0.0f)
            ds->samplesize = ent_sample;
    }

    // Resolve chamfer width shader hierarchy
    if (si && si->chamferConvexWidth >= 0.0f)
    {
        ds->chamferConvexWidth = si->chamferConvexWidth;
    }
    if (si && si->chamferConcaveWidth >= 0.0f)
    {
        ds->chamferConcaveWidth = si->chamferConcaveWidth;
    }

    // Fast mode: ignore requests for higher resolution than the compilation setting
    if (g_fast && ds->samplesize < game->defaultSampleSize)
    {
        ds->samplesize = game->defaultSampleSize;
    }
    
    // Apply maxSampleSize floor (planar: snap to next power of 2)
    // NOTE: samplesize clamping intentionally differs between planar brushes and misc_model.
    // Planar lightmaps rely on strict grid alignment to prevent seams between adjacent walls, 
    // requiring power-of-2 (or very clean integer) samplesizes. misc_model uses exact fractional 
    // values for more efficient arbitrary UV packing. Do not unify these routines.
    if (!g_fast && si && si->maxSampleSize > 0.0f && si->maxSampleSize < ds->samplesize)
    {
        int minPow2 = 1;
        while (minPow2 < (int)ceil(si->maxSampleSize))
            minPow2 <<= 1;
        if (minPow2 < ds->samplesize)
            ds->samplesize = (float)minPow2;
    }
    // Brushes strictly follow the Shader/Global hierarchy for samplesize.
    // Manual entity-level overrides are no longer supported.
    ds->lightmapScale = 1.0f;

    // Resolve sidecar properties from parent entity
    ResolveSurfaceExtraProperties(ds, b->epairs);

    ds->numVerts = w->numpoints;
    ds->verts = malloc(ds->numVerts * sizeof(*ds->verts));
    memset(ds->verts, 0, ds->numVerts * sizeof(*ds->verts));

    mins[0] = mins[1] = 99999;
    maxs[0] = maxs[1] = -99999;

    // compute s/t coordinates from brush primitive texture matrix
    // compute axis base
    ComputeAxisBase(mapplanes[s->planenum].normal, texX, texY);

    for (j = 0; j < w->numpoints; j++)
    {
        dv = ds->verts + j;

        // round the xyz to a given precision
        for (i = 0; i < 3; i++)
        {
            dv->xyz[i] =
                SNAP_INT_TO_FLOAT * floor(w->points[j][i] * SNAP_FLOAT_TO_INT + 0.5);
        }

        if (g_bBrushPrimit == BPRIMIT_OLDBRUSHES)
        {
            // calculate texture s/t
            dv->st[0] = s->vecs[0][3] + DotProduct(s->vecs[0], dv->xyz);
            dv->st[1] = s->vecs[1][3] + DotProduct(s->vecs[1], dv->xyz);
            dv->st[0] /= si->width;
            dv->st[1] /= si->height;
        }
        else
        {
            // calculate texture s/t from brush primitive texture matrix
            x = DotProduct(dv->xyz, texX);
            y = DotProduct(dv->xyz, texY);
            dv->st[0] = s->texMat[0][0] * x + s->texMat[0][1] * y + s->texMat[0][2];
            dv->st[1] = s->texMat[1][0] * x + s->texMat[1][1] * y + s->texMat[1][2];
        }

        for (i = 0; i < 2; i++)
        {
            if (dv->st[i] < mins[i])
            {
                mins[i] = dv->st[i];
            }
            if (dv->st[i] > maxs[i])
            {
                maxs[i] = dv->st[i];
            }
        }

        // copy normal
        VectorCopy(mapplanes[s->planenum].normal, dv->normal);
    }

    // adjust the texture coordinates to be as close to 0 as possible
    if (!si->globalTexture)
    {
        mins[0] = floor(mins[0]);
        mins[1] = floor(mins[1]);
        for (i = 0; i < w->numpoints; i++)
        {
            dv = ds->verts + i;
            dv->st[0] -= mins[0];
            dv->st[1] -= mins[1];
        }
    }

    return ds;
}

//=========================================================================

typedef struct
{
    int planenum;
    shaderInfo_t *shaderInfo;
    int count;
} sideRef_t;

sideRef_t *sideRefs;
int numSideRefs;

void AddSideRef(side_t *side)
{
    int i;

    if (!sideRefs) {
        sideRefs = malloc(game->maxMapPlanes * sizeof(sideRef_t));
    }

    for (i = 0; i < numSideRefs; i++)
    {
        if (side->planenum == sideRefs[i].planenum &&
            side->shaderInfo == sideRefs[i].shaderInfo)
        {
            sideRefs[i].count++;
            return;
        }
    }

    if (numSideRefs == game->maxMapPlanes)
    {
        Error("game->maxMapPlanes");
    }

    sideRefs[i].planenum = side->planenum;
    sideRefs[i].shaderInfo = side->shaderInfo;
    sideRefs[i].count++;
    numSideRefs++;
}

/*
==================
WindingsShareEdge
==================
*/
static qboolean WindingsShareEdge(winding_t *w1, winding_t *w2)
{
    int i, j;

    for (i = 0; i < w1->numpoints; i++)
    {
        int i2 = (i + 1) % w1->numpoints;
        for (j = 0; j < w2->numpoints; j++)
        {
            int j2 = (j + 1) % w2->numpoints;
            if (VectorCompare(w1->points[i], w2->points[j2]) &&
                VectorCompare(w1->points[i2], w2->points[j]))
            {
                return qtrue;
            }
        }
    }
    return qfalse;
}

/*
==================
MergeDrawSurfs
==================
*/
void MergeDrawSurfs(entity_t *e)
{
    int i, j;
    mapDrawSurface_t *ds1, *ds2;
    winding_t *w1, *w2, *hull;
    float limit;
    shaderInfo_t *si;
    int mergedCount = 0;
    qboolean changed;
    int numBaseDrawSurfs;

    qprintf("----- MergeDrawSurfs -----\n");

    do
    {
        changed = qfalse;
        numBaseDrawSurfs = numMapDrawSurfs;
        for (i = e->firstDrawSurf; i < numBaseDrawSurfs; i++)
        {
            ds1 = &mapDrawSurfs[i];
            if (ds1->numVerts <= 0 || !ds1->side || ds1->patch || ds1->miscModel ||
                ds1->flareSurface)
            {
                continue;
            }
            si = ds1->side->shaderInfo;
            if (!si || si->subdivisions > 0)
            {
                continue;
            }
            if (!ds1->enforceSampleSize || ds1->samplesize <= 0)
            {
                continue;
            }
            if (si->surfaceFlags & (SURF_NOLIGHTMAP | SURF_POINTLIGHT))
            {
                continue;
            }
            if (si->contents &
                (CONTENTS_FOG | CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA))
            {
                continue;
            }

            float effectiveSS = SnapToNearestPowerOfTwo(ds1->samplesize);
            limit = (float)(LIGHTMAP_WIDTH - 3) * effectiveSS;

            for (j = i + 1; j < numBaseDrawSurfs; j++)
            {
                ds2 = &mapDrawSurfs[j];
                if (ds2->numVerts <= 0 || !ds2->side || ds2->patch || ds2->miscModel ||
                    ds2->flareSurface)
                {
                    continue;
                }

                // Must be coplanar
                if (ds1->side->planenum != ds2->side->planenum)
                {
                    continue;
                }

                // Must have same shader and samplesize
                if (ds1->shaderInfo != ds2->shaderInfo ||
                    ds1->samplesize != ds2->samplesize)
                {
                    continue;
                }

                w1 = WindingFromDrawSurf(ds1);
                w2 = WindingFromDrawSurf(ds2);

                if (!w1 || !w2 || !WindingsShareEdge(w1, w2))
                {
                    if (w1)
                        FreeWinding(w1);
                    if (w2)
                        FreeWinding(w2);
                    continue;
                }

                // Check if combined hull fits the limit
                hull = NULL;
                AddWindingToConvexHull(w1, &hull,
                                       mapplanes[ds1->side->planenum].normal);
                AddWindingToConvexHull(w2, &hull,
                                       mapplanes[ds1->side->planenum].normal);

                vec3_t mins, maxs;
                WindingBounds(hull, mins, maxs);

                // Determine lightmap projection axes
                plane_t *plane = &mapplanes[ds1->side->planenum];
                vec3_t planeNormal;
                planeNormal[0] = fabs(plane->normal[0]);
                planeNormal[1] = fabs(plane->normal[1]);
                planeNormal[2] = fabs(plane->normal[2]);

                qboolean fits = qtrue;
                if (planeNormal[0] >= planeNormal[1] &&
                    planeNormal[0] >= planeNormal[2])
                {
                    if (maxs[1] - mins[1] > limit || maxs[2] - mins[2] > limit)
                        fits = qfalse;
                }
                else if (planeNormal[1] >= planeNormal[0] &&
                         planeNormal[1] >= planeNormal[2])
                {
                    if (maxs[0] - mins[0] > limit || maxs[2] - mins[2] > limit)
                        fits = qfalse;
                }
                else
                {
                    if (maxs[0] - mins[0] > limit || maxs[1] - mins[1] > limit)
                        fits = qfalse;
                }

                if (fits)
                {
                    // Check convexity/area to avoid merging concave setups into big boxes
                    float area1 = WindingArea(w1);
                    float area2 = WindingArea(w2);
                    float hullArea = WindingArea(hull);

                    if (hullArea < (area1 + area2) + 0.1)
                    {
                        // MERGE!
                        mapDrawSurface_t *tempds =
                            DrawSurfaceForSide(ds1->mapBrush, ds1->side, hull);

                        // Copy the newly generated surface content to ds1
                        free(ds1->verts);
                        ds1->numVerts = tempds->numVerts;
                        ds1->verts = tempds->verts;
                        tempds->numVerts = 0; // mark the new one as empty

                        ds2->numVerts = 0; // mark the second one as merged
                        mergedCount++;
                        changed = qtrue;

                        FreeWinding(w1);
                        FreeWinding(w2);
                        FreeWinding(hull);
                        break; // restart inner loop because ds1 changed
                    }
                }

                FreeWinding(w1);
                FreeWinding(w2);
                if (hull)
                    FreeWinding(hull);
            }
            if (changed)
                break;
        }
    } while (changed);

    if (mergedCount > 0)
    {
        _printf("%5i surfaces merged\n", mergedCount);
    }
}

/*
=====================
MergeSides

=====================
*/
void MergeSides(entity_t *e, tree_t *tree)
{
    MergeDrawSurfs(e);
}

//=====================================================================

/*
===================
SubdivideDrawSurf
===================
*/
static void SubdivideDrawSurf_r(mapDrawSurface_t *ds, winding_t *w, float subdivisions, int axisMask, qboolean forceGrid)
{
    int i;
    int axis;
    vec3_t bounds[2];
    const float epsilon = 0.1;
    int subFloor, subCeil;
    winding_t *frontWinding, *backWinding;
    mapDrawSurface_t *newds;

    if (!w)
    {
        return;
    }
    if (w->numpoints < 3)
    {
        Error("SubdivideDrawSurf: Bad w->numpoints");
    }

    ClearBounds(bounds[0], bounds[1]);
    for (i = 0; i < w->numpoints; i++)
    {
        AddPointToBounds(w->points[i], bounds[0], bounds[1]);
    }

    for (axis = 0; axis < 3; axis++)
    {
        if (!(axisMask & (1 << axis)))
        {
            continue;
        }

        vec3_t planePoint = {0, 0, 0};
        vec3_t planeNormal = {0, 0, 0};
        float d;

        subFloor = floor(bounds[0][axis] / subdivisions) * subdivisions;
        subCeil = ceil(bounds[1][axis] / subdivisions) * subdivisions;

        planePoint[axis] = subFloor + subdivisions;
        planeNormal[axis] = -1;

        d = DotProduct(planePoint, planeNormal);

        // subdivide if necessary
        float size = bounds[1][axis] - bounds[0][axis];
        if (size > subdivisions || (forceGrid && subCeil - subFloor > subdivisions))
        {
            // gotta clip polygon into two polygons
            ClipWindingEpsilon(w, planeNormal, d, epsilon, &frontWinding,
                               &backWinding);

            // the clip may not produce two polygons if it was epsilon close
            if (!frontWinding)
            {
                w = backWinding;
            }
            else if (!backWinding)
            {
                w = frontWinding;
            }
            else
            {
                SubdivideDrawSurf_r(ds, frontWinding, subdivisions, axisMask, forceGrid);
                SubdivideDrawSurf_r(ds, backWinding, subdivisions, axisMask, forceGrid);

                return;
            }
        }
    }

    // emit this polygon
    newds = DrawSurfaceForSide(ds->mapBrush, ds->side, w);
    newds->fogNum = ds->fogNum;
    newds->parentSurfaceNum = (ds->parentSurfaceNum != -1) ? ds->parentSurfaceNum : (int)(ds - mapDrawSurfs);
}

void SubdivideDrawSurf(mapDrawSurface_t *ds, winding_t *w, float subdivisions)
{
    SubdivideDrawSurf_r(ds, w, subdivisions, 7, qtrue);
}

/*
=====================
SubdivideDrawSurfs

Chop up surfaces that have subdivision attributes
=====================
*/
void SubdivideDrawSurfs(entity_t *e, tree_t *tree)
{
    int i;
    mapDrawSurface_t *ds;
    int numBaseDrawSurfs;
    winding_t *w;
    shaderInfo_t *si;

    qprintf("----- SubdivideDrawSurfs -----\n");
    numBaseDrawSurfs = numMapDrawSurfs;
    for (i = e->firstDrawSurf; i < numBaseDrawSurfs; i++)
    {
        ds = &mapDrawSurfs[i];

        // Candidate surfaces MUST be MST_PLANAR (only brush sides have ds->side set)
        // We also exclude patches, misc_models and flares (though ds->side handles most of this)
        if (!ds->side || ds->patch || ds->miscModel || ds->flareSurface)
        {
            continue;
        }

        // check subdivision for shader
        si = ds->side->shaderInfo;
        if (!si)
        {
            continue;
        }

        if (ds->shaderInfo->autosprite || si->autosprite)
        {
            continue;
        }

        // 1. If shader has tessSize (subdivisions > 0), it takes precedence as per requirements
        if (si->subdivisions > 0)
        {
            w = WindingFromDrawSurf(ds);
            ds->numVerts = 0; // remove this reference
            SubdivideDrawSurf(ds, w, si->subdivisions);
            continue;
        }

        // 2. Lightmap consistency subdivision (enforceSampleSize)
        if (ds->enforceSampleSize && ds->samplesize > 0)
        {
            // Filter out surfaces that don't receive lightmaps
            if (si->surfaceFlags & (SURF_NOLIGHTMAP | SURF_POINTLIGHT))
            {
                continue;
            }

            // Filter out liquid and fog contents
            if (si->contents &
                (CONTENTS_FOG | CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA))
            {
                continue;
            }

            // Calculate the maximum physical size a surface can be before its lightmap
            // exceeds the available atlas block size (causing stretching).
            // We subtract 3: 2 for the padding gutter, and 1 because
            // AllocateLightmapForSurface adds 1 to the bounds difference when
            // calculating texel size.
            float effectiveSS = SnapToNearestPowerOfTwo(ds->samplesize);
            float maxLightmapSubdiv = (float)(LIGHTMAP_WIDTH - 3) * effectiveSS;

            // Determine lightmap projection axes (logic mirrors
            // AllocateLightmapForSurface)
            plane_t *plane = &mapplanes[ds->side->planenum];
            vec3_t planeNormal;
            planeNormal[0] = fabs(plane->normal[0]);
            planeNormal[1] = fabs(plane->normal[1]);
            planeNormal[2] = fabs(plane->normal[2]);

            int axisMask = 0;
            if (planeNormal[0] >= planeNormal[1] &&
                planeNormal[0] >= planeNormal[2])
            {
                axisMask = (1 << 1) | (1 << 2); // Y and Z axes
            }
            else if (planeNormal[1] >= planeNormal[0] &&
                     planeNormal[1] >= planeNormal[2])
            {
                axisMask = (1 << 0) | (1 << 2); // X and Z axes
            }
            else
            {
                axisMask = (1 << 0) | (1 << 1); // X and Y axes
            }

            w = WindingFromDrawSurf(ds);
            ds->numVerts = 0; // remove this reference
            SubdivideDrawSurf_r(ds, w, maxLightmapSubdiv, axisMask, qfalse);
        }
    }
}

//===================================================================================

/*
====================
ClipSideIntoTree_r

Adds non-opaque leaf fragments to the convex hull
====================
*/
void ClipSideIntoTree_r(winding_t *w, side_t *side, node_t *node)
{
    plane_t *plane;
    winding_t *front, *back;

    if (!w)
    {
        return;
    }

    if (node->planenum != PLANENUM_LEAF)
    {
        if (side->planenum == node->planenum)
        {
            ClipSideIntoTree_r(w, side, node->children[0]);
            return;
        }
        if (side->planenum == (node->planenum ^ 1))
        {
            ClipSideIntoTree_r(w, side, node->children[1]);
            return;
        }

        plane = &mapplanes[node->planenum];
        ClipWindingEpsilon(w, plane->normal, plane->dist, ON_EPSILON, &front,
                           &back);
        FreeWinding(w);

        ClipSideIntoTree_r(front, side, node->children[0]);
        ClipSideIntoTree_r(back, side, node->children[1]);

        return;
    }

    // if opaque leaf, don't add
    if (!node->opaque)
    {
        AddWindingToConvexHull(w, &side->visibleHull,
                               mapplanes[side->planenum].normal);
    }

    FreeWinding(w);
    return;
}

/*
=====================
ClipSidesIntoTree

Creates side->visibleHull for all visible sides

The drawsurf for a side will consist of the convex hull of
all points in non-opaque clusters, which allows overlaps
to be trimmed off automatically.
=====================
*/
void ClipSidesIntoTree(entity_t *e, tree_t *tree)
{
    bspbrush_t *b;
    int i;
    winding_t *w;
    side_t *side, *newSide;
    shaderInfo_t *si;

    qprintf("----- ClipSidesIntoTree -----\n");

    for (b = e->brushes; b; b = b->next)
    {
        for (i = 0; i < b->numsides; i++)
        {
            side = &b->sides[i];
            if (!side->winding)
            {
                continue;
            }
            w = CopyWinding(side->winding);
            side->visibleHull = NULL;
            ClipSideIntoTree_r(w, side, tree->headnode);

            w = side->visibleHull;
            if (!w)
            {
                continue;
            }
            si = side->shaderInfo;
            if (!si)
            {
                continue;
            }
            // don't create faces for non-visible sides
            if ((si->surfaceFlags & SURF_NODRAW) || (side->surfaceFlags & SURF_NODRAW))
            {
                continue;
            }

            // always use the original quad winding for auto sprites
            if (side->shaderInfo->autosprite)
            {
                w = side->winding;
            }
            //
            if (side->bevel)
            {
                Error("monkey tried to create draw surface for brush bevel");
            }
            // save this winding as a visible surface
            DrawSurfaceForSide(b, side, w);

            // make a back side for it if needed
            if (!(si->contents & CONTENTS_FOG))
            {
                continue;
            }

            // duplicate the up-facing side
            w = ReverseWinding(w);

            newSide = malloc(sizeof(*side));
            *newSide = *side;
            newSide->visibleHull = w;
            newSide->planenum ^= 1;

            // save this winding as a visible surface
            DrawSurfaceForSide(b, newSide, w);
        }
    }
}

/*
===================================================================================

  FILTER REFERENCES DOWN THE TREE

===================================================================================
*/

/*
====================
FilterDrawSurfIntoTree

Place a reference to the given drawsurf in every leaf it contacts
We assume that the point mesh aproximation to the curve will get a
reference into all the leafs we need.
====================
*/
int FilterMapDrawSurfIntoTree(vec3_t point, mapDrawSurface_t *ds,
                              node_t *node)
{
    drawSurfRef_t *dsr;
    float d;
    plane_t *plane;
    int c;

    if (node->planenum != PLANENUM_LEAF)
    {
        plane = &mapplanes[node->planenum];
        d = DotProduct(point, plane->normal) - plane->dist;
        c = 0;
        if (d >= -ON_EPSILON)
        {
            c += FilterMapDrawSurfIntoTree(point, ds, node->children[0]);
        }
        if (d <= ON_EPSILON)
        {
            c += FilterMapDrawSurfIntoTree(point, ds, node->children[1]);
        }
        return c;
    }

    // if opaque leaf, don't add
    if (node->opaque)
    {
        return 0;
    }

    // add the drawsurf if it hasn't been already
    for (dsr = node->drawSurfReferences; dsr; dsr = dsr->nextRef)
    {
        if (dsr->outputNumber == numDrawSurfaces)
        {
            return 0; // already referenced
        }
    }

    dsr = malloc(sizeof(*dsr));
    dsr->outputNumber = numDrawSurfaces;
    dsr->nextRef = node->drawSurfReferences;
    node->drawSurfReferences = dsr;
    return 1;
}

/*
====================
FilterDrawSurfIntoTree_r

Place a reference to the given drawsurf in every leaf it is in
====================
*/
int FilterMapDrawSurfIntoTree_r(winding_t *w, mapDrawSurface_t *ds,
                                node_t *node)
{
    drawSurfRef_t *dsr;
    plane_t *plane;
    int total;
    winding_t *front, *back;

    if (node->planenum != PLANENUM_LEAF)
    {
        plane = &mapplanes[node->planenum];
        ClipWindingEpsilon(w, plane->normal, plane->dist, ON_EPSILON, &front,
                           &back);

        total = 0;
        if (front)
        {
            total += FilterMapDrawSurfIntoTree_r(front, ds, node->children[0]);
        }
        if (back)
        {
            total += FilterMapDrawSurfIntoTree_r(back, ds, node->children[1]);
        }

        FreeWinding(w);
        return total;
    }

    // if opaque leaf, don't add
    if (node->opaque)
    {
        return 0;
    }

    // add the drawsurf if it hasn't been already
    for (dsr = node->drawSurfReferences; dsr; dsr = dsr->nextRef)
    {
        if (dsr->outputNumber == numDrawSurfaces)
        {
            return 0; // already referenced
        }
    }

    dsr = malloc(sizeof(*dsr));
    dsr->outputNumber = numDrawSurfaces;
    dsr->nextRef = node->drawSurfReferences;
    node->drawSurfReferences = dsr;
    return 1;
}

/*
====================
FilterSideIntoTree_r

Place a reference to the given drawsurf in every leaf it contacts
====================
*/
int FilterSideIntoTree_r(winding_t *w, side_t *side, mapDrawSurface_t *ds,
                         node_t *node)
{
    drawSurfRef_t *dsr;
    plane_t *plane;
    winding_t *front, *back;
    int total;

    if (!w)
    {
        return 0;
    }

    if (node->planenum != PLANENUM_LEAF)
    {
        if (side->planenum == node->planenum)
        {
            return FilterSideIntoTree_r(w, side, ds, node->children[0]);
        }
        if (side->planenum == (node->planenum ^ 1))
        {
            return FilterSideIntoTree_r(w, side, ds, node->children[1]);
        }

        plane = &mapplanes[node->planenum];
        ClipWindingEpsilon(w, plane->normal, plane->dist, ON_EPSILON, &front,
                           &back);

        total = FilterSideIntoTree_r(front, side, ds, node->children[0]);
        total += FilterSideIntoTree_r(back, side, ds, node->children[1]);

        FreeWinding(w);
        return total;
    }

    // if opaque leaf, don't add
    if (node->opaque)
    {
        return 0;
    }

    dsr = malloc(sizeof(*dsr));
    dsr->outputNumber = numDrawSurfaces;
    dsr->nextRef = node->drawSurfReferences;
    node->drawSurfReferences = dsr;

    FreeWinding(w);
    return 1;
}

/*
=====================
FilterFaceIntoTree
=====================
*/
int FilterFaceIntoTree(mapDrawSurface_t *ds, tree_t *tree)
{
    int l;
    winding_t *w;

    w = WindingFromDrawSurf(ds);
    l = FilterSideIntoTree_r(w, ds->side, ds, tree->headnode);

    return l;
}

/*
=====================
FilterPatchSurfIntoTree
=====================
*/
#define SUBDIVISION_LIMIT 8.0
int FilterPatchSurfIntoTree(mapDrawSurface_t *ds, tree_t *tree)
{
    int i, j;
    int l;
    mesh_t baseMesh, *subdividedMesh;
    winding_t *w;

    baseMesh.width = ds->patchWidth;
    baseMesh.height = ds->patchHeight;
    baseMesh.verts = ds->verts;
    subdividedMesh = SubdivideMesh(baseMesh, SUBDIVISION_LIMIT, 32);

    l = 0;
    for (i = 0; i < subdividedMesh->width - 1; i++)
    {
        for (j = 0; j < subdividedMesh->height - 1; j++)
        {
            w = AllocWinding(3);
            VectorCopy(subdividedMesh->verts[j * subdividedMesh->width + i].xyz,
                       w->points[0]);
            VectorCopy(subdividedMesh->verts[j * subdividedMesh->width + i + 1].xyz,
                       w->points[1]);
            VectorCopy(subdividedMesh->verts[(j + 1) * subdividedMesh->width + i].xyz,
                       w->points[2]);
            w->numpoints = 3;
            l += FilterMapDrawSurfIntoTree_r(w, ds, tree->headnode);
            w = AllocWinding(3);
            VectorCopy(subdividedMesh->verts[j * subdividedMesh->width + i + 1].xyz,
                       w->points[0]);
            VectorCopy(
                subdividedMesh->verts[(j + 1) * subdividedMesh->width + i + 1].xyz,
                w->points[1]);
            VectorCopy(subdividedMesh->verts[(j + 1) * subdividedMesh->width + i].xyz,
                       w->points[2]);
            w->numpoints = 3;
            l += FilterMapDrawSurfIntoTree_r(w, ds, tree->headnode);
        }
    }

    // also use the old point filtering into the tree
    for (i = 0; i < subdividedMesh->width * subdividedMesh->height; i++)
    {
        l += FilterMapDrawSurfIntoTree(subdividedMesh->verts[i].xyz, ds,
                                       tree->headnode);
    }

    free(subdividedMesh);

    return l;
}

/*
=====================
FilterMiscModelSurfIntoTree
=====================
*/
int FilterMiscModelSurfIntoTree(mapDrawSurface_t *ds, tree_t *tree)
{
    int i;
    int l;
    winding_t *w;

    l = 0;
    for (i = 0; i < ds->numIndexes - 2; i += 3)
    {
        w = AllocWinding(3);
        VectorCopy(ds->verts[ds->indexes[i]].xyz, w->points[0]);
        VectorCopy(ds->verts[ds->indexes[i + 1]].xyz, w->points[1]);
        VectorCopy(ds->verts[ds->indexes[i + 2]].xyz, w->points[2]);
        w->numpoints = 3;
        l += FilterMapDrawSurfIntoTree_r(w, ds, tree->headnode);
    }

    // also use the old point filtering into the tree
    for (i = 0; i < ds->numVerts; i++)
    {
        l += FilterMapDrawSurfIntoTree(ds->verts[i].xyz, ds, tree->headnode);
    }

    return l;
}

/*
=====================
FilterFlareSurfIntoTree
=====================
*/
int FilterFlareSurfIntoTree(mapDrawSurface_t *ds, tree_t *tree)
{
    return FilterMapDrawSurfIntoTree(ds->lightmapOrigin, ds, tree->headnode);
}

//======================================================================

int c_stripSurfaces, c_fanSurfaces;

/*
==================
IsTriangleDegenerate

Returns qtrue if all three points are collinear or backwards
===================
*/
#define COLINEAR_AREA 10
static qboolean IsTriangleDegenerate(drawVert_t *points, int a, int b, int c)
{
    vec3_t v1, v2, v3;
    float d;

    VectorSubtract(points[b].xyz, points[a].xyz, v1);
    VectorSubtract(points[c].xyz, points[a].xyz, v2);
    CrossProduct(v1, v2, v3);
    d = VectorLength(v3);

    // assume all very small or backwards triangles will cause problems
    if (d < COLINEAR_AREA)
    {
        return qtrue;
    }

    return qfalse;
}

/*
===============
SurfaceAsTriFan

The surface can't be represented as a single tristrip without
leaving a degenerate triangle (and therefore a crack), so add
a point in the middle and create (points-1) triangles in fan order
===============
*/
static void SurfaceAsTriFan(dsurface_t *ds)
{
    int i;
    int colorSum[4];
    drawVert_t *mid, *v;

    // create a new point in the center of the face
    if (numDrawVerts >= game->maxMapDrawVerts)
    {
        _printf("\n--- VERTEX LIMIT EXCEEDED ---\n");
        _printf("Surface Shader: %s\n", dshaders[ds->shaderNum].shader);
        _printf("Total vertices so far: %i\n", numDrawVerts);
        _printf("Current surface vertices: %i\n", ds->numVerts);
        _printf("Limit for current game profile: %i\n", game->maxMapDrawVerts);
        _printf("Advice: High-poly maps require '-game qfusion' during compilation.\n");
        _printf("-----------------------------\n");
        Error("game->maxMapDrawVerts");
    }
    mid = &drawVerts[numDrawVerts];
    numDrawVerts++;

    colorSum[0] = colorSum[1] = colorSum[2] = colorSum[3] = 0;

    v = drawVerts + ds->firstVert;
    for (i = 0; i < ds->numVerts; i++, v++)
    {
        VectorAdd(mid->xyz, v->xyz, mid->xyz);
        mid->st[0] += v->st[0];
        mid->st[1] += v->st[1];
        mid->lightmap[0][0] += v->lightmap[0][0];
        mid->lightmap[0][1] += v->lightmap[0][1];

        colorSum[0] += v->color[0][0];
        colorSum[1] += v->color[0][1];
        colorSum[2] += v->color[0][2];
        colorSum[3] += v->color[0][3];
    }

    mid->xyz[0] /= ds->numVerts;
    mid->xyz[1] /= ds->numVerts;
    mid->xyz[2] /= ds->numVerts;

    mid->st[0] /= ds->numVerts;
    mid->st[1] /= ds->numVerts;

    mid->lightmap[0][0] /= ds->numVerts;
    mid->lightmap[0][1] /= ds->numVerts;

    mid->color[0][0] = colorSum[0] / ds->numVerts;
    mid->color[0][1] = colorSum[1] / ds->numVerts;
    mid->color[0][2] = colorSum[2] / ds->numVerts;
    mid->color[0][3] = colorSum[3] / ds->numVerts;

    VectorCopy((drawVerts + ds->firstVert)->normal, mid->normal);

    // fill in indices in trifan order
    if (numDrawIndexes + ds->numVerts * 3 > game->maxMapDrawIndexes)
    {
        Error("MAX_MAP_DRAW_INDEXES (%d)", game->maxMapDrawIndexes);
    }
    ds->firstIndex = numDrawIndexes;
    ds->numIndexes = ds->numVerts * 3;

    // FIXME
    //  should be: for ( i = 0 ; i < ds->numVerts ; i++ ) {
    //  set a break point and test this in a map
    // for ( i = 0 ; i < ds->numVerts*3 ; i++ ) {
    for (i = 0; i < ds->numVerts; i++)
    {
        drawIndexes[numDrawIndexes++] = ds->numVerts;
        drawIndexes[numDrawIndexes++] = i;
        drawIndexes[numDrawIndexes++] = (i + 1) % ds->numVerts;
    }

    ds->numVerts++;
}

/*
================
SurfaceAsTristrip

Try to create indices that make (points-2) triangles in tristrip order
================
*/
#define MAX_INDICES 1024
static void SurfaceAsTristrip(dsurface_t *ds)
{
    int i;
    int rotate;
    int numIndices;
    int ni = 0;
    int a, b, c;
    int indices[MAX_INDICES];

    // determine the triangle strip order
    numIndices = (ds->numVerts - 2) * 3;
    if (numIndices > MAX_INDICES)
    {
        Error("MAX_INDICES exceeded for surface");
    }

    // try all possible orderings of the points looking
    // for a strip order that isn't degenerate
    for (rotate = 0; rotate < ds->numVerts; rotate++)
    {
        for (ni = 0, i = 0; i < ds->numVerts - 2 - i; i++)
        {
            a = (ds->numVerts - 1 - i + rotate) % ds->numVerts;
            b = (i + rotate) % ds->numVerts;
            c = (ds->numVerts - 2 - i + rotate) % ds->numVerts;

            if (IsTriangleDegenerate(drawVerts + ds->firstVert, a, b, c))
            {
                break;
            }
            indices[ni++] = a;
            indices[ni++] = b;
            indices[ni++] = c;

            if (i + 1 != ds->numVerts - 1 - i)
            {
                a = (ds->numVerts - 2 - i + rotate) % ds->numVerts;
                b = (i + rotate) % ds->numVerts;
                c = (i + 1 + rotate) % ds->numVerts;

                if (IsTriangleDegenerate(drawVerts + ds->firstVert, a, b, c))
                {
                    break;
                }
                indices[ni++] = a;
                indices[ni++] = b;
                indices[ni++] = c;
            }
        }
        if (ni == numIndices)
        {
            break; // got it done without degenerate triangles
        }
    }

    // if any triangle in the strip is degenerate,
    // render from a centered fan point instead
    if (ni < numIndices)
    {
        c_fanSurfaces++;
        SurfaceAsTriFan(ds);
        return;
    }

    // a normal tristrip
    c_stripSurfaces++;

    if (numDrawIndexes + ni > game->maxMapDrawIndexes)
    {
        Error("MAX_MAP_DRAW_INDEXES (%d)", game->maxMapDrawIndexes);
    }
    ds->firstIndex = numDrawIndexes;
    ds->numIndexes = ni;

    memcpy(drawIndexes + numDrawIndexes, indices, ni * sizeof(int));
    numDrawIndexes += ni;
}

/*
===============
EmitPlanarSurf
===============
*/
void EmitPlanarSurf(mapDrawSurface_t *ds)
{
    int j;
    dsurface_t *out;
    drawVert_t *outv;

    if (numDrawSurfaces >= game->maxMapDrawSurfs)
    {
        Error("game->maxMapDrawSurfs");
    }
    out = &drawSurfaces[numDrawSurfaces];
    if (ds->side)
    {
        ds->side->surfaceNum = numDrawSurfaces;
    }
    
    drawExtraSurfaces[numDrawSurfaces].smoothingRadius = ds->smoothingRadius;
    drawExtraSurfaces[numDrawSurfaces].lightValue = ds->lightValue;
    VectorCopy(ds->lightColor, drawExtraSurfaces[numDrawSurfaces].lightColor);
    drawExtraSurfaces[numDrawSurfaces].backsplashFraction = ds->backsplashFraction;
    drawExtraSurfaces[numDrawSurfaces].lightSubdivide = ds->lightSubdivide;
    drawExtraSurfaces[numDrawSurfaces].overrideVertexColor = ds->overrideVertexColor;
    VectorCopy(ds->vertexColor, drawExtraSurfaces[numDrawSurfaces].vertexColor);
    drawExtraSurfaces[numDrawSurfaces].superSampleRadius = ds->superSampleRadius;
    drawExtraSurfaces[numDrawSurfaces].isHalo = ds->isHalo;
    drawExtraSurfaces[numDrawSurfaces].upscale = ds->upscale;
    drawExtraSurfaces[numDrawSurfaces].cutoff = ds->cutoff;
    drawExtraSurfaces[numDrawSurfaces].fadeout = ds->fadeout;
    drawExtraSurfaces[numDrawSurfaces].hasAttenuationOverride = ds->hasAttenuationOverride;
    drawExtraSurfaces[numDrawSurfaces].attenuationModel = ds->attenuationModel;
    drawExtraSurfaces[numDrawSurfaces].noDeluxeInfluence = ds->noDeluxeInfluence;
    drawExtraSurfaces[numDrawSurfaces].noDeluxeInfluenceBacksplash = ds->noDeluxeInfluenceBacksplash;
    drawExtraSurfaces[numDrawSurfaces].gridAmbientScale = ds->gridAmbientScale;
    drawExtraSurfaces[numDrawSurfaces].gridDirectScale = ds->gridDirectScale;
    drawExtraSurfaces[numDrawSurfaces].castShadows = ds->castShadows;
    drawExtraSurfaces[numDrawSurfaces].isPlanar = ds->isPlanar;
    drawExtraSurfaces[numDrawSurfaces].sampleSize = ds->samplesize;

    numDrawSurfaces++;

    out->surfaceType = MST_PLANAR;
    out->shaderNum = EmitShader(ds->shaderInfo->shader);
    out->firstVert = numDrawVerts;
    out->numVerts = ds->numVerts;
    out->fogNum = ds->fogNum;
    out->lightmapNum[0] = ds->lightmapNum;
    out->lightmapOffset[0][0] = ds->lightmapX;
    out->lightmapOffset[0][1] = ds->lightmapY;
    out->lightmapWidth = ds->lightmapWidth;
    out->lightmapHeight = ds->lightmapHeight;
    // FBSP: initialize styles and auxiliary layers
    out->lightmapStyles[0] = 0; // LS_NORMAL
    out->vertexStyles[0] = 0;
    for (j = 1; j < 4; j++)
    {
        out->lightmapNum[j] = -1;
        out->lightmapOffset[j][0] = 0;
        out->lightmapOffset[j][1] = 0;
        out->lightmapStyles[j] = 0xFF; // LS_NONE
        out->vertexStyles[j] = 0xFF;
    }

    VectorCopy(ds->lightmapOrigin, out->lightmapOrigin);
    VectorCopy(ds->lightmapVecs[0], out->lightmapVecs[0]);
    VectorCopy(ds->lightmapVecs[1], out->lightmapVecs[1]);
    VectorCopy(ds->lightmapVecs[2], out->lightmapVecs[2]);

    for (j = 0; j < ds->numVerts; j++)
    {
        if (numDrawVerts >= game->maxMapDrawVerts)
        {
            _printf("\n--- VERTEX LIMIT EXCEEDED ---\n");
            _printf("Surface: %s\n", ds->shaderInfo->shader);
            _printf("Total vertices so far: %i\n", numDrawVerts);
            _printf("Current surface vertices: %i\n", ds->numVerts);
            _printf("Limit for current game profile: %i\n", game->maxMapDrawVerts);
            _printf("Advice: High-poly maps require '-game qfusion' during compilation.\n");
            _printf("-----------------------------\n");
            Error("game->maxMapDrawVerts");
        }
        outv = &drawVerts[numDrawVerts];
        numDrawVerts++;
        memcpy(outv, &ds->verts[j], sizeof(*outv));
        if (!ds->overrideVertexColor)
        {
            outv->color[0][0] = 255;
            outv->color[0][1] = 255;
            outv->color[0][2] = 255;
            outv->color[0][3] = 255;
        }
    }

    // create the indexes
    SurfaceAsTristrip(out);
}

/*
===============
EmitPatchSurf
===============
*/
void EmitPatchSurf(mapDrawSurface_t *ds)
{
    int j;
    dsurface_t *out;
    drawVert_t *outv;

    if (numDrawSurfaces >= game->maxMapDrawSurfs)
    {
        Error("game->maxMapDrawSurfs");
    }
    out = &drawSurfaces[numDrawSurfaces];

    drawExtraSurfaces[numDrawSurfaces].smoothingRadius = ds->smoothingRadius;
    drawExtraSurfaces[numDrawSurfaces].lightValue = ds->lightValue;
    VectorCopy(ds->lightColor, drawExtraSurfaces[numDrawSurfaces].lightColor);
    drawExtraSurfaces[numDrawSurfaces].backsplashFraction = ds->backsplashFraction;
    drawExtraSurfaces[numDrawSurfaces].lightSubdivide = ds->lightSubdivide;
    drawExtraSurfaces[numDrawSurfaces].overrideVertexColor = ds->overrideVertexColor;
    VectorCopy(ds->vertexColor, drawExtraSurfaces[numDrawSurfaces].vertexColor);
    drawExtraSurfaces[numDrawSurfaces].superSampleRadius = ds->superSampleRadius;
    drawExtraSurfaces[numDrawSurfaces].isHalo = ds->isHalo;
    drawExtraSurfaces[numDrawSurfaces].upscale = ds->upscale;
    drawExtraSurfaces[numDrawSurfaces].cutoff = ds->cutoff;
    drawExtraSurfaces[numDrawSurfaces].fadeout = ds->fadeout;
    drawExtraSurfaces[numDrawSurfaces].hasAttenuationOverride = ds->hasAttenuationOverride;
    drawExtraSurfaces[numDrawSurfaces].attenuationModel = ds->attenuationModel;
    drawExtraSurfaces[numDrawSurfaces].noDeluxeInfluence = ds->noDeluxeInfluence;
    drawExtraSurfaces[numDrawSurfaces].noDeluxeInfluenceBacksplash = ds->noDeluxeInfluenceBacksplash;
    drawExtraSurfaces[numDrawSurfaces].gridAmbientScale = ds->gridAmbientScale;
    drawExtraSurfaces[numDrawSurfaces].gridDirectScale = ds->gridDirectScale;
    drawExtraSurfaces[numDrawSurfaces].castShadows = ds->castShadows;
    drawExtraSurfaces[numDrawSurfaces].isPlanar = ds->isPlanar;
    drawExtraSurfaces[numDrawSurfaces].sampleSize = ds->samplesize;

    numDrawSurfaces++;

    out->surfaceType = MST_PATCH;
    out->shaderNum = EmitShader(ds->shaderInfo->shader);
    out->firstVert = numDrawVerts;
    out->numVerts = ds->numVerts;
    out->firstIndex = numDrawIndexes;
    out->numIndexes = ds->numIndexes;
    out->patchWidth = ds->patchWidth;
    out->patchHeight = ds->patchHeight;
    out->fogNum = ds->fogNum;
    out->lightmapNum[0] = ds->lightmapNum;
    out->lightmapOffset[0][0] = ds->lightmapX;
    out->lightmapOffset[0][1] = ds->lightmapY;
    out->lightmapWidth = ds->lightmapWidth;
    out->lightmapHeight = ds->lightmapHeight;
    // FBSP: initialize styles and auxiliary layers
    out->lightmapStyles[0] = 0;
    out->vertexStyles[0] = 0;
    for (j = 1; j < 4; j++)
    {
        out->lightmapNum[j] = -1;
        out->lightmapOffset[j][0] = 0;
        out->lightmapOffset[j][1] = 0;
        out->lightmapStyles[j] = 0xFF;
        out->vertexStyles[j] = 0xFF;
    }

    VectorCopy(ds->lightmapOrigin, out->lightmapOrigin);
    VectorCopy(ds->lightmapVecs[0], out->lightmapVecs[0]);
    VectorCopy(ds->lightmapVecs[1], out->lightmapVecs[1]);
    VectorCopy(ds->lightmapVecs[2], out->lightmapVecs[2]);

    for (j = 0; j < ds->numVerts; j++)
    {
        if (numDrawVerts >= game->maxMapDrawVerts)
        {
            _printf("\n--- VERTEX LIMIT EXCEEDED ---\n");
            _printf("Surface: %s\n", ds->shaderInfo->shader);
            _printf("Total vertices so far: %i\n", numDrawVerts);
            _printf("Current surface vertices: %i\n", ds->numVerts);
            _printf("Limit for current game profile: %i\n", game->maxMapDrawVerts);
            _printf("Advice: High-poly maps require '-game qfusion' during compilation.\n");
            _printf("-----------------------------\n");
            Error("game->maxMapDrawVerts");
        }
        outv = &drawVerts[numDrawVerts];
        numDrawVerts++;
        memcpy(outv, &ds->verts[j], sizeof(*outv));
        outv->color[0][0] = 255;
        outv->color[0][1] = 255;
        outv->color[0][2] = 255;
        outv->color[0][3] = 255;
    }

    for (j = 0; j < ds->numIndexes; j++)
    {
        if (numDrawIndexes >= game->maxMapDrawIndexes)
        {
            Error("game->maxMapDrawIndexes");
        }
        drawIndexes[numDrawIndexes] = ds->indexes[j];
        numDrawIndexes++;
    }
}

/*
===============
EmitFlareSurf
===============
*/
void EmitFlareSurf(mapDrawSurface_t *ds)
{
    dsurface_t *out;

    if (numDrawSurfaces >= game->maxMapDrawSurfs)
    {
        Error("game->maxMapDrawSurfs");
    }
    out = &drawSurfaces[numDrawSurfaces];

    drawExtraSurfaces[numDrawSurfaces].smoothingRadius = ds->smoothingRadius;
    drawExtraSurfaces[numDrawSurfaces].lightValue = ds->lightValue;
    VectorCopy(ds->lightColor, drawExtraSurfaces[numDrawSurfaces].lightColor);
    drawExtraSurfaces[numDrawSurfaces].backsplashFraction = ds->backsplashFraction;
    drawExtraSurfaces[numDrawSurfaces].lightSubdivide = ds->lightSubdivide;
    drawExtraSurfaces[numDrawSurfaces].overrideVertexColor = ds->overrideVertexColor;
    VectorCopy(ds->vertexColor, drawExtraSurfaces[numDrawSurfaces].vertexColor);
    drawExtraSurfaces[numDrawSurfaces].superSampleRadius = ds->superSampleRadius;
    drawExtraSurfaces[numDrawSurfaces].isHalo = ds->isHalo;
    drawExtraSurfaces[numDrawSurfaces].upscale = ds->upscale;
    drawExtraSurfaces[numDrawSurfaces].cutoff = ds->cutoff;
    drawExtraSurfaces[numDrawSurfaces].fadeout = ds->fadeout;
    drawExtraSurfaces[numDrawSurfaces].hasAttenuationOverride = ds->hasAttenuationOverride;
    drawExtraSurfaces[numDrawSurfaces].attenuationModel = ds->attenuationModel;
    drawExtraSurfaces[numDrawSurfaces].noDeluxeInfluence = ds->noDeluxeInfluence;
    drawExtraSurfaces[numDrawSurfaces].noDeluxeInfluenceBacksplash = ds->noDeluxeInfluenceBacksplash;
    drawExtraSurfaces[numDrawSurfaces].gridAmbientScale = ds->gridAmbientScale;
    drawExtraSurfaces[numDrawSurfaces].gridDirectScale = ds->gridDirectScale;
    drawExtraSurfaces[numDrawSurfaces].castShadows = ds->castShadows;
    drawExtraSurfaces[numDrawSurfaces].isPlanar = ds->isPlanar;
    drawExtraSurfaces[numDrawSurfaces].sampleSize = ds->samplesize;

    numDrawSurfaces++;

    out->surfaceType = MST_FLARE;
    out->shaderNum = EmitShader(ds->shaderInfo->shader);
    out->fogNum = ds->fogNum;
    // FBSP: initialize styles and auxiliary layers
    out->lightmapStyles[0] = 0;
    out->vertexStyles[0] = 0;
    {
        int j;
        for (j = 0; j < 4; j++)
        {
            out->lightmapNum[j] = -1;
            out->lightmapOffset[j][0] = 0;
            out->lightmapOffset[j][1] = 0;
        }
        for (j = 1; j < 4; j++)
        {
            out->lightmapStyles[j] = 0xFF;
            out->vertexStyles[j] = 0xFF;
        }
    }

    VectorCopy(ds->lightmapOrigin, out->lightmapOrigin);
    VectorCopy(ds->lightmapVecs[0], out->lightmapVecs[0]); // color
    VectorCopy(ds->lightmapVecs[2], out->lightmapVecs[2]);
}

/*
===============
EmitModelSurf
===============
*/
void EmitModelSurf(mapDrawSurface_t *ds)
{
    int j;
    dsurface_t *out;
    drawVert_t *outv;

    if (numDrawSurfaces >= game->maxMapDrawSurfs)
    {
        Error("game->maxMapDrawSurfs");
    }
    out = &drawSurfaces[numDrawSurfaces];

    drawExtraSurfaces[numDrawSurfaces].smoothingRadius = ds->smoothingRadius;
    drawExtraSurfaces[numDrawSurfaces].lightValue = ds->lightValue;
    VectorCopy(ds->lightColor, drawExtraSurfaces[numDrawSurfaces].lightColor);
    drawExtraSurfaces[numDrawSurfaces].backsplashFraction = ds->backsplashFraction;
    drawExtraSurfaces[numDrawSurfaces].lightSubdivide = ds->lightSubdivide;
    drawExtraSurfaces[numDrawSurfaces].overrideVertexColor = ds->overrideVertexColor;
    VectorCopy(ds->vertexColor, drawExtraSurfaces[numDrawSurfaces].vertexColor);
    drawExtraSurfaces[numDrawSurfaces].superSampleRadius = ds->superSampleRadius;
    drawExtraSurfaces[numDrawSurfaces].isHalo = ds->isHalo;
    drawExtraSurfaces[numDrawSurfaces].upscale = ds->upscale;
    drawExtraSurfaces[numDrawSurfaces].cutoff = ds->cutoff;
    drawExtraSurfaces[numDrawSurfaces].fadeout = ds->fadeout;
    drawExtraSurfaces[numDrawSurfaces].hasAttenuationOverride = ds->hasAttenuationOverride;
    drawExtraSurfaces[numDrawSurfaces].attenuationModel = ds->attenuationModel;
    drawExtraSurfaces[numDrawSurfaces].noDeluxeInfluence = ds->noDeluxeInfluence;
    drawExtraSurfaces[numDrawSurfaces].noDeluxeInfluenceBacksplash = ds->noDeluxeInfluenceBacksplash;
    drawExtraSurfaces[numDrawSurfaces].gridAmbientScale = ds->gridAmbientScale;
    drawExtraSurfaces[numDrawSurfaces].gridDirectScale = ds->gridDirectScale;
    drawExtraSurfaces[numDrawSurfaces].castShadows = ds->castShadows;
    drawExtraSurfaces[numDrawSurfaces].isPlanar = ds->isPlanar;

    numDrawSurfaces++;

    out->surfaceType = MST_TRIANGLE_SOUP;
    out->shaderNum = EmitShader(ds->shaderInfo->shader);
    out->firstVert = numDrawVerts;
    out->numVerts = ds->numVerts;
    out->firstIndex = numDrawIndexes;
    out->numIndexes = ds->numIndexes;
    out->patchWidth = ds->patchWidth;
    out->patchHeight = ds->patchHeight;
    out->fogNum = ds->fogNum;
    out->lightmapNum[0] = ds->lightmapNum;
    out->lightmapOffset[0][0] = ds->lightmapX;
    out->lightmapOffset[0][1] = ds->lightmapY;
    out->lightmapWidth = ds->lightmapWidth;
    out->lightmapHeight = ds->lightmapHeight;
    // FBSP: initialize styles and auxiliary layers
    out->lightmapStyles[0] = 0;
    out->vertexStyles[0] = 0;
    for (j = 1; j < 4; j++)
    {
        out->lightmapNum[j] = -1;
        out->lightmapOffset[j][0] = 0;
        out->lightmapOffset[j][1] = 0;
        out->lightmapStyles[j] = 0xFF;
        out->vertexStyles[j] = 0xFF;
    }

    VectorCopy(ds->lightmapOrigin, out->lightmapOrigin);
    VectorCopy(ds->lightmapVecs[0], out->lightmapVecs[0]);
    VectorCopy(ds->lightmapVecs[1], out->lightmapVecs[1]);
    VectorCopy(ds->lightmapVecs[2], out->lightmapVecs[2]);

    for (j = 0; j < ds->numVerts; j++)
    {
        if (numDrawVerts >= game->maxMapDrawVerts)
        {
            _printf("\n--- VERTEX LIMIT EXCEEDED ---\n");
            _printf("Surface: %s\n", ds->shaderInfo->shader);
            _printf("Total vertices so far: %i\n", numDrawVerts);
            _printf("Current surface vertices: %i\n", ds->numVerts);
            _printf("Limit for current game profile: %i\n", game->maxMapDrawVerts);
            _printf("Advice: High-poly maps require '-game qfusion' during compilation.\n");
            _printf("-----------------------------\n");
            Error("game->maxMapDrawVerts");
        }
        outv = &drawVerts[numDrawVerts];
        numDrawVerts++;
        memcpy(outv, &ds->verts[j], sizeof(*outv));
        outv->color[0][0] = 255;
        outv->color[0][1] = 255;
        outv->color[0][2] = 255;
    }

    for (j = 0; j < ds->numIndexes; j++)
    {
        if (numDrawIndexes >= game->maxMapDrawIndexes)
        {
            Error("game->maxMapDrawIndexes");
        }
        drawIndexes[numDrawIndexes] = ds->indexes[j];
        numDrawIndexes++;
    }
}

//======================================================================

/*
==================
CreateFlareSurface

Light flares from surface lights become
==================
*/
void CreateFlareSurface(mapDrawSurface_t *faceDs)
{
    mapDrawSurface_t *ds;
    int i;

    ds = AllocDrawSurf();

    if (faceDs->shaderInfo->flareShader[0])
    {
        ds->shaderInfo = ShaderInfoForShader(faceDs->shaderInfo->flareShader);
    }
    else
    {
        ds->shaderInfo = ShaderInfoForShader("flareshader");
    }
    ds->flareSurface = qtrue;
    VectorCopy(faceDs->lightmapVecs[2], ds->lightmapVecs[2]);

    // find midpoint
    VectorClear(ds->lightmapOrigin);
    for (i = 0; i < faceDs->numVerts; i++)
    {
        VectorAdd(ds->lightmapOrigin, faceDs->verts[i].xyz, ds->lightmapOrigin);
    }
    VectorScale(ds->lightmapOrigin, 1.0 / faceDs->numVerts, ds->lightmapOrigin);

    VectorMA(ds->lightmapOrigin, 2, ds->lightmapVecs[2], ds->lightmapOrigin);

    VectorCopy(faceDs->shaderInfo->color, ds->lightmapVecs[0]);

    // FIXME: fog
}

/*
=====================
FilterDrawsurfsIntoTree

Upon completion, all drawsurfs that actually generate a reference
will have been emited to the bspfile arrays, and the references
will have valid final indexes
=====================
*/
void FilterDrawsurfsIntoTree(entity_t *e, tree_t *tree)
{
    int i;
    mapDrawSurface_t *ds;
    int refs;
    int c_surfs, c_refs;

    _printf("----- FilterDrawsurfsIntoTree -----\n");

    c_surfs = 0;
    c_refs = 0;
    for (i = e->firstDrawSurf; i < numMapDrawSurfs; i++)
    {
        ds = &mapDrawSurfs[i];

        if (!ds->numVerts && !ds->flareSurface)
        {
            continue;
        }
        if (ds->isDecal && !ds->miscModel)
        {
            winding_t *w = WindingFromDrawSurf(ds);
            refs = FilterMapDrawSurfIntoTree_r(w, ds, tree->headnode);
            if (refs > 0)
            {
                EmitPlanarSurf(ds);
            }
        }
        else if (ds->miscModel)
        {
            refs = FilterMiscModelSurfIntoTree(ds, tree);
            if (refs > 0)
            {
                EmitModelSurf(ds);
            }
        }
        else if (ds->patch)
        {
            refs = FilterPatchSurfIntoTree(ds, tree);
            if (refs > 0)
            {
                EmitPatchSurf(ds);
            }
        }
        else if (ds->flareSurface)
        {
            refs = FilterFlareSurfIntoTree(ds, tree);
            if (refs > 0)
            {
                EmitFlareSurf(ds);
            }
        }
        else if (ds->isHalo)
        {
            winding_t *w = WindingFromDrawSurf(ds);
            refs = FilterMapDrawSurfIntoTree_r(w, ds, tree->headnode);
            if (refs > 0)
            {
                EmitPlanarSurf(ds);
            }
        }
        else
        {
            refs = FilterFaceIntoTree(ds, tree);
            if (refs > 0)
            {
                if (ds->shaderInfo->flareShader[0])
                {
                    CreateFlareSurface(ds);
                }
                EmitPlanarSurf(ds);
            }
        }
        if (refs > 0)
        {
            c_surfs++;
            c_refs += refs;
        }
    }
    _printf("%5i emited drawsurfs\n", c_surfs);
    _printf("%5i references\n", c_refs);
    _printf("%5i stripfaces\n", c_stripSurfaces);
    _printf("%5i fanfaces\n", c_fanSurfaces);
}

/*
================
FindTargetEntity
================
*/
static entity_t *FindTargetEntity(const char *target)
{
    int i;
    const char *n;

    for (i = 0; i < num_entities; i++)
    {
        n = ValueForKey(&entities[i], "targetname");
        if (!strcmp(n, target))
        {
            return &entities[i];
        }
    }

    return NULL;
}

/*
================
GenerateHalos
================
*/
void GenerateHalos(entity_t *e)
{
    int i;
    entity_t *light;
    const char *name;
    const char *target;
    vec3_t origin, normal, dest;
    float intensity, radius;
    const char *_color;
    vec3_t color;
    int count = 0;

    if (!game->haloShader || !game->haloShader[0]) return;

    qprintf("----- GenerateHalos -----\n");

    for (i = 0; i < num_entities; i++) {
        light = &entities[i];
        name = ValueForKey(light, "classname");
        if (strncmp(name, "light", 5)) continue;
        if (ValueForKey(light, "_sun")[0]) continue; // not a sun

        qboolean isSpotlight = qfalse;
        target = ValueForKey(light, "target");
        if (target[0]) {
            entity_t *e2 = FindTargetEntity(target);
            if (e2) {
                GetVectorForKey(e2, "origin", dest);
                GetVectorForKey(light, "origin", origin);
                VectorSubtract(dest, origin, normal);
                if (VectorNormalize(normal, normal) > 0) isSpotlight = qtrue;
            }
        } else if (ValueForKey(light, "dir")[0]) {
            GetVectorForKey(light, "dir", normal);
            if (VectorNormalize(normal, normal) > 0) isSpotlight = qtrue;
        } else if (ValueForKey(light, "angles")[0]) {
            vec3_t angles;
            GetVectorForKey(light, "angles", angles);
            float yaw = angles[1] * (Q_PI / 180.0f);
            float pitch = angles[0] * (Q_PI / 180.0f);
            normal[0] = cos(yaw) * cos(pitch);
            normal[1] = sin(yaw) * cos(pitch);
            normal[2] = -sin(pitch);
            VectorNormalize(normal, normal);
            isSpotlight = qtrue;
        }

        if (isSpotlight) {
            const char *haloShader = ValueForKey(light, "haloshader");
            if (!haloShader[0]) {
                haloShader = game->haloShader;
            }
            
            if (!haloShader || !haloShader[0] || !Q_stricmp(haloShader, "none") || !strcmp(haloShader, "0")) {
                continue;
            }

            if (!ShaderExists(haloShader)) {
                _printf("WARNING: Halo shader '%s' not found, skipping halo generation.\n", haloShader);
                continue;
            }

            GetVectorForKey(light, "origin", origin);

            // Move the origin 8 units backwards so the halo doesn't start abruptly in mid-air
            VectorMA(origin, -8.0f, normal, origin);

            intensity = FloatForKey(light, "light");
            if (!intensity) intensity = 300;

            _color = ValueForKey(light, "color");
            if (_color[0]) {
                ParseColor(_color, color);
            } else {
                VectorSet(color, 1, 1, 1);
            }

            // Normalize luminance to 0.7 so faint colors become brighter, 
            // and pure white (1.0) drops down slightly to 0.7
            float lum = color[0] * 0.299f + color[1] * 0.587f + color[2] * 0.114f;
            if (lum > 0.001f) {
                float scale = 0.7f / lum;
                VectorScale(color, scale, color);
            }

            radius = FloatForKey(light, "radius");
            if (!radius) radius = 64;

            // Scale length to 0.3x intensity to keep it from extending too far
            float length = intensity * 0.3f;
            if (length > 768.0f) length = 768.0f;
            if (length < 128.0f) length = 128.0f;
            
            // The texture needs a constant scaling factor to fit the curved halo perfectly.
            // You can adjust this 'haloScale' value until it looks right in-game.
            float haloScale = 1.5f; 
            
            // Clamp the radius for the quad calculation so extremely wide spotlights
            // don't create overwhelmingly massive billboards that clip the room
            float quadRadius = radius;
            if (quadRadius > 90.0f) quadRadius = 90.0f;
            
            // Calculate the physical width of the cone at the END of the quad (at `length`)
            // and apply the haloScale padding to fit the texture.
            float physicalWidthAtEnd = 2.0f * length * (quadRadius / 64.0f);
            float width = physicalWidthAtEnd * haloScale;
            
            float customHaloScale = FloatForKey(light, "haloscale");
            if (customHaloScale == 0.0f) {
                customHaloScale = 1.0f;
            }
            
            length *= customHaloScale;
            width *= customHaloScale;
            
            // Ensure length is strictly greater than width so autosprite2 rotates around the correct axis
            if (width >= length) {
                width = length - 1.0f;
            }

            // Generate mapDrawSurface_t
            mapDrawSurface_t *ds = AllocDrawSurf();
            ds->shaderInfo = ShaderInfoForShader(haloShader);
            ds->isHalo = qtrue;
            ds->lightmapNum = -1;
            ds->fogNum = -1;

            ds->numVerts = 4;
            ds->verts = malloc(4 * sizeof(drawVert_t));
            memset(ds->verts, 0, 4 * sizeof(drawVert_t));
            
            vec3_t right, up;
            vec3_t temp;
            VectorSet(temp, 0, 0, 1);
            if (fabs(normal[2]) > 0.99f) VectorSet(temp, 1, 0, 0);
            CrossProduct(temp, normal, right);
            VectorNormalize(right, right);
            CrossProduct(normal, right, up);

            VectorMA(origin, -width * 0.5f, right, ds->verts[0].xyz);
            VectorMA(origin,  width * 0.5f, right, ds->verts[1].xyz);
            VectorMA(ds->verts[1].xyz, length, normal, ds->verts[2].xyz);
            VectorMA(ds->verts[0].xyz, length, normal, ds->verts[3].xyz);

            for (int v = 0; v < 4; v++) {
                VectorCopy(up, ds->verts[v].normal);
                
                float r = color[0] * 255.0f;
                float g = color[1] * 255.0f;
                float b = color[2] * 255.0f;
                
                ds->verts[v].color[0][0] = r > 255.0f ? 255 : (byte)r;
                ds->verts[v].color[0][1] = g > 255.0f ? 255 : (byte)g;
                ds->verts[v].color[0][2] = b > 255.0f ? 255 : (byte)b;
                ds->verts[v].color[0][3] = 255;
            }
            ds->overrideVertexColor = qtrue;
            VectorCopy(color, ds->vertexColor);

            ds->verts[0].st[0] = 0; ds->verts[0].st[1] = 0;
            ds->verts[1].st[0] = 1; ds->verts[1].st[1] = 0;
            ds->verts[2].st[0] = 1; ds->verts[2].st[1] = 1;
            ds->verts[3].st[0] = 0; ds->verts[3].st[1] = 1;

            VectorCopy(origin, ds->lightmapOrigin);
            VectorClear(ds->lightmapVecs[0]); // Ensure no garbage
            VectorClear(ds->lightmapVecs[1]); // Ensure no garbage
            VectorCopy(normal, ds->lightmapVecs[2]);
            
            count++;
        }
    }
    
    _printf("%5i halos generated\n", count);
}
