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
#include "light.h"
#include <embree4/rtcore.h>

RTCDevice g_device = NULL;
RTCScene g_scene = NULL;

static void AddBrushesToEmbree(RTCScene scene);
void AlphaFilter(const struct RTCFilterFunctionNArguments *args);

#define TRACE_EPSILON 0.001

int c_totalTrace;

/*
====================
InitTracingGeometry

Builds the Embree scene for ray tracing against surfaces and brushes
====================
*/
void InitTracingGeometry(void)
{
    int i, j;
    dsurface_t *dsurf;

    // Embree 4 initialization
    g_device = rtcNewDevice(NULL);
    if (!g_device)
    {
        Error("Embree: Failed to create device\n");
    }
    g_scene = rtcNewScene(g_device);
    rtcSetSceneFlags(g_scene, RTC_SCENE_FLAG_ROBUST);

    _printf("--- InitTracingGeometry: Embree 4.x ---\n");

    // Add brushes to Embree for solid occlusion
    AddBrushesToEmbree(g_scene);

    int count = 0;
    for (i = 0; i < numDrawSurfaces; i++)
    {
        dsurf = &drawSurfaces[i];

        // don't make surfaces for transparent objects
        // because we want light to pass through them
        shaderInfo_t *si = ShaderInfoForShader(dshaders[dsurf->shaderNum].shader);
        if ((si->contents & CONTENTS_TRANSLUCENT) &&
            !(si->surfaceFlags & (SURF_ALPHASHADOW | SURF_LIGHTFILTER)))
        {
            continue;
        }
        
        // do not add sky patches/triangles to Embree, sky never occludes!
        if (si->surfaceFlags & SURF_SKY)
        {
            continue;
        }

        if (dsurf->numIndexes > 0 &&
            (dsurf->surfaceType == MST_TRIANGLE_SOUP ||
             dsurf->surfaceType == MST_PLANAR))
        {

            RTCGeometry geom = rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE);
            rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_HIGH);

            rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0,
                                       RTC_FORMAT_FLOAT3, drawVerts, 0,
                                       sizeof(drawVert_t), numDrawVerts);

            unsigned int *indices = rtcSetNewGeometryBuffer(
                geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                3 * sizeof(unsigned int), dsurf->numIndexes / 3);
            for (j = 0; j < dsurf->numIndexes; j++)
            {
                indices[j] = (unsigned int)(drawIndexes[dsurf->firstIndex + j] +
                                            dsurf->firstVert);
            }

            qboolean needsFilter = qfalse;
            if (si->surfaceFlags & (SURF_ALPHASHADOW | SURF_LIGHTFILTER)) needsFilter = qtrue;
            if (!localSurfaces[i].castShadows) needsFilter = qtrue;

            if (needsFilter) {
                rtcSetGeometryIntersectFilterFunction(geom, AlphaFilter);
                rtcSetGeometryOccludedFilterFunction(geom, AlphaFilter);
            }
            rtcCommitGeometry(geom);
            rtcAttachGeometryByID(g_scene, geom, i);
            rtcReleaseGeometry(geom);
            count++;
        }
        else if (dsurf->surfaceType == MST_PATCH)
        {
            float ssize = game->defaultSampleSize;
            if (si->lightmapSampleSize)
                ssize = si->lightmapSampleSize;
            mesh_t *subdivided = SubdividePatchForLighting(dsurf, ssize);

            RTCGeometry geom = rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE);
            rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_HIGH);

            int numVerts = subdivided->width * subdivided->height;
            void *verts = rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0,
                                                  RTC_FORMAT_FLOAT3,
                                                  sizeof(drawVert_t), numVerts);
            memcpy(verts, subdivided->verts, sizeof(drawVert_t) * numVerts);

            int numTris = (subdivided->width - 1) * (subdivided->height - 1) * 2;
            unsigned int *indices = rtcSetNewGeometryBuffer(
                geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                3 * sizeof(unsigned int), numTris);

            int triCount = 0;
            for (int x = 0; x < subdivided->width - 1; x++)
            {
                for (int y = 0; y < subdivided->height - 1; y++)
                {
                    int v1 = y * subdivided->width + x;
                    int v2 = v1 + 1;
                    int v3 = v1 + subdivided->width + 1;
                    int v4 = v1 + subdivided->width;

                    indices[triCount * 3 + 0] = v1;
                    indices[triCount * 3 + 1] = v4;
                    indices[triCount * 3 + 2] = v3;
                    triCount++;

                    indices[triCount * 3 + 0] = v1;
                    indices[triCount * 3 + 1] = v3;
                    indices[triCount * 3 + 2] = v2;
                    triCount++;
                }
            }

            FreeMesh(subdivided);

            qboolean needsFilter = qfalse;
            if (si->surfaceFlags & (SURF_ALPHASHADOW | SURF_LIGHTFILTER)) needsFilter = qtrue;
            if (!localSurfaces[i].castShadows) needsFilter = qtrue;

            if (needsFilter) {
                rtcSetGeometryIntersectFilterFunction(geom, AlphaFilter);
                rtcSetGeometryOccludedFilterFunction(geom, AlphaFilter);
            }
            rtcCommitGeometry(geom);
            rtcAttachGeometryByID(g_scene, geom, i);
            rtcReleaseGeometry(geom);
            count++;
        }
    }

    rtcCommitScene(g_scene);
    _printf("%d surfaces added to Embree scene\n", count);
}

