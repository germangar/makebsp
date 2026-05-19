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
// light.c

#include "light.h"
#include "radiosity.h"
#include "../common/imagelib.h"
#include <string.h>
#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

#define DEFAULT_SPOTLIGHT_DISTANCE 64.0f

ssMode_t superSampleMode = SUPERSAMPLE_NONE;

qboolean notrace;
qboolean patchshadows = qtrue;
qboolean upscale;
qboolean lightmapBorder;

qboolean debugLightmaps;
qboolean debugLightmapsAlpha;

int novertexlighting = 0;
int nogridlighting = 0;

// for run time tweaking of all area sources in the level
float areaScale = 0.25;

// for run time tweaking of all point sources in the level
float pointScale = 7500;
int CompareSurfaces(const void *a, const void *b)
{
    int i1 = *(int *)a;
    int i2 = *(int *)b;
    dsurface_t *ds1 = &drawSurfaces[i1];
    dsurface_t *ds2 = &drawSurfaces[i2];
    int w1 = ds1->lightmapWidth * ds1->lightmapHeight;
    int w2 = ds2->lightmapWidth * ds2->lightmapHeight;

    // Bonus weight for patches and triangle soups as they take more CPU cycles per luxel
    if (ds1->surfaceType == MST_PATCH || ds1->surfaceType == MST_TRIANGLE_SOUP)
        w1 *= 2;
    if (ds2->surfaceType == MST_PATCH || ds2->surfaceType == MST_TRIANGLE_SOUP)
        w2 *= 2;

    if (w1 > w2)
        return -1;
    if (w1 < w2)
        return 1;
    return 0;
}

float formFactorValueScale = 3;

float linearScale = 1.0 / 8000;

light_t *lights;
int numPointLights;
int numAreaLights;
int numLights;

vec3_t gridMins;
vec3_t gridSize = {64, 64, 128};
int gridBounds[3];
// numGridPoints is defined in bspfile.c

// int			defaultLightSubdivide = 128;		// vary by
// surface size?
int defaultLightSubdivide = 999; // vary by surface size?

vec3_t ambientColor;

localSurface_t *localSurfaces;

// 7,9,11 normalized to avoid being nearly coplanar with common faces
// vec3_t		sunDirection = { 0.441835, 0.56807, 0.694313 };
// vec3_t		sunDirection = { 0, 0, 1 };

// these are usually overrided by shader values
vec3_t sunDirection = {0.45, 0.3, 0.9};
vec3_t sunLight = {100, 100, 50};

int numSkyBrushes;
skyBrush_t skyBrushes[MAX_MAP_BRUSHES];

/*
=================================================================

  LIGHT SETUP

=================================================================
*/

/*
================
FindSkyBrushes
================
*/
void FindSkyBrushes(void)
{
    int i, j;
    dbrush_t *b;
    skyBrush_t *sb;
    dbrushside_t *s;

    // find the brushes
    for (i = 0; i < numbrushes; i++)
    {
        b = &dbrushes[i];
        for (j = 0; j < b->numSides; j++)
        {
            s = &dbrushsides[b->firstSide + j];
            if (dshaders[s->shaderNum].surfaceFlags & SURF_SKY)
            {
                sb = &skyBrushes[numSkyBrushes];
                sb->b = b;
                sb->bounds[0][0] =
                    -dplanes[dbrushsides[b->firstSide + 0].planeNum].dist - 1;
                sb->bounds[1][0] =
                    dplanes[dbrushsides[b->firstSide + 1].planeNum].dist + 1;
                sb->bounds[0][1] =
                    -dplanes[dbrushsides[b->firstSide + 2].planeNum].dist - 1;
                sb->bounds[1][1] =
                    dplanes[dbrushsides[b->firstSide + 3].planeNum].dist + 1;
                sb->bounds[0][2] =
                    -dplanes[dbrushsides[b->firstSide + 4].planeNum].dist - 1;
                sb->bounds[1][2] =
                    dplanes[dbrushsides[b->firstSide + 5].planeNum].dist + 1;
                numSkyBrushes++;
                break;
            }
        }
    }

    // default
    VectorNormalize(sunDirection, sunDirection);

    // find the sky shader
    // (NOTE: This is now handled by InjectSunEntity in the BSP stage
    // and CreateEntityLights in the Light stage via the entity system)
}

