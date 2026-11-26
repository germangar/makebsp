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

long long c_totalTrace;

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

    // Phase 10 diagnostics: native packet support determines whether the
    // scene-level packet APIs run true packet kernels or scalar emulation
    // (the CHANGELOG's EMBREE_RAY_PACKETS question). Recorded for the gate.
    _printf("Embree: native ray packets 4/8/16 = %d/%d/%d\n",
            rtcGetDeviceProperty(g_device, RTC_DEVICE_PROPERTY_NATIVE_RAY4_SUPPORTED),
            rtcGetDeviceProperty(g_device, RTC_DEVICE_PROPERTY_NATIVE_RAY8_SUPPORTED),
            rtcGetDeviceProperty(g_device, RTC_DEVICE_PROPERTY_NATIVE_RAY16_SUPPORTED));

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
extern qboolean patchshadows;

/*
Phase 10: SOA accessors for the filter packet layout. Embree's
rtcore_ray.h defines these as C++-only reference helpers; C needs the raw
offsets. Pin: Embree 4 rtcore_ray.h:193-221 — ray words: org(0-2), tnear(3),
dir(4-6), time(7), tfar(8), mask(9), id(10), flags(11); hit words: Ng(0-2),
u(3), v(4), primID(5), geomID(6), instID(7+). The old AlphaFilter's AOS cast
of args->hit only worked because scalar queries run N=1 (SOA degenerates to
AOS); these macros are the packet-safe replacement.
*/
#define RTCRAYN_TNEAR(ray, N, i)   (((float *)(ray))[3 * (N) + (i)])
#define RTCRAYN_TFAR(ray, N, i)    (((float *)(ray))[8 * (N) + (i)])
#define RTCRAYN_ID(ray, N, i)      (((unsigned int *)(ray))[10 * (N) + (i)])
#define RTCHITN_U(hit, N, i)       (((float *)(hit))[3 * (N) + (i)])
#define RTCHITN_V(hit, N, i)       (((float *)(hit))[4 * (N) + (i)])
#define RTCHITN_PRIMID(hit, N, i)  (((unsigned int *)(hit))[5 * (N) + (i)])
#define RTCHITN_GEOMID(hit, N, i)  (((unsigned int *)(hit))[6 * (N) + (i)])

/*
================
AlphaDecision

Phase 10: the portable per-lane alpha DECISION core (the RR2-era classify
kernel is a port of this function — it takes no Embree types; the Embree
adapter resolves geomID/primID/u/v/surface data and the filter target, and
the core is invoked ONLY for draw-surface lanes; brush lanes pass through
untouched so the hit stands and blocks).

Returns qtrue when the hit must be REJECTED (ray continues), qfalse when
it stands (blocks). Tints accumulate into filterOut (never NULL: the
adapter passes the lane's ticket filter or a per-lane dummy).

Every branch is the exact line-for-line translation of the scalar
AlphaFilter body (including the W6 self-ignore quirk and the W7-class
tint branches); the old `if (tw->trace)` guards translate to `if
(filterOut)` — filterOut is always non-NULL from the adapter, matching
every reachable old state.
================
*/
static qboolean AlphaDecision(unsigned int geomID, dsurface_t *ds, shaderInfo_t *si,
                              unsigned int primID, float u, float v,
                              traceWork_t *tw, int patchshadowsFlag, float *filterOut)
{
    // Respect patchshadows setting
    if (!patchshadowsFlag && ds->surfaceType == MST_PATCH)
        return qtrue;

    // Only ignore the surface if it's truly planar (can't shadow itself).
    // For patches and triangle soups (models), we want them to cast shadows on themselves.
    if (tw && tw->ignoreSurface != -1 && geomID == (unsigned int)tw->ignoreSurface)
    {
        if (ds->surfaceType == MST_PLANAR)
            return qtrue;
    }

    // If the surface has castShadows disabled, it ONLY casts shadows on itself.
    if (!localSurfaces[geomID].castShadows)
    {
        if (!tw || tw->ignoreSurface == -1 || geomID != (unsigned int)tw->ignoreSurface)
            return qtrue;
    }

    if (si->surfaceFlags & (SURF_ALPHASHADOW | SURF_LIGHTFILTER))
    {
        if (tw && tw->isLightgrid)
        {
            if (si->surfaceFlags & SURF_ALPHASHADOW)
            {
                float trans = 1.0f - si->averageAlpha;
                filterOut[0] *= trans;
                filterOut[1] *= trans;
                filterOut[2] *= trans;
            }
            else if (si->surfaceFlags & SURF_LIGHTFILTER)
            {
                filterOut[0] *= (si->averageColor[0] / 255.0f);
                filterOut[1] *= (si->averageColor[1] / 255.0f);
                filterOut[2] *= (si->averageColor[2] / 255.0f);
            }
            return qtrue; // ignore the hit, continue
        }

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
            return qfalse; // Unknown surface type: hit stands (old early return)
        }

        // Sample the filter
        if (Trace_SampleFilter(si, s, t, filterOut, (si->surfaceFlags & SURF_LIGHTFILTER) != 0))
        {
            // Opaque hit - keep the valid flag (blocks)
            return qfalse;
        }
        // Transparent/Tinted hit - tell Embree to ignore and continue
        return qtrue;
    }

    return qfalse; // no alpha flags: hit stands
}