/*
================
Trace_SampleFilter

Returns qtrue if the ray is fully blocked (absolute opacity).
Returns qfalse if the ray passes through (transparent or tinted).
Multiplies 'filter' by the sampled texture color.
================
*/
qboolean Trace_SampleFilter(shaderInfo_t *si, float s, float t, vec3_t filter, qboolean isLightFilter)
{
    int x, y;
    byte *pixel;
    byte alpha;

    if (!si || !si->pixels)
    {
        VectorClear(filter);
        return qtrue; // Solid
    }

    // 1. Wrap UVs
    s = s - floor(s);
    t = 1.0f - (t - floor(t)); // Flip T for standard texture orientation

    x = s * si->width;
    y = t * si->height;
    if (x < 0)
        x = 0;
    else if (x >= si->width)
        x = si->width - 1;
    if (y < 0)
        y = 0;
    else if (y >= si->height)
        y = si->height - 1;

    pixel = si->pixels + 4 * (y * si->width + x);
    alpha = pixel[3];

    // 2. Apply Alpha Threshold (Proposed 80% transparency rule)
    // If it's more than 80% transparent (alpha < 51), it's a hole.
    if (alpha < 51)
    {
        return qfalse; // Ray passes through unfiltered
    }

    // 3. Apply Tinting
    // Multiply cumulative filter by normalized texture RGB
    if (isLightFilter)
    {
        filter[0] *= (float)pixel[0] / 255.0f;
        filter[1] *= (float)pixel[1] / 255.0f;
        filter[2] *= (float)pixel[2] / 255.0f;
    }

    // 4. Determine if it blocks entirely
    // If alpha is high (> 250), we consider it fully opaque for occlusion.
    if (alpha > 250)
    {
        return qtrue; // Blocks ray
    }

    return qfalse; // Continues ray (tinted if lightfilter)
}