/*
===============
SubdivideAreaLight

Subdivide area lights that are very large
A light that is subdivided will never backsplash, avoiding weird pools of light
near edges
===============
*/
void SubdivideAreaLight(shaderInfo_t *ls, winding_t *w, vec3_t normal,
                        float areaSubdivide, qboolean backsplash)
{
    float area, value, intensity;
    light_t *dl, *dl2;
    vec3_t mins, maxs;
    int axis;
    winding_t *front, *back;
    vec3_t planeNormal;
    float planeDist;

    if (!w)
    {
        return;
    }

    WindingBounds(w, mins, maxs);

    // check for subdivision
    for (axis = 0; axis < 3; axis++)
    {
        if (maxs[axis] - mins[axis] > areaSubdivide)
        {
            VectorClear(planeNormal);
            planeNormal[axis] = 1;
            planeDist = (maxs[axis] + mins[axis]) * 0.5;
            ClipWindingEpsilon(w, planeNormal, planeDist, ON_EPSILON, &front, &back);
            SubdivideAreaLight(ls, front, normal, areaSubdivide, qfalse);
            SubdivideAreaLight(ls, back, normal, areaSubdivide, qfalse);
            FreeWinding(w);
            return;
        }
    }

    // create a light from this
    area = WindingArea(w);
    if (area <= 0 || area > 20000000)
    {
        return;
    }

    numAreaLights++;
    dl = malloc(sizeof(*dl));
    memset(dl, 0, sizeof(*dl));
    dl->next = lights;
    lights = dl;
    dl->type = emit_area;

    WindingCenter(w, dl->origin);
    dl->w = w;
    dl->area = area;
    VectorCopy(normal, dl->normal);
    dl->dist = DotProduct(dl->origin, normal);

    value = ls->value;
    intensity = value * area * areaScale;
    VectorMA(dl->origin, 0.1f, dl->normal, dl->origin);

    VectorCopy(ls->color, dl->color);

    dl->photons = intensity;

    // emitColor is irrespective of the area
    float volumetricScale = 1.0f;
    if (ls->contents & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER | CONTENTS_FOG))
    {
        volumetricScale = 0.25f;
    }

    VectorScale(ls->color, value * formFactorValueScale * areaScale * volumetricScale,
                dl->emitColor);

    dl->si = ls;

    if (ls->contents & (CONTENTS_FOG | CONTENTS_LAVA | CONTENTS_SLIME))
    {
        dl->twosided = qtrue;
    }

    // optionally create a point backsplash light
    if (backsplash && ls->backsplashFraction > 0)
    {
        dl2 = malloc(sizeof(*dl));
        memset(dl2, 0, sizeof(*dl2));
        dl2->next = lights;
        lights = dl2;
        dl2->type = emit_point;

        VectorMA(dl->origin, ls->backsplashDistance, normal, dl2->origin);

        VectorCopy(ls->color, dl2->color);

        dl2->photons = dl->photons * ls->backsplashFraction;
        dl2->si = ls;
        dl2->reach = CalculateLightReach(0, dl2->photons, MIN_LIGHT_ADD, qfalse);
    }

    dl->reach = CalculateLightReach(area, value * areaScale, MIN_LIGHT_ADD, qfalse);
}