void AlphaFilter(const struct RTCFilterFunctionNArguments *args)
{
    if (!args->context)
        return; // No context provided, skip advanced filtering
    struct MyRayQueryContext *mcontext = (struct MyRayQueryContext *)args->context;
    traceWork_t *tw = mcontext->tw;
    unsigned int laneCount = args->N;

    for (unsigned int lane = 0; lane < laneCount; lane++)
    {
        // Inactive / no-candidate lanes: hit data is undefined, never read.
        if (args->valid[lane] != -1)
            continue;

        // Phase 10: per-lane ticket identity via ray.id (robust to packet
        // emulation, lane compaction, and N mismatch — the callback lane
        // index is NOT a safe ticket key). NULL table = scalar caller.
        float *filterTarget;
        if (mcontext->ticketLanes)
        {
            unsigned int id = RTCRAYN_ID(args->ray, laneCount, lane);
            if (id >= (unsigned int)mcontext->laneCapacity)
                Error("AlphaFilter: packet ray id %u out of range (capacity %d)", id, mcontext->laneCapacity);
            filterTarget = mcontext->ticketLanes[id]->trace.filter;
        }
        else
        {
            // Scalar caller: the flush sets tw->trace per ticket; queries
            // with tw == NULL (PointInTrisoup) fall back to the per-lane
            // dummy, exactly like the old AOS path.
            filterTarget = (tw && tw->trace) ? tw->trace->filter : NULL;
        }

        unsigned int geomID = RTCHITN_GEOMID(args->hit, laneCount, lane);
        unsigned int primID = RTCHITN_PRIMID(args->hit, laneCount, lane);
        float u = RTCHITN_U(args->hit, laneCount, lane);
        float v = RTCHITN_V(args->hit, laneCount, lane);

        // Only perform additional checks for draw surfaces; brush lanes
        // (geomID >= numDrawSurfaces) pass through untouched — the hit
        // stands and blocks (load-bearing: see §10.7.4).
        if (geomID < (unsigned int)numDrawSurfaces)
        {
            dsurface_t *ds = &drawSurfaces[geomID];
            shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
            qboolean reject;

            if (filterTarget)
                reject = AlphaDecision(geomID, ds, si, primID, u, v, tw, mcontext->patchshadows, filterTarget);
            else
            {
                vec3_t dummyFilter = {1.0f, 1.0f, 1.0f}; // per-lane: never share across lanes
                reject = AlphaDecision(geomID, ds, si, primID, u, v, tw, mcontext->patchshadows, dummyFilter);
            }

            if (reject)
                args->valid[lane] = 0;
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

int BoxInOpaqueDetail(vec3_t start, float margin);

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
    int detailContents = BoxInOpaqueDetail(origin, margin);

    if (detailContents == CONTENTS_SOLID)
        return qtrue;

    if (detailContents == CONTENTS_TRISOUP)
    {
        vec3_t dirs[3] = { {0, 0, 1}, {1, 0, 0}, {0, 1, 0} };
        int i;
        qboolean is_solid = qtrue;
        for (i = 0; i < 3; i++)
        {
            if (!PointInTrisoup(origin, dirs[i]))
            {
                is_solid = qfalse;
                break;
            }
        }
        
        if (is_solid)
        {
            return qtrue;
        }
    }

    return qfalse;
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
int BoxInOpaqueDetail(vec3_t start, float margin)
{
    int leafNum = PointInLeafNum(start);
    int i, j;
    qboolean hitTrisoup = qfalse;
    
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
            
        qboolean isTrisoupBrush = (ds->contentFlags & CONTENTS_TRISOUP) ? qtrue : qfalse;

        // Ignore brushes that don't block light (translucent, liquids, clips, fog, sky)
        // Note: We deliberately do NOT ignore SURF_NODRAW (caulk), because caulk is solid and blocks light.
        if (!isTrisoupBrush && ((ds->contentFlags & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER |
                                 CONTENTS_TRANSLUCENT | CONTENTS_FOG |
                                 CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP | CONTENTS_BOTCLIP)) ||
            (ds->surfaceFlags & SURF_SKY)))
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
            if (isTrisoupBrush) {
                hitTrisoup = qtrue;
            } else {
                return CONTENTS_SOLID; // Hard detail solid takes absolute precedence
            }
        }
    }
    
    if (hitTrisoup) {
        return CONTENTS_TRISOUP;
    }
    
    return 0;
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
    memset(&context, 0, sizeof(context)); // §10.5.1: every field, every site (Phase 10)
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
            if (localSurfaces && localSurfaces[rayhit.hit.geomID].isPlanarDerived)
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

    int detailContents = BoxInOpaqueDetail(start, 0.0f);
    if (detailContents == CONTENTS_TRISOUP)
    {
        vec3_t dirs[3] = { {0, 0, 1}, {1, 0, 0}, {0, 1, 0} };
        int i;
        qboolean is_solid = qtrue;
        for (i = 0; i < 3; i++)
        {
            if (!PointInTrisoup(start, dirs[i]))
            {
                is_solid = qfalse;
                break;
            }
        }

        if (is_solid)
        {
            return qtrue;
        }
    }

    return qfalse;
}