/*
=============
AlphaFilter

Embree intersection filter for handling ignoreSurface and alpha shadows
=============
*/
void AlphaFilter(const struct RTCFilterFunctionNArguments *args)
{
    if (args->valid[0] != -1)
        return;
    if (!args->context)
        return; // No context provided, skip advanced filtering
    struct MyRayQueryContext *mcontext = (struct MyRayQueryContext *)args->context;
    traceWork_t *tw = mcontext->tw;
    struct RTCHit *hit = (struct RTCHit *)args->hit;
    unsigned int geomID = hit->geomID;
    unsigned int primID = hit->primID;

    // Only perform additional checks for draw surfaces
    if (geomID < (unsigned int)numDrawSurfaces)
    {
        dsurface_t *ds = &drawSurfaces[geomID];

        // Respect patchshadows setting
        if (!mcontext->patchshadows && ds->surfaceType == MST_PATCH)
        {
            args->valid[0] = 0;
            return;
        }

        // Only ignore the surface if it's truly planar (can't shadow itself).
        // For patches and triangle soups (models), we want them to cast shadows on themselves.
        if (tw && tw->ignoreSurface != -1 && geomID == (unsigned int)tw->ignoreSurface)
        {
            if (ds->surfaceType == MST_PLANAR)
            {
                args->valid[0] = 0;
                return;
            }
        }

        // If the surface has castShadows disabled, it ONLY casts shadows on itself.
        if (!localSurfaces[geomID].castShadows)
        {
            if (!tw || tw->ignoreSurface == -1 || geomID != (unsigned int)tw->ignoreSurface)
            {
                args->valid[0] = 0;
                return;
            }
        }

        shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

        if (si->surfaceFlags & (SURF_ALPHASHADOW | SURF_LIGHTFILTER))
        {
            float u = hit->u;
            float v = hit->v;
            float s, t;

            if (ds->surfaceType == MST_TRIANGLE_SOUP || ds->surfaceType == MST_PLANAR)
            {
                // Standard draw surface vertices
                drawVert_t *v0 = &drawVerts[drawIndexes[ds->firstIndex + primID * 3 + 0] + ds->firstVert];
                drawVert_t *v1 = &drawVerts[drawIndexes[ds->firstIndex + primID * 3 + 1] + ds->firstVert];
                drawVert_t *v2 = &drawVerts[drawIndexes[ds->firstIndex + primID * 3 + 2] + ds->firstVert];

                s = (1.0f - u - v) * v0->st[0] + u * v1->st[0] + v * v2->st[0];
                t = (1.0f - u - v) * v0->st[1] + u * v1->st[1] + v * v2->st[1];
            }
            else if (ds->surfaceType == MST_PATCH)
            {
                // Patches have their vertices copied into the Embree buffer during InitTracingGeometry.
                RTCGeometry geom = rtcGetGeometry(g_scene, geomID);
                drawVert_t *verts = (drawVert_t *)rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_VERTEX, 0);
                unsigned int *indices = (unsigned int *)rtcGetGeometryBufferData(geom, RTC_BUFFER_TYPE_INDEX, 0);

                drawVert_t *v0 = &verts[indices[primID * 3 + 0]];
                drawVert_t *v1 = &verts[indices[primID * 3 + 1]];
                drawVert_t *v2 = &verts[indices[primID * 3 + 2]];

                s = (1.0f - u - v) * v0->st[0] + u * v1->st[0] + v * v2->st[0];
                t = (1.0f - u - v) * v0->st[1] + u * v1->st[1] + v * v2->st[1];
            }
            else
            {
                return; // Unknown surface type
            }

            // Sample the filter
            vec3_t dummyFilter = {1.0f, 1.0f, 1.0f};
            float *filterPtr = (tw && tw->trace) ? tw->trace->filter : dummyFilter;

            if (Trace_SampleFilter(si, s, t, filterPtr, (si->surfaceFlags & SURF_LIGHTFILTER) != 0))
            {
                // Opaque hit - keep the valid flag (blocks)
            }
            else
            {
                // Transparent/Tinted hit - tell Embree to ignore and continue
                args->valid[0] = 0;
            }
        }
    }
}