/*
===============
CreateSurfaceLights

This creates area lights
===============
*/
void CreateSurfaceLights(void)
{
    int i, j, side;
    dsurface_t *ds;
    shaderInfo_t *ls;
    winding_t *w;
    light_t *dl;
    vec3_t origin;
    drawVert_t *dv;
    int c_lightSurfaces;
    float lightSubdivide;
    vec3_t normal;

    qprintf("--- CreateSurfaceLights ---\n");
    c_lightSurfaces = 0;

    for (i = 0; i < numDrawSurfaces; i++)
    {
        // see if this surface is light emiting
        ds = &drawSurfaces[i];
        localSurface_t *localSurface = &localSurfaces[i];

        if (localSurface->si_override) {
            ls = localSurface->si_override;
        } else {
            ls = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
        }

        if (ls->value == 0)
        {
            continue;
        }

        // determine how much we need to chop up the surface
        if (ls->lightSubdivide)
        {
            lightSubdivide = ls->lightSubdivide;
        }
        else
        {
            lightSubdivide = defaultLightSubdivide;
        }

        c_lightSurfaces++;

        // an autosprite shader will not create any lights
        if (ls->autosprite)
        {
            continue;
        }

        // possibly create for both sides of the polygon
        int maxSide = ls->twoSided;
        // Liquids are volumetric and emit omnidirectionally from a single plane
        if (ls->contents & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER | CONTENTS_FOG))
        {
            maxSide = 0;
        }

        for (side = 0; side <= maxSide; side++)
        {
            if (ds->surfaceType == MST_PATCH)
            {
                // curves
                mesh_t srcMesh, *mesh, *subdivided;
                srcMesh.width = ds->patchWidth;
                srcMesh.height = ds->patchHeight;
                srcMesh.verts = &drawVerts[ds->firstVert];

                mesh = SubdivideMesh(srcMesh, 8, 999);
                PutMeshOnCurve(*mesh);
                MakeMeshNormals(*mesh);
                subdivided = RemoveLinearMeshColumnsRows(mesh);
                FreeMesh(mesh);

                for (int x = 0; x < subdivided->width - 1; x++)
                {
                    for (int y = 0; y < subdivided->height - 1; y++)
                    {
                        drawVert_t *v[4];
                        v[0] = subdivided->verts + y * subdivided->width + x;
                        v[1] = v[0] + 1;
                        v[2] = v[0] + subdivided->width + 1;
                        v[3] = v[0] + subdivided->width;

                        // try to make a quad, otherwise two triangles
                        vec4_t plane;
                        PlaneFromPoints(plane, v[0]->xyz, v[3]->xyz, v[2]->xyz);
                        float dist = DotProduct(v[1]->xyz, plane) - plane[3];

                        if (fabs(dist) < 0.1f)
                        {
                            // quad
                            w = AllocWinding(4);
                            w->numpoints = 4;
                            VectorCopy(v[0]->xyz, w->points[0]);
                            VectorCopy(v[3]->xyz, w->points[1]);
                            VectorCopy(v[2]->xyz, w->points[2]);
                            VectorCopy(v[1]->xyz, w->points[3]);
                            VectorCopy(plane, normal);
                            if (side)
                            {
                                winding_t *t = w;
                                w = ReverseWinding(t);
                                FreeWinding(t);
                                VectorSubtract(vec3_origin, normal, normal);
                            }
                            SubdivideAreaLight(ls, w, normal, lightSubdivide, qtrue);
                        }
                        else
                        {
                            // two triangles
                            // tri 1
                            w = AllocWinding(3);
                            w->numpoints = 3;
                            VectorCopy(v[0]->xyz, w->points[0]);
                            VectorCopy(v[3]->xyz, w->points[1]);
                            VectorCopy(v[2]->xyz, w->points[2]);
                            VectorCopy(plane, normal);
                            if (side)
                            {
                                winding_t *t = w;
                                w = ReverseWinding(t);
                                FreeWinding(t);
                                VectorSubtract(vec3_origin, normal, normal);
                            }
                            SubdivideAreaLight(ls, w, normal, lightSubdivide, qtrue);

                            // tri 2
                            w = AllocWinding(3);
                            w->numpoints = 3;
                            VectorCopy(v[0]->xyz, w->points[0]);
                            VectorCopy(v[2]->xyz, w->points[1]);
                            VectorCopy(v[1]->xyz, w->points[2]);
                            PlaneFromPoints(plane, v[0]->xyz, v[2]->xyz, v[1]->xyz);
                            VectorCopy(plane, normal);
                            if (side)
                            {
                                winding_t *t = w;
                                w = ReverseWinding(t);
                                FreeWinding(t);
                                VectorSubtract(vec3_origin, normal, normal);
                            }
                            SubdivideAreaLight(ls, w, normal, lightSubdivide, qtrue);
                        }
                    }
                }
                FreeMesh(subdivided);
            }
            else if (ds->surfaceType == MST_TRIANGLE_SOUP || ds->surfaceType == MST_PLANAR)
            {
                // polygons or misc_models
                for (j = 0; j < ds->numIndexes; j += 3)
                {
                    int i0 = drawIndexes[ds->firstIndex + j];
                    int i1 = drawIndexes[ds->firstIndex + j + 1];
                    int i2 = drawIndexes[ds->firstIndex + j + 2];

                    w = AllocWinding(3);
                    w->numpoints = 3;
                    VectorCopy(drawVerts[ds->firstVert + i0].xyz, w->points[0]);
                    VectorCopy(drawVerts[ds->firstVert + i1].xyz, w->points[1]);
                    VectorCopy(drawVerts[ds->firstVert + i2].xyz, w->points[2]);

                    if (ds->surfaceType == MST_PLANAR)
                    {
                        VectorCopy(ds->lightmapVecs[2], normal);
                    }
                    else
                    {
                        vec4_t plane;
                        PlaneFromPoints(plane, w->points[0], w->points[1], w->points[2]);
                        VectorCopy(plane, normal);
                    }

                    if (side)
                    {
                        winding_t *t = w;
                        w = ReverseWinding(t);
                        FreeWinding(t);
                        VectorSubtract(vec3_origin, normal, normal);
                    }
                    SubdivideAreaLight(ls, w, normal, lightSubdivide, qtrue);
                }
            }
        }
    }

    _printf("%5i light emitting surfaces\n", c_lightSurfaces);
}