/*
=============
TraceLine_Embree_Ranged / TraceLine_Embree

High-performance Embree tracing path.

Phase 5: the body is parameterized by an optional ray range so radiosity's
RadVisCheck rays (tnear = RAD_ORIGIN_NUDGE * 0.5, tfar = len - nudge) can
flow through the same ticket flush as every other family. Overrides <= 0
mean "standard" (tnear = 0.0001f, tfar = length) — bit-identical for all
existing callers, which pass (0, 0).
=============
*/
static void TraceLine_Embree_Ranged(const vec3_t start, const vec3_t stop,
                                    trace_t *trace, qboolean testAll, traceWork_t *tw,
                                    float nearOverride, float farOverride)
{
    int i;

    // Phase 0 instrumentation: golden total ray count. Incremented from many
    // worker threads, hence the atomic; integer event count, so the total is
    // exact regardless of order. The ticket-wavefront phases gate on this
    // number staying identical (see studies/wavefront_ticket_embree_plan.md).
    #pragma omp atomic
    c_totalTrace++;

    struct RTCRayHit rayhit;
    memset(&rayhit, 0, sizeof(rayhit));
    struct MyRayQueryContext context;
    memset(&context, 0, sizeof(context)); // §10.5.1: every field, every site (Phase 10)
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

    float tnear = nearOverride > 0.0f ? nearOverride : 0.0001f;
    float tfar = farOverride > 0.0f ? farOverride : length;

    rayhit.ray.org_x = start[0];
    rayhit.ray.org_y = start[1];
    rayhit.ray.org_z = start[2];
    rayhit.ray.dir_x = dir[0];
    rayhit.ray.dir_y = dir[1];
    rayhit.ray.dir_z = dir[2];
    rayhit.ray.tnear = tnear;
    rayhit.ray.tfar = tfar;
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
    trace->hitFraction = rayhit.ray.tfar / tfar;

    for (i = 0; i < 3; i++)
    {
        trace->hit[i] = start[i] + (stop[i] - start[i]) * trace->hitFraction;
    }
}