/*
=============
AddBrushesToEmbree

Experimental: Converts all brushes to triangles and adds them to the Embree scene.
=============
*/
static void AddBrushesToEmbree(RTCScene scene)
{
    int i, j, k;
    dbrush_t *b;
    dbrushside_t *s;
    dplane_t *p, *p2;
    winding_t *w;
    int brushTriangles = 0;

    _printf("--- AddBrushesToEmbree: Converting brushes to triangles ---\n");

    for (i = 0; i < numbrushes; i++)
    {
        if (!brushCastsShadow[i]) {
            continue;
        }

        b = &dbrushes[i];

        qboolean skipBrush = qfalse;
        for (j = 0; j < b->numSides; j++)
        {
            s = &dbrushsides[b->firstSide + j];
            if ((dshaders[s->shaderNum].contentFlags &
                 (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER |
                  CONTENTS_TRANSLUCENT | CONTENTS_FOG |
                  CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP | CONTENTS_BOTCLIP)) ||
                (dshaders[s->shaderNum].surfaceFlags & SURF_SKY))
            {
                skipBrush = qtrue;
                break;
            }
        }
        if (skipBrush)
        {
            continue;
        }

        for (j = 0; j < b->numSides; j++)
        {
            s = &dbrushsides[b->firstSide + j];
            p = &dplanes[s->planeNum];

            w = BaseWindingForPlane(p->normal, p->dist);
            for (k = 0; k < b->numSides && w; k++)
            {
                if (k == j)
                {
                    continue;
                }
                p2 = &dplanes[dbrushsides[b->firstSide + k].planeNum ^ 1];
                ChopWindingInPlace(&w, p2->normal, p2->dist, 0.0f);
            }

            if (!w)
            {
                continue;
            }

            if (w->numpoints < 3)
            {
                FreeWinding(w);
                continue;
            }

            RTCGeometry geom = rtcNewGeometry(g_device, RTC_GEOMETRY_TYPE_TRIANGLE);
            rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_HIGH);

            vec3_t *verts = (vec3_t *)rtcSetNewGeometryBuffer(
                geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(vec3_t),
                w->numpoints);
            int *indices = (int *)rtcSetNewGeometryBuffer(
                geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(int) * 3,
                w->numpoints - 2);

            for (k = 0; k < w->numpoints; k++)
            {
                VectorCopy(w->points[k], verts[k]);
            }

            for (k = 0; k < w->numpoints - 2; k++)
            {
                indices[k * 3 + 0] = 0;
                indices[k * 3 + 1] = k + 1;
                indices[k * 3 + 2] = k + 2;
            }

            rtcCommitGeometry(geom);
            rtcAttachGeometryByID(scene, geom, numDrawSurfaces + brushTriangles);
            rtcReleaseGeometry(geom);

            brushTriangles++;
            FreeWinding(w);
        }
    }
    _printf("Added %i brush sides as geometry to Embree\n", brushTriangles);
}

/*
==============
InitTrace

Loads the tracing geometry structure
=============
*/
void InitTrace(void)
{
    InitTracingGeometry();
}

qboolean BoxInOpaqueDetail(vec3_t start, float margin);

/*
===================
BoxOnPlaneSide
===================
*/
int BoxOnPlaneSide(vec3_t emins, vec3_t emaxs, dplane_t *p)
{
    float dist1, dist2;
    float bpts[3], wpts[3];
    int i;

    for (i = 0; i < 3; i++)
    {
        if (p->normal[i] >= 0)
        {
            bpts[i] = emaxs[i];
            wpts[i] = emins[i];
        }
        else
        {
            bpts[i] = emins[i];
            wpts[i] = emaxs[i];
        }
    }

    dist1 = DotProduct(bpts, p->normal) - p->dist;
    dist2 = DotProduct(wpts, p->normal) - p->dist;

    int sides = 0;
    if (dist1 >= 0) sides = 1;
    if (dist2 < 0) sides |= 2;

    return sides;
}