/*
==================
FindTargetEntity
==================
*/
entity_t *FindTargetEntity(const char *target)
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
=============
CreateEntityLights
=============
*/
void CreateEntityLights(void)
{
    int i;
    light_t *dl;
    entity_t *e, *e2;
    const char *name;
    const char *target;
    vec3_t dest;
    const char *_color;
    float intensity;
    int spawnflags;

    //
    // entities
    //
    for (i = 0; i < num_entities; i++)
    {
        e = &entities[i];
        name = ValueForKey(e, "classname");
        if (strncmp(name, "light", 5))
            continue;

        // Check if this is a sun entity (injected or manual)
        if (ValueForKey(e, "_sun")[0])
        {
            float intensity;
            const char *t;
            entity_t *tEnt;

            // 1. Direction Calculation (Targeting > Vector Fallback)
            t = ValueForKey(e, "target");
            if (t && t[0] && (tEnt = FindTargetEntity(t)))
            {
                vec3_t sunOrigin, targetOrigin;
                GetVectorForKey(e, "origin", sunOrigin);
                GetVectorForKey(tEnt, "origin", targetOrigin);
                VectorSubtract(sunOrigin, targetOrigin, sunDirection);
            }
            else
            {
                // Fallback to high-precision dir from injector or default
                const char *sunDirKey = ValueForKey(e, "_sun_dir");
                if (sunDirKey && sunDirKey[0])
                {
                    GetVectorForKey(e, "_sun_dir", sunDirection);
                }
                else
                {
                    VectorSet(sunDirection, -0.45, -0.3, 0.9); // Q3 default-ish (UP)
                }
            }
            VectorNormalize(sunDirection, sunDirection);

            // 2. Intensity and Color
            intensity = FloatForKey(e, "light");
            if (!intensity)
                intensity = FloatForKey(e, "_light");

            _color = ValueForKey(e, "_color");
            if (_color && _color[0])
            {
                ParseColor(_color, sunLight);

                // If the mapper provided a separate 'light' key, we treat sunLight as a normalized multiplier
                if (intensity > 0)
                {
                    VectorScale(sunLight, intensity, sunLight);
                }
            }
            else
            {
                // Default white sun if no color provided
                if (intensity <= 0)
                    intensity = 300.0f;
                VectorSet(sunLight, intensity, intensity, intensity);
            }

            continue;
        }

        numPointLights++;
        dl = malloc(sizeof(*dl));
        memset(dl, 0, sizeof(*dl));
        dl->next = lights;
        lights = dl;
        dl->coneSoftness = 1.0f;

        spawnflags = FloatForKey(e, "spawnflags");
        if (spawnflags & 1)
        {
            dl->linearLight = qtrue;
        }

        GetVectorForKey(e, "origin", dl->origin);
        dl->style = FloatForKey(e, "_style");
        if (!dl->style)
            dl->style = FloatForKey(e, "style");
        if (dl->style < 0)
            dl->style = 0;

        intensity = FloatForKey(e, "light");
        if (!intensity)
            intensity = FloatForKey(e, "_light");
        if (!intensity)
            intensity = 300;
        _color = ValueForKey(e, "_color");
        if (_color && _color[0])
        {
            ParseColor(_color, dl->color);
        }
        else
        {
            // If no color key, check for _lightimage
            const char *lightimage = ValueForKey(e, "_lightimage");
            if (lightimage[0])
            {
                shaderInfo_t *si = ShaderInfoForShader(lightimage);
                if (si)
                {
                    VectorScale(si->averageColor, 1.0f / 255.0f, dl->color);
                }
                else
                {
                    dl->color[0] = dl->color[1] = dl->color[2] = 1.0;
                }
            }
            else
            {
                dl->color[0] = dl->color[1] = dl->color[2] = 1.0;
            }
        }

        intensity = intensity * pointScale;
        dl->photons = intensity;

        dl->coneSoftness = FloatForKey(e, "_softness");
        if (!dl->coneSoftness)
            dl->coneSoftness = FloatForKey(e, "softness");
        if (dl->coneSoftness < 0)
            dl->coneSoftness = 0;
        else if (!ValueForKey(e, "_softness")[0] && !ValueForKey(e, "softness")[0])
            dl->coneSoftness = 1.0f; // Default if both keys missing

        dl->type = emit_point;

        // spotlights
        target = ValueForKey(e, "target");
        const char *dirStr = ValueForKey(e, "_dir");
        const char *anglesStr = ValueForKey(e, "_angles");
        qboolean isSpotlight = qfalse;

        if (target[0])
        {
            e2 = FindTargetEntity(target);
            if (!e2)
            {
                _printf("WARNING: light at (%i %i %i) has missing target\n",
                        (int)dl->origin[0], (int)dl->origin[1], (int)dl->origin[2]);
            }
            else
            {
                GetVectorForKey(e2, "origin", dest);
                VectorSubtract(dest, dl->origin, dl->normal);
                if (VectorNormalize(dl->normal, dl->normal) > 0)
                {
                    isSpotlight = qtrue;
                }
            }
        }
        else if (dirStr[0])
        {
            GetVectorForKey(e, "_dir", dl->normal);
            if (VectorNormalize(dl->normal, dl->normal) > 0)
            {
                isSpotlight = qtrue;
            }
        }
        else if (anglesStr[0])
        {
            vec3_t angles;
            GetVectorForKey(e, "_angles", angles);
            float yaw = angles[1] * (Q_PI / 180.0f);
            float pitch = angles[0] * (Q_PI / 180.0f);
            dl->normal[0] = cos(yaw) * cos(pitch);
            dl->normal[1] = sin(yaw) * cos(pitch);
            dl->normal[2] = -sin(pitch);
            VectorNormalize(dl->normal, dl->normal);
            isSpotlight = qtrue;
        }

        if (isSpotlight)
        {
            float radius = FloatForKey(e, "radius");
            if (!radius)
                radius = 64;

            dl->radiusByDist = (radius + ((SPOTLIGHT_SOFTNESS_RANGE * 0.5f) * dl->coneSoftness)) / DEFAULT_SPOTLIGHT_DISTANCE;
            dl->type = emit_spotlight;

            // Backlight / Backsplash implementation
            float bsIntensity = 0.0f;
            const char *bsStr = ValueForKey(e, "backsplash");
            
            if (bsStr[0])
                bsIntensity = atof(bsStr);

            if (bsIntensity > 0)
            {
                light_t *bl = malloc(sizeof(*bl));
                memcpy(bl, dl, sizeof(*bl)); // Inherit color, style, flags, etc.
                bl->next = lights;
                lights = bl;

                bl->photons = bsIntensity * pointScale;
                VectorMA(dl->origin, 3.0f, dl->normal, bl->origin);
                bl->type = emit_point;
                bl->reach = CalculateLightReach(0, bl->photons, MIN_LIGHT_ADD, bl->linearLight);
            }
        }
        dl->reach = CalculateLightReach(0, dl->photons, MIN_LIGHT_ADD, dl->linearLight);
    }
}