static void TraceLine_Embree(const vec3_t start, const vec3_t stop,
                             trace_t *trace, qboolean testAll, traceWork_t *tw)
{
    TraceLine_Embree_Ranged(start, stop, trace, testAll, tw, 0.0f, 0.0f);
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
TraceLineRanged

Phase 5: radiosity's visibility rays use a clipped range
(tnear = RAD_ORIGIN_NUDGE * 0.5, tfar = len - nudge). Overrides <= 0 select
the standard values, so this is bit-identical to TraceLine for every
existing caller.
=============
*/
void TraceLineRanged(const vec3_t start, const vec3_t stop, trace_t *trace,
                     qboolean testAll, traceWork_t *tw, float nearClip, float farClip)
{
    TraceLine_Embree_Ranged(start, stop, trace, testAll, tw, nearClip, farClip);
}

/*
=============
TraceLineBatchEmbree

Ticket wavefront (Phase 10): packetized Stage 2. Traces
tickets[first .. first+count) — both flush sites route here.

TRACE_PACKET_WIDTH = 1: the scalar path verbatim (Gate A / shipped
default until Gate B proves otherwise).

WIDTH > 1: partitions by testAll (tracing order is free — per-ticket
results; the consume reads tickets in place), fills AOS packets
(RTCRayHit16 as a width-agnostic buffer: the RTCRay4/8/16 layouts are
exact prefixes), maps lanes to tickets via ray.id, and issues
rtcOccluded4/8/16 or rtcIntersect4/8/16. Per-lane scratch is explicit:
filter init to (1,1,1), a private active mask (never reuse Embree's
mutable valid[]), the ORIGINAL tfar per lane (hitFraction's denominator),
degenerate lanes handled outside the packet (scalar semantics, counted).
TRACE_PACKET_FAKE=1 issues each lane through the scalar queries extracted
from the packet (Gate A2 — plumbing test: fill + extraction + lane
mapping + epilogue with no kernel variable).
=============
*/
void TraceLineBatchEmbree(lightTicket_t *tickets, int first, int count, traceWork_t *tw)
{
    if (count <= 0)
        return;

#if TRACE_PACKET_WIDTH <= 1
    // Gate A / shipped default: bit-identical to the Phase 9 path.
    for (int i = first; i < first + count; i++)
    {
        lightTicket_t *t = &tickets[i];
        TraceLineRanged(t->origin, t->end, &t->trace, t->testAll, tw,
                        t->nearClip, t->farClip);
    }
    return;
#else
    // Phase 10 §10.7 (gate-found): ALL packet accesses go through the SAME
    // width-typed pointer that the Embree call receives. Filling through
    // `struct RTCRayHit16` members while calling through an `RTCRay4*` cast
    // MISCOMPILES under GCC -O2 (TBAA: the store type and the call pointer
    // type differ — stores were silently lost, found via probe bisection).
    // The RTCRayHit16-shaped raw storage is used only for size/alignment.
    unsigned char packetStorage[sizeof(struct RTCRayHit16)];
#if TRACE_PACKET_WIDTH == 4
    struct RTCRay4 *pr = (struct RTCRay4 *)packetStorage;
    struct RTCRayHit4 *prh = (struct RTCRayHit4 *)packetStorage;
#elif TRACE_PACKET_WIDTH == 8
    struct RTCRay8 *pr = (struct RTCRay8 *)packetStorage;
    struct RTCRayHit8 *prh = (struct RTCRayHit8 *)packetStorage;
#elif TRACE_PACKET_WIDTH == 16
    struct RTCRay16 *pr = (struct RTCRay16 *)packetStorage;
    struct RTCRayHit16 *prh = (struct RTCRayHit16 *)packetStorage;
#else
#error "TraceLineBatchEmbree: unsupported packet width"
#endif

    int queryValid[TRACE_PACKET_WIDTH];    // Embree's mask — may be mutated, never reused
    float origTfar[TRACE_PACKET_WIDTH];    // hitFraction denominator
    byte laneActive[TRACE_PACKET_WIDTH];   // our private epilogue mask
    lightTicket_t *laneTickets[TRACE_PACKET_WIDTH];
    struct MyRayQueryContext context;

    memset(&context, 0, sizeof(context)); // explicit: every field, every site (§10.5.1)
    rtcInitRayQueryContext(&context.context);
    context.tw = tw;
    context.patchshadows = tw ? tw->patchshadows : patchshadows;
    context.ticketLanes = laneTickets;
    context.laneCapacity = TRACE_PACKET_WIDTH;

    // Two partitions: occluded (testAll qfalse, the majority) then
    // intersect (testAll qtrue — sun/grid-bounce rays).
    for (int pass = 0; pass < 2; pass++)
    {
        qboolean wantTestAll = (pass == 1);
        int i = first;
        while (i < first + count)
        {
            // Fill up to WIDTH lanes from tickets matching this partition,
            // scanning forward past the other partition's tickets (they are
            // handled by the other pass; each ticket fills exactly once).
            int lanesFilled = 0;
            int scan = i;
            while (lanesFilled < TRACE_PACKET_WIDTH && scan < first + count)
            {
                lightTicket_t *t = &tickets[scan];
                if (t->testAll == wantTestAll)
                {
                    vec3_t dir;
                    VectorSubtract(t->end, t->origin, dir);
                    float length = VectorLength(dir);

                    if (length < 0.0001f)
                    {
                        // Degenerate lane: scalar semantics, outside the packet.
                        t->trace.filter[0] = 1.0f;
                        t->trace.filter[1] = 1.0f;
                        t->trace.filter[2] = 1.0f;
                        t->trace.hitFraction = 1.0f;
                        VectorCopy(t->origin, t->trace.hit);
                        #pragma omp atomic
                        c_totalTrace++;
                    }
                    else
                    {
                        VectorScale(dir, 1.0f / length, dir);

                        pr->org_x[lanesFilled] = t->origin[0];
                        pr->org_y[lanesFilled] = t->origin[1];
                        pr->org_z[lanesFilled] = t->origin[2];
                        pr->dir_x[lanesFilled] = dir[0];
                        pr->dir_y[lanesFilled] = dir[1];
                        pr->dir_z[lanesFilled] = dir[2];
                        pr->tnear[lanesFilled] = t->nearClip > 0.0f ? t->nearClip : 0.0001f;
                        pr->tfar[lanesFilled] = t->farClip > 0.0f ? t->farClip : length;
                        pr->time[lanesFilled] = 0.0f;
                        pr->mask[lanesFilled] = 0xFFFFFFFF;
                        pr->id[lanesFilled] = (unsigned int)lanesFilled;
                        pr->flags[lanesFilled] = 0;
                        prh->hit.geomID[lanesFilled] = RTC_INVALID_GEOMETRY_ID;
                        prh->hit.instID[0][lanesFilled] = RTC_INVALID_GEOMETRY_ID;

                        t->trace.filter[0] = 1.0f;
                        t->trace.filter[1] = 1.0f;
                        t->trace.filter[2] = 1.0f;

                        laneTickets[lanesFilled] = t;
                        origTfar[lanesFilled] = pr->tfar[lanesFilled];
                        laneActive[lanesFilled] = 1;
                        queryValid[lanesFilled] = -1;

                        #pragma omp atomic
                        c_totalTrace++;
                        lanesFilled++;
                    }
                }
                scan++;
            }
            i = scan;

            if (lanesFilled == 0)
                continue; // everything up to scan belonged to the other pass

            // Zero-init the inactive remainder lanes defensively (§10.6.6):
            // traversal may touch them; never leave NaN/garbage in a packet.
            // The remainder lanes' ray.id is an OUT-OF-RANGE sentinel so the
            // id-scan epilogue (§10.7) can never map them to a ticket.
            for (int l = lanesFilled; l < TRACE_PACKET_WIDTH; l++)
            {
                pr->org_x[l] = 0.0f;
                pr->org_y[l] = 0.0f;
                pr->org_z[l] = 0.0f;
                pr->tnear[l] = 0.0f;
                pr->dir_x[l] = 0.0f;
                pr->dir_y[l] = 0.0f;
                pr->dir_z[l] = 0.0f;
                pr->time[l] = 0.0f;
                pr->tfar[l] = 0.0f;
                pr->mask[l] = 0;
                pr->id[l] = 0xFFFFFFFF; // id: invalid sentinel
                pr->flags[l] = 0;
                laneTickets[l] = NULL;
                laneActive[l] = 0;
                queryValid[l] = 0;
            }

            if (wantTestAll)
            {
                struct RTCIntersectArguments iargs;
                rtcInitIntersectArguments(&iargs);
                iargs.context = &context.context;
#if TRACE_PACKET_FAKE
                // Gate A2: scalar issue of the packet's extracted lanes.
                for (int l = 0; l < lanesFilled; l++)
                {
                    struct RTCRayHit rh;
                    memset(&rh, 0, sizeof(rh));
                    rh.ray.org_x = pr->org_x[l];
                    rh.ray.org_y = pr->org_y[l];
                    rh.ray.org_z = pr->org_z[l];
                    rh.ray.dir_x = pr->dir_x[l];
                    rh.ray.dir_y = pr->dir_y[l];
                    rh.ray.dir_z = pr->dir_z[l];
                    rh.ray.tnear = pr->tnear[l];
                    rh.ray.tfar = pr->tfar[l];
                    rh.ray.mask = pr->mask[l];
                    rh.ray.id = pr->id[l];
                    rh.ray.flags = pr->flags[l];
                    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                    rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
                    rtcIntersect1(g_scene, &rh, &iargs);
                    pr->tfar[l] = rh.ray.tfar;
                    prh->hit.geomID[l] = rh.hit.geomID;
                    prh->hit.instID[0][l] = rh.hit.instID[0];
                }
#else
                switch (TRACE_PACKET_WIDTH)
                {
                case 4:
                    rtcIntersect4(queryValid, g_scene, (struct RTCRayHit4 *)prh, &iargs);
                    break;
                case 8:
                    rtcIntersect8(queryValid, g_scene, (struct RTCRayHit8 *)prh, &iargs);
                    break;
                case 16:
                    rtcIntersect16(queryValid, g_scene, (struct RTCRayHit16 *)prh, &iargs);
                    break;
                default:
                    Error("TraceLineBatchEmbree: unsupported packet width %d", TRACE_PACKET_WIDTH);
                }
#endif
            }
            else
            {
                struct RTCOccludedArguments oargs;
                rtcInitOccludedArguments(&oargs);
                oargs.context = &context.context;
#if TRACE_PACKET_FAKE
                for (int l = 0; l < lanesFilled; l++)
                {
                    struct RTCRay rh;
                    memset(&rh, 0, sizeof(rh));
                    rh.org_x = pr->org_x[l];
                    rh.org_y = pr->org_y[l];
                    rh.org_z = pr->org_z[l];
                    rh.dir_x = pr->dir_x[l];
                    rh.dir_y = pr->dir_y[l];
                    rh.dir_z = pr->dir_z[l];
                    rh.tnear = pr->tnear[l];
                    rh.tfar = pr->tfar[l];
                    rh.mask = pr->mask[l];
                    rh.id = pr->id[l];
                    rh.flags = pr->flags[l];
                    rtcOccluded1(g_scene, &rh, &oargs);
                    pr->tfar[l] = rh.tfar;
                }
#else
                switch (TRACE_PACKET_WIDTH)
                {
                case 4:
                    rtcOccluded4(queryValid, g_scene, (struct RTCRay4 *)pr, &oargs);
                    break;
                case 8:
                    rtcOccluded8(queryValid, g_scene, (struct RTCRay8 *)pr, &oargs);
                    break;
                case 16:
                    rtcOccluded16(queryValid, g_scene, (struct RTCRay16 *)pr, &oargs);
                    break;
                default:
                    Error("TraceLineBatchEmbree: unsupported packet width %d", TRACE_PACKET_WIDTH);
                }
#endif
            }

            // Per-lane epilogue. Phase 10 §10.7: the packet may be REORDERED
            // or COMPACTED by filter callbacks (rtcOccluded4 docs: "may
            // change the ray packet size and ray order"), so results are
            // mapped back by ray.id — the id travels with the ray — never
            // by callback/position index. Each original lane is consumed
            // at most once; a lane with no surviving position is counted
            // in the unmapped canary (its ticket keeps generation-time
            // trace state; if this ever fires, width 1 is the answer).
            {
                byte written[TRACE_PACKET_WIDTH];
                memset(written, 0, sizeof(written));

                for (int l = 0; l < TRACE_PACKET_WIDTH; l++)
                {
                    unsigned int id = pr->id[l];
                    if (id >= (unsigned int)TRACE_PACKET_WIDTH)
                        continue; // inactive remainder slot or Embree scratch
                    if (!laneActive[id] || written[id])
                        continue;
                    written[id] = 1;

                    lightTicket_t *t = laneTickets[id];
                    qboolean passSolid;
                    float fraction;
                    if (wantTestAll)
                    {
                        passSolid = (prh->hit.geomID[l] != RTC_INVALID_GEOMETRY_ID);
                        fraction = pr->tfar[l] / origTfar[id];
                    }
                    else
                    {
                        // Embree 4 occlusion semantics: tfar becomes -inf on hit.
                        if (pr->tfar[l] < 0.0f)
                        {
                            passSolid = qtrue;
                            fraction = 0.0f;
                        }
                        else
                        {
                            passSolid = qfalse;
                            fraction = pr->tfar[l] / origTfar[id];
                        }
                    }

                    t->trace.passSolid = passSolid;
                    t->trace.hitFraction = fraction;
                    for (int c = 0; c < 3; c++)
                        t->trace.hit[c] = t->origin[c] + (t->end[c] - t->origin[c]) * fraction;
                }

                for (int id = 0; id < TRACE_PACKET_WIDTH; id++)
                {
                    if (laneActive[id] && !written[id])
                    {
                        #pragma omp atomic
                        c_packetUnmappedLanes++;
                    }
                }
            }
        }
    }
#endif
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