/*
===================
BoxInSolid_r
===================
*/
qboolean BoxInSolid_r(vec3_t mins, vec3_t maxs, int node)
{
    while (node >= 0)
    {
        dnode_t *dnode = &dnodes[node];
        dplane_t *dplane = &dplanes[dnode->planeNum];
        int sides = BoxOnPlaneSide(mins, maxs, dplane);

        if (sides == 3)
        {
            // Box crosses plane, must be engulfed in BOTH children to be completely solid
            return BoxInSolid_r(mins, maxs, dnode->children[0]) &&
                   BoxInSolid_r(mins, maxs, dnode->children[1]);
        }
        if (sides == 1)
            node = dnode->children[0];
        else
            node = dnode->children[1];
    }

    int leafNum = -node - 1;
    if (dleafs[leafNum].cluster == -1)
    {
        return qtrue; // Solid leaf (including the void)
    }
    return qfalse; // Playable leaf
}

/*
===================
BoxInSolid
===================
*/
qboolean BoxInSolid(vec3_t origin, float margin, qboolean structuralonly)
{
    vec3_t mins, maxs;
    mins[0] = origin[0] - margin;
    mins[1] = origin[1] - margin;
    mins[2] = origin[2] - margin;
    maxs[0] = origin[0] + margin;
    maxs[1] = origin[1] + margin;
    maxs[2] = origin[2] + margin;

    // Check structural BSP bounds
    if (BoxInSolid_r(mins, maxs, 0))
    {
        return qtrue;
    }

    if (structuralonly)
    {
        return qfalse;
    }

    // Check detail brushes volumetrically
    return BoxInOpaqueDetail(origin, margin);
}

/*
===================
PointInSolid
===================
*/
qboolean PointInSolid_r(vec3_t start, int node)
{
    dnode_t *dnode;
    dplane_t *dplane;
    double front;

    while (node >= 0)
    {
        dnode = &dnodes[node];
        dplane = &dplanes[dnode->planeNum];

        int type = PlaneTypeForNormal(dplane->normal);
        if (type <= PLANE_Z)
        {
            front = (double)start[type] - dplane->dist;
        }
        else
        {
            front = ((double)start[0] * dplane->normal[0] +
                     (double)start[1] * dplane->normal[1] +
                     (double)start[2] * dplane->normal[2]) -
                    dplane->dist;
        }

        if (front > -TRACE_EPSILON && front < TRACE_EPSILON)
        {
            // exactly on node, must check both sides
            return (qboolean)(PointInSolid_r(start, dnode->children[0]) |
                              PointInSolid_r(start, dnode->children[1]));
        }

        if (front >= TRACE_EPSILON)
        {
            node = dnode->children[0];
        }
        else
        {
            node = dnode->children[1];
        }
    }

    // Handle leaf
    int leafNum = -node - 1;
    if (dleafs[leafNum].cluster == -1)
    {
        return qtrue; // Opaque cluster is solid
    }
    return qfalse;
}

/*
===================
PointInLeafNum
===================
*/
int PointInLeafNum(vec3_t start)
{
    int node = 0;
    while (node >= 0)
    {
        dnode_t *dnode = &dnodes[node];
        dplane_t *dplane = &dplanes[dnode->planeNum];
        double front;

        int type = PlaneTypeForNormal(dplane->normal);
        if (type <= PLANE_Z)
        {
            front = (double)start[type] - dplane->dist;
        }
        else
        {
            front = ((double)start[0] * dplane->normal[0] +
                     (double)start[1] * dplane->normal[1] +
                     (double)start[2] * dplane->normal[2]) -
                    dplane->dist;
        }

        if (front >= 0)
        {
            node = dnode->children[0];
        }
        else
        {
            node = dnode->children[1];
        }
    }

    return -node - 1;
}