/*
================
SetEntityOrigins

Find the offset values for inline models
================
*/
void SetEntityOrigins(void)
{
    int i, j;
    entity_t *e;
    vec3_t origin;
    const char *key;
    int modelnum;
    dmodel_t *dm;

    for (i = 0; i < num_entities; i++)
    {
        e = &entities[i];
        key = ValueForKey(e, "model");
        if (key[0] != '*')
        {
            continue;
        }
        modelnum = atoi(key + 1);
        dm = &dmodels[modelnum];

        // set entity surface to true for all surfaces for this model
        for (j = 0; j < dm->numSurfaces; j++)
        {
            localSurfaces[dm->firstSurface + j].isEntity = qtrue;
        }

        key = ValueForKey(e, "origin");
        if (!key[0])
        {
            continue;
        }
        GetVectorForKey(e, "origin", origin);

        // set origin for all surfaces for this model
        for (j = 0; j < dm->numSurfaces; j++)
        {
            int surfIdx = dm->firstSurface + j;
            VectorCopy(origin, localSurfaces[surfIdx].entityOrigin);
            localSurfaces[surfIdx].isEntity = qtrue;
        }
    }
}

/*
================
LoadSurfaceExtraFile

Loads per-surface metadata from a binary .srf sidecar file.
================
*/
static extraSurface_t *LoadSurfaceExtraFile(const char *path, int *numSurfaces)
{
    char srfPath[1024];
    char baseName[256];
    FILE *f;
    int count;
    extraSurface_t *extra;

    ExtractFileBase(path, baseName);
    sprintf(srfPath, "cache/%s.srf", baseName);

    f = fopen(srfPath, "rb");
    if (!f)
        return NULL;

    if (fread(&count, sizeof(int), 1, f) != 1)
    {
        fclose(f);
        return NULL;
    }

    extra = malloc(sizeof(extraSurface_t) * count);
    if (fread(extra, sizeof(extraSurface_t), count, f) != (size_t)count)
    {
        free(extra);
        fclose(f);
        return NULL;
    }

    fclose(f);
    if (numSurfaces)
        *numSurfaces = count;
    return extra;
}

/*
================
BuildLocalSurfaces

Consolidates geometric bounds, entity offsets, and sidecar data
================
*/
void BuildLocalSurfaces(void)
{
    int i, j;
    vec3_t mins, maxs;
    char mapName[1024];
    int numPatchesSubdivided = 0;

    _printf("--- BuildLocalSurfaces ---\n");

    localSurfaces = calloc(numDrawSurfaces, sizeof(localSurface_t));

    for (i = 0; i < numDrawSurfaces; i++)
    {
        dsurface_t *ds = &drawSurfaces[i];
        localSurfaces[i].smoothingRadius = -1.0f;

        // 1. Compute geometric bounds
        ClearBounds(mins, maxs);
        if (ds->numVerts > 0)
        {
            for (j = 0; j < ds->numVerts; j++)
            {
                AddPointToBounds(drawVerts[ds->firstVert + j].xyz, mins, maxs);
            }
            SphereFromBounds(mins, maxs, localSurfaces[i].origin, &localSurfaces[i].radius);
        }
        else
        {
            VectorClear(localSurfaces[i].origin);
            localSurfaces[i].radius = 0;
        }

        // 1.5 Cache patch geometry
        if (ds->surfaceType == MST_PATCH)
        {
            shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
            int ssize = samplesize;
            if (si && si->lightmapSampleSize)
                ssize = si->lightmapSampleSize;
            localSurfaces[i].patchMesh = SubdividePatchForLighting(ds, (float)ssize);
            numPatchesSubdivided++;
        }
        else
        {
            localSurfaces[i].patchMesh = NULL;
        }
    }

    if (numPatchesSubdivided > 0)
    {
        _printf("    %d patch meshes subdivided and cached.\n", numPatchesSubdivided);
    }

    // 2. Process entity origins (writes to localSurfaces[i].entityOrigin / isEntity)
    SetEntityOrigins();

    // 3. Load sidecar metadata and initialize defaults
    // mapName logic from main.c
    extern char source[];
    strcpy(mapName, source);
    StripExtension(mapName);
    
    int numExtra;
    extraSurface_t *extra = LoadSurfaceExtraFile(mapName, &numExtra);
    
    extern int rad_interval;
    for (i = 0; i < numDrawSurfaces; i++)
    {
        dsurface_t *ds = &drawSurfaces[i];
        localSurfaces[i].radInterval = rad_interval;
        
        // Ensure trisoups have enough quality for radiosity unless -fast is active
        if (ds->surfaceType == MST_TRIANGLE_SOUP && !g_fast)
        {
            shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
            int ssize = samplesize;
            if (si && si->lightmapSampleSize)
                ssize = si->lightmapSampleSize;

            int oldInterval = localSurfaces[i].radInterval;
            while (localSurfaces[i].radInterval > 1 && localSurfaces[i].radInterval * ssize > 16) {
                localSurfaces[i].radInterval /= 2;
            }
            if (verbose && localSurfaces[i].radInterval != oldInterval) {
                _printf("  Surface %d (Trisoup): rad_interval reduced from %d to %d (ssize: %d)\n", i, oldInterval, localSurfaces[i].radInterval, ssize);
            }
        }

        localSurfaces[i].smoothingRadius = game->defaultSmoothRadius;
        localSurfaces[i].si_override = NULL;

        // Apply sidecar if present
        if (extra && i < numExtra)
        {
            qboolean shaderOverride = qfalse;

            if (extra[i].smoothingRadius >= 0.0f) {
                localSurfaces[i].smoothingRadius = extra[i].smoothingRadius;
            }
            if (extra[i].lightValue >= 0.0f || extra[i].backsplashFraction >= 0.0f || extra[i].lightSubdivide >= 0.0f) {
                shaderOverride = qtrue;
            }
            if (extra[i].lightColor[0] >= 0.0f && extra[i].lightColor[1] >= 0.0f && extra[i].lightColor[2] >= 0.0f ) {
                shaderOverride = qtrue;
            }

            if (shaderOverride)
            {
                localSurfaces[i].si_override = malloc(sizeof(shaderInfo_t));
                memcpy(localSurfaces[i].si_override, ShaderInfoForShader(dshaders[ds->shaderNum].shader), sizeof(shaderInfo_t));

                if (extra[i].lightValue >= 0.0f) {
                    localSurfaces[i].si_override->value = extra[i].lightValue;
                }
                if (extra[i].backsplashFraction >= 0.0f) {
                    localSurfaces[i].si_override->backsplashFraction = extra[i].backsplashFraction;
                }
                if (extra[i].lightSubdivide >= 0.0f) {
                    localSurfaces[i].si_override->lightSubdivide = extra[i].lightSubdivide;
                }
                if (extra[i].lightColor[0] >= 0.0f && extra[i].lightColor[1] >= 0.0f && extra[i].lightColor[2] >= 0.0f) {
                    VectorCopy(extra[i].lightColor, localSurfaces[i].si_override->color);
                }
            }
        }

        // Resolve vertex color override: shader is the base, entity key wins.
        {
            shaderInfo_t *si = localSurfaces[i].si_override ? localSurfaces[i].si_override : ShaderInfoForShader(dshaders[ds->shaderNum].shader);
            if (si && si->hasVertexColor)
            {
                localSurfaces[i].hasVertexColor = qtrue;
                VectorCopy(si->vertexColor, localSurfaces[i].vertexColor);
            }
            else
            {
                localSurfaces[i].hasVertexColor = qfalse;
                VectorClear(localSurfaces[i].vertexColor);
            }

            // Entity-level 'vertexcolor' key (func_group / misc_model) overrides the shader.
            if (extra && i < numExtra && extra[i].hasVertexColor)
            {
                localSurfaces[i].hasVertexColor = qtrue;
                VectorCopy(extra[i].vertexColor, localSurfaces[i].vertexColor);
            }
        }
    }
    if (extra) free(extra);
}

/*
==========================
VisualizeLightmapAllocation
==========================
*/
#define RASTERIZE_PLANAR_UVS 0

static void RasterizeTriangleToMask(dsurface_t *ds, float st0[2], float st1[2], float st2[2])
{
    float minX = st0[0];
    if (st1[0] < minX)
        minX = st1[0];
    if (st2[0] < minX)
        minX = st2[0];
    float maxX = st0[0];
    if (st1[0] > maxX)
        maxX = st1[0];
    if (st2[0] > maxX)
        maxX = st2[0];
    float minY = st0[1];
    if (st1[1] < minY)
        minY = st1[1];
    if (st2[1] < minY)
        minY = st2[1];
    float maxY = st0[1];
    if (st1[1] > maxY)
        maxY = st1[1];
    if (st2[1] > maxY)
        maxY = st2[1];

    int startX = (int)floor(minX) - ds->lightmapOffset[0][0];
    int endX = (int)ceil(maxX) - ds->lightmapOffset[0][0];
    int startY = (int)floor(minY) - ds->lightmapOffset[0][1];
    int endY = (int)ceil(maxY) - ds->lightmapOffset[0][1];

    if (startX < 0)
        startX = 0;
    if (endX >= ds->lightmapWidth)
        endX = ds->lightmapWidth - 1;
    if (startY < 0)
        startY = 0;
    if (endY >= ds->lightmapHeight)
        endY = ds->lightmapHeight - 1;

    if (startX > endX || startY > endY)
        return;

    // Edge functions for fast incremental rasterization
    float dx01 = st0[0] - st1[0];
    float dy01 = st0[1] - st1[1];
    float dx12 = st1[0] - st2[0];
    float dy12 = st1[1] - st2[1];
    float dx20 = st2[0] - st0[0];
    float dy20 = st2[1] - st0[1];

    // Determine winding order
    float area = dx01 * dy20 - dy01 * dx20;
    if (area == 0.0f)
        return; // Degenerate triangle

    // Flip edges if winding is negative so "inside" is always positive
    if (area < 0.0f)
    {
        dx01 = -dx01;
        dy01 = -dy01;
        dx12 = -dx12;
        dy12 = -dy12;
        dx20 = -dx20;
        dy20 = -dy20;
    }

    // Starting point (pixel center)
    float px0 = (float)ds->lightmapOffset[0][0] + (float)startX + 0.5f;
    float py0 = (float)ds->lightmapOffset[0][1] + (float)startY + 0.5f;

    // Initial edge values at (startX, startY)
    float e0_row = (px0 - st0[0]) * dy01 - (py0 - st0[1]) * dx01;
    float e1_row = (px0 - st1[0]) * dy12 - (py0 - st1[1]) * dx12;
    float e2_row = (px0 - st2[0]) * dy20 - (py0 - st2[1]) * dx20;

    for (int y = startY; y <= endY; y++)
    {
        float e0 = e0_row;
        float e1 = e1_row;
        float e2 = e2_row;

        int p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + startX;

        for (int x = startX; x <= endX; x++)
        {
            // Epsilon tolerance for edge coverage
            if (e0 >= -0.001f && e1 >= -0.001f && e2 >= -0.001f)
            {
                lightAlphaMask[p] = ds->surfaceType;
            }
            // Step X
            e0 += dy01;
            e1 += dy12;
            e2 += dy20;
            p++;
        }

        // Step Y
        e0_row -= dx01;
        e1_row -= dx12;
        e2_row -= dx20;
    }
}