/*
===================
BoxInOpaqueDetail
===================
*/
qboolean BoxInOpaqueDetail(vec3_t start, float margin)
{
    int leafNum = PointInLeafNum(start);
    int i, j;
    
    // Check all brushes in this leaf
    for (i = 0; i < dleafs[leafNum].numLeafBrushes; i++)
    {
        int brushNum = dleafbrushes[dleafs[leafNum].firstLeafBrush + i];
        
        // Only cull if it's an opaque brush
        if (!brushCastsShadow[brushNum])
            continue;
            
        dbrush_t *b = &dbrushes[brushNum];
        dshader_t *ds = &dshaders[b->shaderNum];
        
        if (b->numSides == 0)
            continue;
            
        // Ignore brushes that don't block light (translucent, liquids, clips, fog, sky)
        // Note: We deliberately do NOT ignore SURF_NODRAW (caulk), because caulk is solid and blocks light.
        if ((ds->contentFlags & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER |
                                 CONTENTS_TRANSLUCENT | CONTENTS_FOG |
                                 CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP | CONTENTS_BOTCLIP)) ||
            (ds->surfaceFlags & SURF_SKY))
        {
            continue;
        }
            
        qboolean inBrush = qtrue;
        
        for (j = 0; j < b->numSides; j++)
        {
            dbrushside_t *s = &dbrushsides[b->firstSide + j];
            dplane_t *plane = &dplanes[s->planeNum];
            float d = DotProduct(start, plane->normal) - plane->dist;
            
            // Allow coplanarity with a small epsilon smaller than 0.1f (light extrusion)
            if (d > -margin)
            {
                inBrush = qfalse;
                break;
            }
        }
        
        if (inBrush)
        {
            return qtrue; // Inside an opaque detail (or structural) brush
        }
    }
    
    return qfalse;
}

/*
=============
PointInBrush
=============
*/
qboolean PointInBrush(vec3_t start) 
{ 
    return PointInSolid_r(start, 0) || BoxInOpaqueDetail(start, 0.05f); 
}

/*
===================
PointInTrisoup
===================
*/
qboolean PointInTrisoup(vec3_t origin, vec3_t normal)
{
    if (g_scene == NULL)
        return qfalse;

    struct RTCRayHit rayhit;
    memset(&rayhit, 0, sizeof(rayhit));
    struct RTCIntersectArguments iargs;
    struct MyRayQueryContext context;
    rtcInitRayQueryContext(&context.context);
    context.tw = NULL;
    context.patchshadows = patchshadows;

    rayhit.ray.org_x = origin[0];
    rayhit.ray.org_y = origin[1];
    rayhit.ray.org_z = origin[2];
    rayhit.ray.dir_x = normal[0];
    rayhit.ray.dir_y = normal[1];
    rayhit.ray.dir_z = normal[2];
    rayhit.ray.tnear = 0.0001f;
    rayhit.ray.tfar = 10000.0f; // Cast far enough to hit the enclosing hull
    rayhit.ray.mask = 0xFFFFFFFF;
    rayhit.ray.flags = 0;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

    rtcInitIntersectArguments(&iargs);
    iargs.context = &context.context;
    rtcIntersect1(g_scene, &rayhit, &iargs);

    if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID && rayhit.hit.geomID < (unsigned int)numDrawSurfaces)
    {
        dsurface_t *ds = &drawSurfaces[rayhit.hit.geomID];
        if (ds->surfaceType == MST_TRIANGLE_SOUP)
        {
            if (localSurfaces && localSurfaces[rayhit.hit.geomID].surfaceIsPlanar)
            {
                return qfalse;
            }

            shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
            if (si)
            {
                if ((si->surfaceFlags & (SURF_NODRAW | SURF_SKY | SURF_NONSOLID)) ||
                    (si->contents & (CONTENTS_TRANSLUCENT | CONTENTS_FOG | CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA)))
                {
                    return qfalse; // Skip positive returns for transparent, liquid, or non-solid materials
                }
            }

            // Check if we hit the backface (inside of the Trisoup looking out)
            float dot = rayhit.ray.dir_x * rayhit.hit.Ng_x + rayhit.ray.dir_y * rayhit.hit.Ng_y + rayhit.ray.dir_z * rayhit.hit.Ng_z;
            if (dot > 0.0f)
            {
                return qtrue; // We are inside a closed Trisoup
            }
        }
    }

    return qfalse;
}