void GenerateLightmapAlphaMask(void)
{
    int i, x, y, p;
    dsurface_t *ds;

    if (!lightAlphaMask)
        return;

    _printf("--- GenerateLightmapAlphaMask ---\n");

    for (i = 0; i < numDrawSurfaces; i++)
    {
        ds = &drawSurfaces[i];
        if (ds->lightmapNum[0] < 0)
            continue;

        if (ds->surfaceType == MST_PATCH)
        {
            mesh_t *mesh = localSurfaces[i].patchMesh;
            if (!mesh)
                continue;

            for (int my = 0; my < mesh->height - 1; my++)
            {
                for (int mx = 0; mx < mesh->width - 1; mx++)
                {
                    drawVert_t *v00 = &mesh->verts[my * mesh->width + mx];
                    drawVert_t *v10 = &mesh->verts[my * mesh->width + mx + 1];
                    drawVert_t *v01 = &mesh->verts[(my + 1) * mesh->width + mx];
                    drawVert_t *v11 = &mesh->verts[(my + 1) * mesh->width + mx + 1];
                    float st00[2] = {v00->lightmap[0][0] * LIGHTMAP_WIDTH, v00->lightmap[0][1] * LIGHTMAP_HEIGHT};
                    float st10[2] = {v10->lightmap[0][0] * LIGHTMAP_WIDTH, v10->lightmap[0][1] * LIGHTMAP_HEIGHT};
                    float st01[2] = {v01->lightmap[0][0] * LIGHTMAP_WIDTH, v01->lightmap[0][1] * LIGHTMAP_HEIGHT};
                    float st11[2] = {v11->lightmap[0][0] * LIGHTMAP_WIDTH, v11->lightmap[0][1] * LIGHTMAP_HEIGHT};

                    RasterizeTriangleToMask(ds, st00, st10, st11);
                    RasterizeTriangleToMask(ds, st00, st11, st01);
                }
            }
        }
        else if (ds->surfaceType == MST_TRIANGLE_SOUP || (ds->surfaceType == MST_PLANAR && RASTERIZE_PLANAR_UVS))
        {
            for (int j = 0; j < ds->numIndexes; j += 3)
            {
                drawVert_t *v0 = &drawVerts[ds->firstVert + drawIndexes[ds->firstIndex + j]];
                drawVert_t *v1 = &drawVerts[ds->firstVert + drawIndexes[ds->firstIndex + j + 1]];
                drawVert_t *v2 = &drawVerts[ds->firstVert + drawIndexes[ds->firstIndex + j + 2]];
                float st0[2] = {v0->lightmap[0][0] * LIGHTMAP_WIDTH, v0->lightmap[0][1] * LIGHTMAP_HEIGHT};
                float st1[2] = {v1->lightmap[0][0] * LIGHTMAP_WIDTH, v1->lightmap[0][1] * LIGHTMAP_HEIGHT};
                float st2[2] = {v2->lightmap[0][0] * LIGHTMAP_WIDTH, v2->lightmap[0][1] * LIGHTMAP_HEIGHT};
                RasterizeTriangleToMask(ds, st0, st1, st2);
            }
        }
        else
        {
            // Planar faces (with RASTERIZE_PLANAR_UVS == 0) and others: fill the bounding box
            for (y = 0; y < ds->lightmapHeight; y++)
            {
                for (x = 0; x < ds->lightmapWidth; x++)
                {
                    p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH +
                        (ds->lightmapOffset[0][0] + x);
                    lightAlphaMask[p] = ds->surfaceType;
                }
            }
        }
    }
}

/*
==========================
ExportAlphaMask
==========================
*/
void ExportAlphaMask(const char *filenamePrefix)
{
    int i, x, y, p, numPages;
    char filename[1024];
    dsurface_t *ds;
    int k;
    byte color[3];

    if (!lightAlphaMask)
        return;

    _printf("--- ExportAlphaMask (%s) ---\n", filenamePrefix);
    memset(lightBytes, 24, numLightBytes);

    for (i = 0; i < numDrawSurfaces; i++)
    {
        ds = &drawSurfaces[i];
        if (ds->lightmapNum[0] < 0)
            continue;

        color[0] = (i * 123) % 200 + 55;
        color[1] = (i * 456) % 200 + 55;
        color[2] = (i * 789) % 200 + 55;

        int scale = upscale ? 2 : 1;
        int currentGutter = upscale ? (GUTTER * 2) : 0;
        int extW = ds->lightmapWidth * scale + currentGutter * 2;
        int extH = ds->lightmapHeight * scale + currentGutter * 2;

        for (y = 0; y < extH; y++)
        {
            for (x = 0; x < extW; x++)
            {
                int px = ds->lightmapOffset[0][0] + (x - currentGutter) / scale;
                int py = ds->lightmapOffset[0][1] + (y - currentGutter) / scale;

                if (px >= 0 && px < LIGHTMAP_WIDTH && py >= 0 && py < LIGHTMAP_HEIGHT)
                {
                    p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + py) * LIGHTMAP_WIDTH + px;
                    if (lightAlphaMask[p])
                    {
                        k = p * 3;
                        lightBytes[k] = color[0];
                        lightBytes[k + 1] = color[1];
                        lightBytes[k + 2] = color[2];
                    }
                }
            }
        }
    }

    numPages = 0;
    for (i = 0; i < numDrawSurfaces; i++)
    {
        if (drawSurfaces[i].lightmapNum[0] > numPages)
            numPages = drawSurfaces[i].lightmapNum[0];
    }
    numPages++;

    for (i = 0; i < numPages; i++)
    {
        sprintf(filename, "%s%d.bmp", filenamePrefix, i);
        _printf("    Writing %s...\n", filename);
        SaveBMP(filename, &lightBytes[i * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3], LIGHTMAP_WIDTH, LIGHTMAP_HEIGHT, 3);
    }
}

/*
========
LightMain

========
*/
void LightMain(void)
{
    float f;

    _printf("--- LightMain ---\n");

    // find the optional world ambient
    const char *_color = ValueForKey(&entities[0], "_color");
    if (_color[0])
    {
        ParseColor(_color, ambientColor);
    }
    else
    {
        VectorSet(ambientColor, 1.0f, 1.0f, 1.0f);
    }

    f = FloatForKey(&entities[0], "ambient");
    VectorScale(ambientColor, f, ambientColor);

    FindSkyBrushes();
    SetEntityOrigins();

    // Validate the lightmaps
    {
        int count, numSamples;
        int i, j;
        dsurface_t *ds;

        _printf("--- CountLightmaps ---\n");
        count = -1;
        numSamples = 0;
        for (i = 0; i < numDrawSurfaces; i++)
        {
            ds = &drawSurfaces[i];
            for (j = 0; j < 4; j++)
            {
                if (ds->lightmapNum[j] > count)
                {
                    count = ds->lightmapNum[j];
                }
            }
            if (ds->lightmapNum[0] >= 0)
            {
                numSamples += ds->lightmapWidth * ds->lightmapHeight;
            }
        }

        count++;
        numLightBytes = count * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3;

        _printf("%5i drawSurfaces\n", numDrawSurfaces);
        _printf("%5i lightmaps\n", count);
        _printf("%5i lightmap samples\n", numSamples);
    }

    // Generate the base geometric mask
    GenerateLightmapAlphaMask();

    if (debugLightmaps)
    {
        ExportAlphaMask("vis_lm_");
        return;
    }

    // Initialize the grid
    {
        int i;
        vec3_t maxs;

        const char *value = ValueForKey(&entities[0], "gridsize");
        if (strlen(value))
        {
            sscanf(value, "%f %f %f", &gridSize[0], &gridSize[1], &gridSize[2]);
            _printf("grid size = {%1.1f, %1.1f, %1.1f}\n", gridSize[0], gridSize[1],
                    gridSize[2]);
        }

        for (i = 0; i < 3; i++)
        {
            gridMins[i] = gridSize[i] * ceil(dmodels[0].mins[i] / gridSize[i]);
            maxs[i] = gridSize[i] * floor(dmodels[0].maxs[i] / gridSize[i]);
            gridBounds[i] = (maxs[i] - gridMins[i]) / gridSize[i] + 1;
        }

        numGridPoints = gridBounds[0] * gridBounds[1] * gridBounds[2];
        CheckGridData32();
        if (numGridPoints * sizeof(bspGridPoint_t) >= MAX_MAP_LIGHTGRID)
            Error("MAX_MAP_LIGHTGRID");
        qprintf("%5i gridPoints\n", numGridPoints);
    }

    // create lights out of patches and lights
    _printf("--- CreateLights ---\n");
    CreateEntityLights();
    CreateSurfaceLights();

    // count total lights
    numLights = 0;
    for (light_t *l = lights; l; l = l->next)
    {
        numLights++;
    }
    _printf("%i point lights\n", numPointLights);
    _printf("%i area lights\n", numAreaLights);

    InitTrace();
    LightWorld();

    // Call radiosity passes
    LightRadiosity();

    if (debugLightmapsAlpha)
    {
        ExportAlphaMask("vis_alpha_");
    }

    DilateDeluxeDirections();

    PostProcessLightmaps();

    for (int i = 0; i < numDrawSurfaces; i++)
    {
        if (localSurfaces[i].patchMesh)
            FreeMesh(localSurfaces[i].patchMesh);
        if (localSurfaces[i].si_override)
            free(localSurfaces[i].si_override);
    }
}