/*
=============
PointInSolid
=============
*/
qboolean PointInSolid(vec3_t start)
{
    if (PointInBrush(start))
        return qtrue;

    vec3_t dirs[3] = {
        {0, 0, 1},
        {1, 0, 0},
        {0, 1, 0}
    };
    
    int i;
    for (i = 0; i < 3; i++)
    {
        if (!PointInTrisoup(start, dirs[i]))
        {
            return qfalse;
        }
    }

    return qtrue;
}

/*
=============
TraceLine_Embree

High-performance Embree tracing path
=============
*/
static void TraceLine_Embree(const vec3_t start, const vec3_t stop,
                             trace_t *trace, qboolean testAll, traceWork_t *tw)
{
    int i;
    struct RTCRayHit rayhit;
    memset(&rayhit, 0, sizeof(rayhit));
    struct MyRayQueryContext context;
    rtcInitRayQueryContext(&context.context);
    context.tw = tw;
    context.patchshadows = tw ? tw->patchshadows : patchshadows;

    if (tw) {
        tw->trace = trace;
    }

    trace->filter[0] = 1.0f;
    trace->filter[1] = 1.0f;
    trace->filter[2] = 1.0f;

    vec3_t dir;
    float length;

    VectorSubtract(stop, start, dir);
    length = VectorLength(dir);
    if (length < 0.0001f)
    {
        trace->hitFraction = 1.0f;
        VectorCopy(start, trace->hit);
        return;
    }
    VectorScale(dir, 1.0f / length, dir);

    rayhit.ray.org_x = start[0];
    rayhit.ray.org_y = start[1];
    rayhit.ray.org_z = start[2];
    rayhit.ray.dir_x = dir[0];
    rayhit.ray.dir_y = dir[1];
    rayhit.ray.dir_z = dir[2];
    rayhit.ray.tnear = 0.0001f;
    rayhit.ray.tfar = length;
    rayhit.ray.mask = 0xFFFFFFFF;
    rayhit.ray.flags = 0;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

    if (testAll)
    {
        struct RTCIntersectArguments iargs;
        rtcInitIntersectArguments(&iargs);
        iargs.context = &context.context;
        rtcIntersect1(g_scene, &rayhit, &iargs);
    }
    else
    {
        // Optimization for simple occlusion checks
        struct RTCOccludedArguments oargs;
        rtcInitOccludedArguments(&oargs);
        oargs.context = &context.context;
        rtcOccluded1(g_scene, &rayhit.ray, &oargs);

        // If occluded, tfar becomes -infinity in Embree 4
        if (rayhit.ray.tfar < 0)
        {
            rayhit.hit.geomID = 0; // Mark as hit
            rayhit.ray.tfar = 0.0f;
        }
    }

    trace->passSolid = (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID);
    trace->hitFraction = rayhit.ray.tfar / length;

    for (i = 0; i < 3; i++)
    {
        trace->hit[i] = start[i] + (stop[i] - start[i]) * trace->hitFraction;
    }
}

/*
=============
TraceLine
=============
*/
void TraceLine(const vec3_t start, const vec3_t stop, trace_t *trace,
               qboolean testAll, traceWork_t *tw)
{
    TraceLine_Embree(start, stop, trace, testAll, tw);
}

/*
=============
CleanupTrace

Releases Embree resources
=============
*/
void CleanupTrace(void)
{
    if (g_scene)
    {
        rtcReleaseScene(g_scene);
        g_scene = NULL;
    }
    if (g_device)
    {
        rtcReleaseDevice(g_device);
        g_device = NULL;
    }
}
