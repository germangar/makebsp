/*
===========================================================================
radiosity.c — Global Illumination (Radiant Energy Bounce)

Architecture:
  Phase 1 - Emit:       Scan lightFloats, turn each lit luxel into a
                        world-space polygon area emitter.
  Phase 2 - Integrate:  For each destination luxel, gather irradiance
                        from all line-of-sight emitters using an analytic
                        closed-form area integral (no Monte Carlo).
  Phase 2.5 - Voxelize: Splat all Phase 2 results into a world-space
                        voxel grid to share light across UV islands.
  Phase 3 - Reconstruct: Fill in-between pixels by sampling the Voxel Grid
                        rather than UV-local bilinear interpolation.
  Phase 4 - Merge:      Additively blend radiosityFloats → lightFloats.
===========================================================================
*/

#include "light.h"
#include "radiosity.h"
#include "../shared/surface_extra.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tuning parameters (managed via main.c CLI)
// ---------------------------------------------------------------------------

float rad_bounce_scale  = 0.5f;   // Energy per bounce (conserved)
float rad_color_ratio   = 0.5f;   // Greyscale vs colour bleeding
float rad_min_energy    = 1.0f;   // Min brightness for emitters
float rad_depth_min     = RAD_DEPTH_MIN_DEFAULT;
float rad_depth_max     = RAD_DEPTH_MAX_DEFAULT;
float rad_depth_intensity = RAD_DEPTH_INTENSITY_DEFAULT;
int   rad_interval      = 4;      // Sparse grid resolution (4 = 4x4)
float rad_voxel_size    = 0.0f;   // Adaptive default: samplesize * rad_interval
float rad_angle_match   = 60.0f;  // Angle in degrees (Default: 60)
static float rad_angle_match_cos = 0.5f;

// Amount to nudge the emitter origin off the surface along its normal.
// Prevents the emitter from self-shadowing via Embree.
#define RAD_ORIGIN_NUDGE        1.5f

// light.h/c exports


// ---------------------------------------------------------------------------
// emitter_t
// One per lit luxel on a planar/patch surface.
// ---------------------------------------------------------------------------

typedef struct {
    vec3_t  center;   // world-space centre of this luxel (nudged off the surface)
    vec3_t  normal;   // surface normal (hemisphere direction)
    vec3_t  color;    // effective emitted energy (lightFloats * albedo * bounceScale * colorRatio)
    float   area;     // world-space area of this luxel quad (units²)
} emitter_t;

// ---------------------------------------------------------------------------
// Voxel Grid Structure
// ---------------------------------------------------------------------------

typedef struct radVoxel_s {
    vec3_t color;
    vec3_t normal;
    float weight;
    struct radVoxel_s *next;
} radVoxel_t;

static radVoxel_t *g_radVoxels = NULL;
static vec3_t g_radVoxelMins;
static int g_radVoxelDims[3];

#define RAD_VOXEL_INDEX(x, y, z) \
    (((size_t)(x) * g_radVoxelDims[1] * g_radVoxelDims[2]) + \
     ((size_t)(y) * g_radVoxelDims[2]) + (z))

static void RadiosityVoxelReset(void) {
    if (g_radVoxels) {
        size_t total = (size_t)g_radVoxelDims[0] * g_radVoxelDims[1] * g_radVoxelDims[2];
        for (size_t i = 0; i < total; i++) {
            radVoxel_t *curr = g_radVoxels[i].next;
            while (curr) {
                radVoxel_t *next = curr->next;
                free(curr);
                curr = next;
            }
        }
        free(g_radVoxels);
    }

    for (int i = 0; i < 3; i++) {
        g_radVoxelMins[i] = floor(dmodels[0].mins[i] / rad_voxel_size) * rad_voxel_size - rad_voxel_size;
        float maxVal = ceil(dmodels[0].maxs[i] / rad_voxel_size) * rad_voxel_size + rad_voxel_size;
        g_radVoxelDims[i] = (int)((maxVal - g_radVoxelMins[i]) / rad_voxel_size) + 1;
    }

    size_t numHeads = (size_t)g_radVoxelDims[0] * g_radVoxelDims[1] * g_radVoxelDims[2];
    g_radVoxels = calloc(numHeads, sizeof(radVoxel_t));
}

static void RadiosityVoxelAdd(const vec3_t pos, const vec3_t normal, const vec3_t color) {
    int v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = (int)((pos[i] - g_radVoxelMins[i]) / rad_voxel_size);
        if (v[i] < 0 || v[i] >= g_radVoxelDims[i]) return;
    }

    // Safety: Ignore invalid normals or zero energy to prevent NaN propagation
    if (normal[0] == 0 && normal[1] == 0 && normal[2] == 0) return;
    if (color[0] <= 0 && color[1] <= 0 && color[2] <= 0) return;

    size_t idx = RAD_VOXEL_INDEX(v[0], v[1], v[2]);
    radVoxel_t *head = &g_radVoxels[idx];
    
    // 1. If this is the very first sample in this voxel, use the head node itself
    if (head->weight <= 0) {
        VectorCopy(color, head->color);
        VectorCopy(normal, head->normal);
        head->weight = 1.0f;
        head->next = NULL;
        return;
    }

    // 2. Otherwise, find a voxel bucket with a similar normal
    radVoxel_t *curr = head;
    while (curr) {
        if (DotProduct(curr->normal, normal) > rad_angle_match_cos) {
            VectorAdd(curr->color, color, curr->color);
            VectorAdd(curr->normal, normal, curr->normal);
            curr->weight += 1.0f;
            return;
        }
        if (!curr->next) break;
        curr = curr->next;
    }

    // 3. Create new normal-specific bucket if no match found
    radVoxel_t *newV = malloc(sizeof(radVoxel_t));
    VectorCopy(color, newV->color);
    VectorCopy(normal, newV->normal);
    newV->weight = 1.0f;
    newV->next = NULL;
    curr->next = newV;
}


// ---------------------------------------------------------------------------
// Helper: test line-of-sight between two world points via Embree.
// Returns qtrue if the path is CLEAR (not occluded).
// ---------------------------------------------------------------------------

static qboolean RadVisCheck(const vec3_t from, const vec3_t to) {
    struct RTCRayHit rayhit;
    struct RTCIntersectArguments iargs;
    vec3_t  dir;
    float   len;

    VectorSubtract(to, from, dir);

    len = VectorLength(dir);
    if (len < 0.001f)
        return qfalse; // degenerate

    rayhit.ray.org_x  = from[0];
    rayhit.ray.org_y  = from[1];
    rayhit.ray.org_z  = from[2];
    rayhit.ray.dir_x  = dir[0] / len;
    rayhit.ray.dir_y  = dir[1] / len;
    rayhit.ray.dir_z  = dir[2] / len;
    rayhit.ray.tnear  = RAD_ORIGIN_NUDGE * 0.5f;
    rayhit.ray.tfar   = len - RAD_ORIGIN_NUDGE * 0.5f;
    rayhit.ray.mask   = 0xFFFFFFFF;
    rayhit.ray.flags  = 0;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

    struct MyRayQueryContext context;
    rtcInitRayQueryContext(&context.context);
    context.tw = NULL;
    context.patchshadows = patchshadows;

    rtcInitIntersectArguments(&iargs);
    iargs.context = &context.context;

    rtcIntersect1(g_scene, &rayhit, &iargs);

    return (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) ? qtrue : qfalse;
}

// ---------------------------------------------------------------------------
// Patch Subdivision Helper
// ---------------------------------------------------------------------------

static mesh_t *SubdividePatchToLightmap(dsurface_t *ds) {
    mesh_t srcMesh, *mesh, *subdivided, *finalMesh;
    int widthtable[MAX_EXPANDED_AXIS], heighttable[MAX_EXPANDED_AXIS];
    int ssize = samplesize;
    shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
    if (si && si->lightmapSampleSize) ssize = si->lightmapSampleSize;

    srcMesh.width = ds->patchWidth;
    srcMesh.height = ds->patchHeight;
    srcMesh.verts = drawVerts + ds->firstVert;

    mesh = SubdivideMesh(srcMesh, 8, 999);
    PutMeshOnCurve(*mesh);
    MakeMeshNormals(*mesh);

    subdivided = RemoveLinearMeshColumnsRows(mesh);
    FreeMesh(mesh);

    finalMesh = SubdivideMeshQuads(subdivided, ssize, LIGHTMAP_WIDTH, widthtable, heighttable);
    FreeMesh(subdivided);

    if (finalMesh->width != ds->lightmapWidth || finalMesh->height != ds->lightmapHeight) {
        static qboolean warned = qfalse;
        if (!warned) {
            _printf("WARNING: Radiosity patch subdivision mismatch (%dx%d != %dx%d)\n",
                    finalMesh->width, finalMesh->height, ds->lightmapWidth, ds->lightmapHeight);
            warned = qtrue;
        }
    }

    return finalMesh;
}

// ---------------------------------------------------------------------------
// Phase 1 — Emit
// ---------------------------------------------------------------------------

static emitter_t *g_emitters     = NULL;
static int        g_numEmitters  = 0;



extern float      *accumRadiosityFloats;

static void RadiosityEmit(const float *srcBuffer) {
    int             i, k, lx, ly;
    int             capacity = 4096;


    g_numEmitters = 0;
    g_emitters    = malloc(sizeof(emitter_t) * capacity);

    if (!g_emitters)
        Error("RadiosityEmit: malloc failed");

    for (i = 0; i < numDrawSurfaces; i++) {
        dsurface_t   *ds = &drawSurfaces[i];
        shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

        if (ds->lightmapNum[0] < 0) continue;
        if (ds->lightmapWidth <= 0 || ds->lightmapHeight <= 0) continue;

        localSurfaces[i].emitterStart = g_numEmitters;
        float maxIntensity = 0;
        float totalArea = 0;

        mesh_t *patchMesh = NULL;
        vec3_t surfNormal = {0,0,0};
        if (ds->surfaceType == MST_PATCH) {
            patchMesh = SubdividePatchToLightmap(ds);
        } else if (ds->surfaceType == MST_PLANAR) {
            CrossProduct(ds->lightmapVecs[0], ds->lightmapVecs[1], surfNormal);
            VectorNormalize(surfNormal, surfNormal);
        } else {
            if (ds->numVerts < 3) continue;
            VectorCopy(drawVerts[ds->firstVert].normal, surfNormal);
        }

        // ... [Area calculation logic remains the same] ...
        float luxelArea;
        // ... (skipping for brevity but keeping logic) ...
        if (ds->surfaceType == MST_TRIANGLE_SOUP) {
            double total3DArea = 0;
            for (int t = 0; t < ds->numIndexes; t += 3) {
                vec3_t v1, v2, c;
                int i0 = drawIndexes[ds->firstIndex + t];
                int i1 = drawIndexes[ds->firstIndex + t + 1];
                int i2 = drawIndexes[ds->firstIndex + t + 2];
                VectorSubtract(drawVerts[ds->firstVert + i1].xyz, drawVerts[ds->firstVert + i0].xyz, v1);
                VectorSubtract(drawVerts[ds->firstVert + i2].xyz, drawVerts[ds->firstVert + i0].xyz, v2);
                CrossProduct(v1, v2, c);
                total3DArea += 0.5 * VectorLength(c);
            }
            int numTexels = ds->lightmapWidth * ds->lightmapHeight;
            luxelArea = (numTexels > 0) ? (float)(total3DArea / numTexels) : 1.0f;
        } else if (ds->surfaceType == MST_PATCH) {
            double total3DArea = 0;
            if (patchMesh && patchMesh->width > 1 && patchMesh->height > 1) {
                for (int ty = 0; ty < patchMesh->height - 1; ty++) {
                    for (int tx = 0; tx < patchMesh->width - 1; tx++) {
                        vec3_t v1, v2, v3, v4, c;
                        VectorSubtract(patchMesh->verts[ty * patchMesh->width + tx + 1].xyz, patchMesh->verts[ty * patchMesh->width + tx].xyz, v1);
                        VectorSubtract(patchMesh->verts[(ty + 1) * patchMesh->width + tx].xyz, patchMesh->verts[ty * patchMesh->width + tx].xyz, v2);
                        CrossProduct(v1, v2, c);
                        total3DArea += 0.5 * VectorLength(c);
                        VectorSubtract(patchMesh->verts[(ty + 1) * patchMesh->width + tx + 1].xyz, patchMesh->verts[ty * patchMesh->width + tx + 1].xyz, v3);
                        VectorSubtract(patchMesh->verts[(ty + 1) * patchMesh->width + tx].xyz, patchMesh->verts[ty * patchMesh->width + tx + 1].xyz, v4);
                        CrossProduct(v3, v4, c);
                        total3DArea += 0.5 * VectorLength(c);
                    }
                }
            }
            int numTexels = ds->lightmapWidth * ds->lightmapHeight;
            luxelArea = (numTexels > 0 && total3DArea > 0) ? (float)(total3DArea / numTexels) : 1.0f;
        } else {
            luxelArea = VectorLength(ds->lightmapVecs[0]) * VectorLength(ds->lightmapVecs[1]);
        }

        vec3_t albedo;
        float avgLum = (si->averageColor[0] + si->averageColor[1] + si->averageColor[2]) / (3.0f * 255.0f);
        for (k = 0; k < 3; k++) {
            float fullColor  = si->averageColor[k] / 255.0f;
            albedo[k] = fullColor * rad_color_ratio + avgLum * (1.0f - rad_color_ratio);
            if (albedo[k] < 0.0f) albedo[k] = 0.0f;
        }

        for (ly = 0; ly < ds->lightmapHeight; ly += rad_interval) {
            for (lx = 0; lx < ds->lightmapWidth; lx += rad_interval) {
                int k_lm = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
                float *src = (float *)&srcBuffer[k_lm * 3];

                if (lightAlphaMask && !lightAlphaMask[k_lm]) continue;
                if (src[0] + src[1] + src[2] < rad_min_energy) continue;

                if (g_numEmitters >= capacity) {
                    capacity *= 2;
                    g_emitters = realloc(g_emitters, sizeof(emitter_t) * capacity);
                }

                emitter_t *em = &g_emitters[g_numEmitters++];

                if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + (float)rad_interval * 0.5f;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + (float)rad_interval * 0.5f;
                    if (!TriSoupSamplePoint(ds, st, em->center, em->normal)) {
                        VectorClear(em->center); VectorClear(em->normal);
                    }
                    VectorMA(em->center, RAD_ORIGIN_NUDGE, em->normal, em->center);
                    VectorAdd(em->center, localSurfaces[i].entityOrigin, em->center);
                } else if (ds->surfaceType == MST_PATCH) {
                    if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                        drawVert_t *dv = &patchMesh->verts[ly * patchMesh->width + lx];
                        VectorCopy(dv->normal, em->normal);
                        VectorMA(dv->xyz, RAD_ORIGIN_NUDGE, em->normal, em->center);
                        VectorAdd(em->center, localSurfaces[i].entityOrigin, em->center);
                    } else {
                        VectorClear(em->normal);
                        VectorClear(em->center);
                    }
                } else {
                    VectorMA(ds->lightmapOrigin, (float)lx + (float)rad_interval * 0.5f, ds->lightmapVecs[0], em->center);
                    VectorMA(em->center, (float)ly + (float)rad_interval * 0.5f, ds->lightmapVecs[1], em->center);
                    VectorMA(em->center, RAD_ORIGIN_NUDGE, surfNormal, em->center);
                    VectorAdd(em->center, localSurfaces[i].entityOrigin, em->center);
                    VectorCopy(surfNormal, em->normal);
                }
                
                em->area = luxelArea * (float)(rad_interval * rad_interval);
                VectorScale(src, rad_bounce_scale, em->color);
                for (k = 0; k < 3; k++) em->color[k] *= albedo[k];

                totalArea += em->area;
                float lum = em->color[0] > em->color[1] ? (em->color[0] > em->color[2] ? em->color[0] : em->color[2]) : (em->color[1] > em->color[2] ? em->color[1] : em->color[2]);
                if (lum > maxIntensity) maxIntensity = lum;
            }
        }
        if (patchMesh) FreeMesh(patchMesh);

        localSurfaces[i].emitterCount = g_numEmitters - localSurfaces[i].emitterStart;
        if (localSurfaces[i].emitterCount > 0 && maxIntensity > 0) {
            localSurfaces[i].maxReach = CalculateRadiosityLightReach(totalArea, maxIntensity, MIN_RADIOSITY_EMITTER_GROUP_ADD);
        } else {
            localSurfaces[i].maxReach = 0;
        }
    }
    _printf("    %d emitters generated from lit luxels\n", g_numEmitters);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Phase 2 — Integrate
// ---------------------------------------------------------------------------

static void RadiosityIntegrateOneSurface(int surfIdx) {
    dsurface_t   *ds = &drawSurfaces[surfIdx];
    if (ds->lightmapNum[0] < 0 || ds->lightmapWidth <= 0 || ds->lightmapHeight <= 0 || g_numEmitters <= 0) return;

    mesh_t *patchMesh = NULL;
    vec3_t dstNormal;
    if (ds->surfaceType == MST_PATCH) {
        patchMesh = SubdividePatchToLightmap(ds);
    } else if (ds->surfaceType == MST_PLANAR) {
        CrossProduct(ds->lightmapVecs[0], ds->lightmapVecs[1], dstNormal);
        if (VectorNormalize(dstNormal, dstNormal) < 0.0001f) VectorCopy(drawVerts[ds->firstVert].normal, dstNormal);
    } else {
        if (ds->numVerts < 3) return;
        VectorCopy(drawVerts[ds->firstVert].normal, dstNormal);
    }

    voxelPoint_t *points = NULL;
    int numPoints = 0;
    if (ds->surfaceType == MST_TRIANGLE_SOUP) {
        points = VoxelCache_Load(surfIdx, &numPoints);
    }

    for (int ly = 0; ly < ds->lightmapHeight; ly += rad_interval) {
        for (int lx = 0; lx < ds->lightmapWidth; lx += rad_interval) {
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
            if (lightAlphaMask && !lightAlphaMask[k_dst]) continue;

            vec3_t dst;
            if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                qboolean found = qfalse;
                if (points) {
                    for (int i = 0; i < numPoints; i++) {
                        if (points[i].pixelIndex == k_dst) {
                            VectorCopy(points[i].pos, dst);
                            VectorCopy(points[i].normal, dstNormal);
                            found = qtrue;
                            break;
                        }
                    }
                }
                if (!found) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + 0.5f;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + 0.5f;
                    if (!TriSoupSamplePoint(ds, st, dst, dstNormal)) continue;
                }
                VectorMA(dst, RAD_ORIGIN_NUDGE, dstNormal, dst);
                VectorAdd(dst, localSurfaces[surfIdx].entityOrigin, dst);
            } else if (ds->surfaceType == MST_PATCH) {

                if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                    drawVert_t *dv = &patchMesh->verts[ly * patchMesh->width + lx];
                    VectorCopy(dv->normal, dstNormal);
                    VectorMA(dv->xyz, RAD_ORIGIN_NUDGE, dstNormal, dst);
                    VectorAdd(dst, localSurfaces[surfIdx].entityOrigin, dst);
                } else {
                    continue;
                }
            } else {
                VectorMA(ds->lightmapOrigin, (float)lx + 0.5f, ds->lightmapVecs[0], dst);
                VectorMA(dst, (float)ly + 0.5f, ds->lightmapVecs[1], dst);
                VectorMA(dst, RAD_ORIGIN_NUDGE, dstNormal, dst);
                VectorAdd(dst, localSurfaces[surfIdx].entityOrigin, dst);
            }

            // ---------------------------------------------------------------
            // Trisoup: original path — bake cosDst into scalar, write radiosityFloats.
            // ---------------------------------------------------------------
            if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                float accum[3] = {0, 0, 0};
                for (int s = 0; s < numDrawSurfaces; s++) {
                    if (localSurfaces[s].emitterCount == 0) continue;

                    // Cull A: Distance
                    vec3_t v_to_surf; VectorSubtract(localSurfaces[s].origin, dst, v_to_surf);
                    float dist_to_surf = VectorLength(v_to_surf);
                    if (dist_to_surf > localSurfaces[s].maxReach + localSurfaces[s].radius) continue;

                    // Cull B: Receiver Plane (is surface entirely behind the destination?)
                    if (DotProduct(v_to_surf, dstNormal) < -localSurfaces[s].radius) continue;

                    // Cull C: Emitter Plane (is destination entirely behind the emitter?)
                    if (drawSurfaces[s].surfaceType == MST_PLANAR) {
                        vec3_t emitNormal;
                        CrossProduct(drawSurfaces[s].lightmapVecs[0], drawSurfaces[s].lightmapVecs[1], emitNormal);
                        if (VectorNormalize(emitNormal, emitNormal) > 0) {
                            vec3_t v_to_dst; VectorSubtract(dst, localSurfaces[s].origin, v_to_dst);
                            if (DotProduct(v_to_dst, emitNormal) < -localSurfaces[s].radius) continue;
                        }
                    }

                    for (int e = localSurfaces[s].emitterStart; e < localSurfaces[s].emitterStart + localSurfaces[s].emitterCount; e++) {
                        emitter_t *em = &g_emitters[e];
                        vec3_t ray; VectorSubtract(em->center, dst, ray);
                        float dist = VectorLength(ray);
                        if (dist < 0.001f) continue;
                        vec3_t rayDir; VectorScale(ray, 1.0f / dist, rayDir);

                        float cosEmit = -DotProduct(em->normal, rayDir);
                        if (cosEmit <= 0.0f) continue;
                        float cosDst = DotProduct(dstNormal, rayDir);
                        if (cosDst <= 0.0f) continue;

                        float distClamped = dist < rad_depth_max ? rad_depth_max : dist;
                        float formFactor = (em->area * cosEmit * cosDst) / (M_PI * distClamped * distClamped);
                        if (g_game->deluxeMap) {
                            // Store Irradiance: remove cosDst from the color contribution
                            formFactor = (em->area * cosEmit) / (M_PI * distClamped * distClamped);
                        }
                        
                        if (dist < rad_depth_min) formFactor *= rad_depth_intensity;
                        else if (dist < rad_depth_max) {
                            float lerp = (dist - rad_depth_min) / (rad_depth_max - rad_depth_min);
                            formFactor *= rad_depth_intensity + (1.0f - rad_depth_intensity) * lerp;
                        }
                        if (formFactor > 1.0f) formFactor = 1.0f;
                        
                        // Precise intensity cull: check if brightest color component * formFactor < threshold
                        float maxColor = em->color[0] > em->color[1] ? (em->color[0] > em->color[2] ? em->color[0] : em->color[2]) : (em->color[1] > em->color[2] ? em->color[1] : em->color[2]);
                        if (formFactor * maxColor <= MIN_RADIOSITY_EMITTER_ADD) continue;

                        if (!RadVisCheck(dst, em->center)) continue;
                        VectorMA(accum, formFactor, em->color, accum);
                    }
                }
                if (accum[0] > 0 || accum[1] > 0 || accum[2] > 0) {
                    ThreadLock();
                    VectorAdd(&radiosityFloats[k_dst * 3], accum, &radiosityFloats[k_dst * 3]);
                    ThreadUnlock();
                }
            } else {
                // ---------------------------------------------------------------
                // Planar / Patch: irradiance vector path.
                // ---------------------------------------------------------------
                float ivec[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
                vec3_t lvec = {0,0,0};
                vec3_t energySumVec = {0,0,0};

                for (int s = 0; s < numDrawSurfaces; s++) {
                    if (localSurfaces[s].emitterCount == 0) continue;

                    // Cull A: Distance (More sensitive for Radiosity)
                    vec3_t v_to_surf; VectorSubtract(localSurfaces[s].origin, dst, v_to_surf);
                    float dist_to_surf = VectorLength(v_to_surf);
                    if (dist_to_surf > localSurfaces[s].maxReach + localSurfaces[s].radius) continue;

                    // Cull B: Receiver Plane
                    if (DotProduct(v_to_surf, dstNormal) < -localSurfaces[s].radius) continue;

                    // Cull C: Emitter Plane
                    if (drawSurfaces[s].surfaceType == MST_PLANAR) {
                        vec3_t emitNormal;
                        CrossProduct(drawSurfaces[s].lightmapVecs[0], drawSurfaces[s].lightmapVecs[1], emitNormal);
                        if (VectorNormalize(emitNormal, emitNormal) > 0) {
                            vec3_t v_to_dst; VectorSubtract(dst, localSurfaces[s].origin, v_to_dst);
                            if (DotProduct(v_to_dst, emitNormal) < -localSurfaces[s].radius) continue;
                        }
                    }

                    for (int e = localSurfaces[s].emitterStart; e < localSurfaces[s].emitterStart + localSurfaces[s].emitterCount; e++) {
                        emitter_t *em = &g_emitters[e];
                        vec3_t ray; VectorSubtract(em->center, dst, ray);
                        float dist = VectorLength(ray);
                        if (dist < 0.001f) continue;
                        vec3_t rayDir; VectorScale(ray, 1.0f / dist, rayDir);

                        float cosEmit = -DotProduct(em->normal, rayDir);
                        if (cosEmit <= 0.0f) continue;
                        float cosDst = DotProduct(dstNormal, rayDir);
                        if (cosDst <= 0.0f) continue;

                        float distClamped = dist < rad_depth_max ? rad_depth_max : dist;
                        float formFactorBase = (em->area * cosEmit) / (M_PI * distClamped * distClamped);
                        if (dist < rad_depth_min) formFactorBase *= rad_depth_intensity;
                        else if (dist < rad_depth_max) {
                            float lerp = (dist - rad_depth_min) / (rad_depth_max - rad_depth_min);
                            formFactorBase *= rad_depth_intensity + (1.0f - rad_depth_intensity) * lerp;
                        }
                        if (formFactorBase * cosDst > 1.0f) formFactorBase = 1.0f / cosDst;
                        
                        // Precise intensity cull: use specialized radiosity threshold
                        float maxColor = em->color[0] > em->color[1] ? (em->color[0] > em->color[2] ? em->color[0] : em->color[2]) : (em->color[1] > em->color[2] ? em->color[1] : em->color[2]);
                        if (formFactorBase * cosDst * maxColor <= MIN_RADIOSITY_EMITTER_ADD) continue;

                        if (!RadVisCheck(dst, em->center)) continue;

                        float energySum = 0;
                        for (int c = 0; c < 3; c++) {
                            float energy = formFactorBase * em->color[c];
                            energySum += energy;
                            energySumVec[c] += energy;
                            VectorMA(ivec[c], energy, rayDir, ivec[c]);
                        }
                        // Lambertian weighted vector (Irradiance * Cosine)
                        VectorMA(lvec, energySum * cosDst, rayDir, lvec);
                    }
                }

                if (ivec[0][0] != 0 || ivec[0][1] != 0 || ivec[0][2] != 0 ||
                    ivec[1][0] != 0 || ivec[1][1] != 0 || ivec[1][2] != 0 ||
                    ivec[2][0] != 0 || ivec[2][1] != 0 || ivec[2][2] != 0) {

                    // Write scalar result (dot with sparse-point normal) for voxel path compat
                    float accum[3];
                    for (int c = 0; c < 3; c++) {
                        accum[c] = DotProduct(dstNormal, ivec[c]);
                        if (accum[c] < 0.0f) accum[c] = 0.0f;
                    }


                    ThreadLock();
                    VectorAdd(&radiosityFloats[k_dst * 3], accum, &radiosityFloats[k_dst * 3]);
                    if (irradianceScalarFloats) {
                        VectorAdd(&irradianceScalarFloats[k_dst * 3], energySumVec, &irradianceScalarFloats[k_dst * 3]);
                    }
                    if (irradianceVecFloats) {
                        for (int c = 0; c < 3; c++) {
                            irradianceVecFloats[k_dst * 9 + c * 3 + 0] += ivec[c][0];
                            irradianceVecFloats[k_dst * 9 + c * 3 + 1] += ivec[c][1];
                            irradianceVecFloats[k_dst * 9 + c * 3 + 2] += ivec[c][2];
                        }
                    }
                    if (lambertianVecFloats) {
                        VectorAdd(&lambertianVecFloats[k_dst * 3], lvec, &lambertianVecFloats[k_dst * 3]);
                    }
                    if (deluxeFloats && lightSurfaceIndex) {
                        lightSurfaceIndex[k_dst] = surfIdx;
                    }
                    ThreadUnlock();
                }
            }
        }
    }
    if (patchMesh) FreeMesh(patchMesh);
    if (points) free(points);
}


static void RadiosityIntegrateThread(int surfIdx) {
    RadiosityIntegrateOneSurface(surfIdx);
}

// ---------------------------------------------------------------------------
static void RadiosityVoxelize(void) {
    _printf("  [voxelize]   Unified world-space splatting... ");
    RadiosityVoxelReset();

    for (int s = 0; s < numDrawSurfaces; s++) {
        dsurface_t *ds = &drawSurfaces[s];
        if (ds->lightmapNum[0] < 0) continue;

        mesh_t *patchMesh = NULL;
        vec3_t surfNormal = {0,0,0};
        if (ds->surfaceType == MST_PATCH) {
            patchMesh = SubdividePatchToLightmap(ds);
        } else if (ds->numVerts > 0) {
            VectorCopy(drawVerts[ds->firstVert].normal, surfNormal);
        } else {
            VectorSet(surfNormal, 0, 0, 1);
        }

        if (ds->surfaceType == MST_TRIANGLE_SOUP) {
            int numPoints = 0;
            voxelPoint_t *points = VoxelCache_Load(s, &numPoints);
            if (points) {
                for (int i = 0; i < numPoints; i++) {
                    int pIdx = points[i].pixelIndex;
                    int lmLocal = pIdx % (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT);
                    int lx = lmLocal % LIGHTMAP_WIDTH - ds->lightmapOffset[0][0];
                    int ly = lmLocal / LIGHTMAP_WIDTH - ds->lightmapOffset[0][1];

                    if (lx % rad_interval == 0 && ly % rad_interval == 0) {
                        if (radiosityFloats[pIdx * 3] == 0 && radiosityFloats[pIdx * 3 + 1] == 0 && radiosityFloats[pIdx * 3 + 2] == 0) continue;
                        
                        vec3_t pos, normal;
                        VectorCopy(points[i].pos, pos);
                        VectorCopy(points[i].normal, normal);
                        VectorAdd(pos, localSurfaces[s].entityOrigin, pos);
                        RadiosityVoxelAdd(pos, normal, &radiosityFloats[pIdx * 3]);
                    }
                }
                free(points);
                if (patchMesh) FreeMesh(patchMesh);
                continue;
            }
        }

        for (int ly = 0; ly < ds->lightmapHeight; ly += rad_interval) {
            for (int lx = 0; lx < ds->lightmapWidth; lx += rad_interval) {
                int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
                if (lightAlphaMask && !lightAlphaMask[k_dst]) continue;

                if (radiosityFloats[k_dst * 3] == 0 && radiosityFloats[k_dst * 3 + 1] == 0 && radiosityFloats[k_dst * 3 + 2] == 0) continue;

                vec3_t pos, normal;
                if (ds->surfaceType == MST_PATCH) {
                    if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                        drawVert_t *dv = &patchMesh->verts[ly * patchMesh->width + lx];
                        VectorCopy(dv->xyz, pos);
                        VectorCopy(dv->normal, normal);
                        VectorAdd(pos, localSurfaces[s].entityOrigin, pos);
                    } else {
                        continue;
                    }
                } else {
                    VectorMA(ds->lightmapOrigin, (float)lx + 0.5f, ds->lightmapVecs[0], pos);
                    VectorMA(pos, (float)ly + 0.5f, ds->lightmapVecs[1], pos);
                    VectorAdd(pos, localSurfaces[s].entityOrigin, pos);
                    VectorCopy(surfNormal, normal);
                }
                RadiosityVoxelAdd(pos, normal, &radiosityFloats[k_dst * 3]);
            }
        }
        if (patchMesh) FreeMesh(patchMesh);
    }

    size_t total = (size_t)g_radVoxelDims[0] * g_radVoxelDims[1] * g_radVoxelDims[2];
    for (size_t i = 0; i < total; i++) {
        radVoxel_t *v = &g_radVoxels[i];
        while (v) {
            if (v->weight > 0) {
                VectorScale(v->color, 1.0f / v->weight, v->color);
                // Safety: only normalize if the summed normal is non-zero
                if (VectorLength(v->normal) > 0.0001f) {
                    VectorNormalize(v->normal, v->normal);
                } else {
                    VectorSet(v->normal, 0, 0, 1); // Fallback
                }
            }
            v = v->next;
        }
    }
    _printf("done\n");
}

// ---------------------------------------------------------------------------
// Phase 3 — Reconstruction (Trilinear Interpolated Sampling)
// ---------------------------------------------------------------------------

// Helper: Bilinearly interpolate irradiance vectors from the sparse grid, then apply
// the per-texel normal to produce the final color. This is the key step that fixes
// faceting on curved patches: instead of interpolating flat RGB, we interpolate the
// directional irradiance and re-apply the correct normal at each texel.
static void RadiosityBilinearSample(dsurface_t *ds, int lx, int ly, const vec3_t normal, vec3_t outColor) {
    int x0 = (lx / rad_interval) * rad_interval;
    int x1 = x0 + rad_interval;
    int y0 = (ly / rad_interval) * rad_interval;
    int y1 = y0 + rad_interval;

    if (x1 >= ds->lightmapWidth)  x1 = x0;
    if (y1 >= ds->lightmapHeight) y1 = y0;

    float fx = (float)(lx - x0) / (float)rad_interval;
    float fy = (float)(ly - y0) / (float)rad_interval;

    int k00 = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0);
    int k10 = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1);
    int k01 = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0);
    int k11 = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1);

    // Fallback if irradiance vectors are not available (non-deluxe mode)
    if (!irradianceVecFloats) {
        int idx00 = k00 * 3;
        int idx10 = k10 * 3;
        int idx01 = k01 * 3;
        int idx11 = k11 * 3;
        for (int c = 0; c < 3; c++) {
            float row0 = radiosityFloats[idx00 + c] * (1.0f - fx) + radiosityFloats[idx10 + c] * fx;
            float row1 = radiosityFloats[idx01 + c] * (1.0f - fx) + radiosityFloats[idx11 + c] * fx;
            outColor[c] = row0 * (1.0f - fy) + row1 * fy;
        }
        return;
    }

    // Bilinearly interpolate each of the 3 per-channel irradiance vec3s, then dot with normal.
    for (int c = 0; c < 3; c++) {
        float *iv00 = &irradianceVecFloats[k00 * 9 + c * 3];
        float *iv10 = &irradianceVecFloats[k10 * 9 + c * 3];
        float *iv01 = &irradianceVecFloats[k01 * 9 + c * 3];
        float *iv11 = &irradianceVecFloats[k11 * 9 + c * 3];

        vec3_t row0, row1, interp;
        for (int a = 0; a < 3; a++) {
            row0[a] = iv00[a] * (1.0f - fx) + iv10[a] * fx;
            row1[a] = iv01[a] * (1.0f - fx) + iv11[a] * fx;
            interp[a] = row0[a] * (1.0f - fy) + row1[a] * fy;
        }
        float val = DotProduct(normal, interp);
        outColor[c] = (val > 0.0f) ? val : 0.0f;
    }
}


static qboolean RadiosityVoxelSample(const vec3_t pos, const vec3_t normal, vec3_t outColor) {
    int v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = (int)((pos[i] - g_radVoxelMins[i]) / rad_voxel_size);
        if (v[i] < 0 || v[i] >= g_radVoxelDims[i]) return qfalse;
    }

    vec3_t totalColor = {0,0,0};
    float totalWeight = 0.0f;

    for (int dx = -1; dx <= 1; dx++) {
        int nx = v[0] + dx;
        if (nx < 0 || nx >= g_radVoxelDims[0]) continue;
        for (int dy = -1; dy <= 1; dy++) {
            int ny = v[1] + dy;
            if (ny < 0 || ny >= g_radVoxelDims[1]) continue;
            for (int dz = -1; dz <= 1; dz++) {
                int nz = v[2] + dz;
                if (nz < 0 || nz >= g_radVoxelDims[2]) continue;
                
                size_t idx = RAD_VOXEL_INDEX(nx, ny, nz);
                radVoxel_t *curr = &g_radVoxels[idx];
                while (curr) {
                    if (curr->weight <= 0) {
                        curr = curr->next;
                        continue;
                    }
                    float dot = DotProduct(curr->normal, normal);
                    if (dot > rad_angle_match_cos) {
                        vec3_t voxelCenter;
                        voxelCenter[0] = g_radVoxelMins[0] + (nx + 0.5f) * rad_voxel_size;
                        voxelCenter[1] = g_radVoxelMins[1] + (ny + 0.5f) * rad_voxel_size;
                        voxelCenter[2] = g_radVoxelMins[2] + (nz + 0.5f) * rad_voxel_size;
                        
                        vec3_t delta;
                        VectorSubtract(pos, voxelCenter, delta);
                        float d = VectorLength(delta);
                        float w = (1.0f / (d + 1.0f)) * dot;
                        VectorMA(totalColor, w, curr->color, totalColor);
                        totalWeight += w;
                    }
                    curr = curr->next;
                }
            }
        }
    }


    if (totalWeight < 0.0001f) return qfalse;

    VectorScale(totalColor, 1.0f / totalWeight, outColor);
    return qtrue;
}

static void RadiosityReconstructOneSurface(int surfIdx) {
    dsurface_t *ds = &drawSurfaces[surfIdx];
    if (ds->lightmapNum[0] < 0) return;

    radFillMode_t mode = localSurfaces[surfIdx].radFillMode;

    if (mode == RAD_FILL_DEFAULT) {
        // Voxel fill is designed for MST_TRIANGLE_SOUP (no direct UV→world mapping).
        // Patches and planar surfaces have a proper subdivided mesh / lightmapVecs,
        // so they use the faster irradiance-vector bilinear path.
        if (ds->surfaceType == MST_TRIANGLE_SOUP && rad_voxel) {
            mode = RAD_FILL_VOXEL;
        } else {
            mode = RAD_FILL_BILINEAR;
        }
    }

    mesh_t *patchMesh = NULL;
    vec3_t surfNormal = {0,0,0};
    if (ds->surfaceType == MST_PATCH) {
        patchMesh = SubdividePatchToLightmap(ds);
    } else if (ds->numVerts > 0) {
        VectorCopy(drawVerts[ds->firstVert].normal, surfNormal);
    } else {
        VectorSet(surfNormal, 0, 0, 1);
    }

    int numPixels = ds->lightmapWidth * ds->lightmapHeight;
    if (numPixels <= 0) {
        if (patchMesh) FreeMesh(patchMesh);
        return;
    }

    vec3_t *tempBuffer = malloc(numPixels * sizeof(vec3_t));
    
    if (!tempBuffer) {
        if (patchMesh) FreeMesh(patchMesh);
        return;
    }

    // Initialize tempBuffer with existing values for all surfaces
    for (int ly = 0; ly < ds->lightmapHeight; ly++) {
        for (int lx = 0; lx < ds->lightmapWidth; lx++) {
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
            int k_temp = ly * ds->lightmapWidth + lx;
            VectorCopy(&radiosityFloats[k_dst * 3], tempBuffer[k_temp]);
        }
    }

    // -----------------------------------------------------------------------
    // Optimized Triangle Soup Path: Cache-driven + Inclusive Dilation
    // -----------------------------------------------------------------------
    if (mode == RAD_FILL_VOXEL && ds->surfaceType == MST_TRIANGLE_SOUP) {
        int numPoints = 0;
        voxelPoint_t *points = VoxelCache_Load(surfIdx, &numPoints);
        if (points) {
            byte *filled = calloc(numPixels, 1);
            vec3_t *pointPos = malloc(numPixels * sizeof(vec3_t));
            vec3_t *pointNorm = malloc(numPixels * sizeof(vec3_t));

            // Step 1: Exact hits via Voxel Cache
            for (int i = 0; i < numPoints; i++) {
                int pIdx = points[i].pixelIndex;
                int lmLocal = pIdx % (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT);
                int lx = lmLocal % LIGHTMAP_WIDTH - ds->lightmapOffset[0][0];
                int ly = lmLocal / LIGHTMAP_WIDTH - ds->lightmapOffset[0][1];
                int k_temp = ly * ds->lightmapWidth + lx;

                if (k_temp < 0 || k_temp >= numPixels) continue;

                VectorCopy(points[i].pos, pointPos[k_temp]);
                VectorCopy(points[i].normal, pointNorm[k_temp]);
                VectorAdd(pointPos[k_temp], localSurfaces[surfIdx].entityOrigin, pointPos[k_temp]);

                if (RadiosityVoxelSample(pointPos[k_temp], pointNorm[k_temp], tempBuffer[k_temp])) {
                    filled[k_temp] = 1;
                }
            }

            // Step 2: Inclusive Dilation (Near-miss pixels)
            for (int ly = 0; ly < ds->lightmapHeight; ly++) {
                for (int lx = 0; lx < ds->lightmapWidth; lx++) {
                    int k_temp = ly * ds->lightmapWidth + lx;
                    if (filled[k_temp]) continue;

                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = lx + dx, ny = ly + dy;
                            if (nx < 0 || nx >= ds->lightmapWidth || ny < 0 || ny >= ds->lightmapHeight) continue;
                            int kn = ny * ds->lightmapWidth + nx;
                            if (filled[kn] == 1) { // Borrow from original geometric hits
                                if (RadiosityVoxelSample(pointPos[kn], pointNorm[kn], tempBuffer[k_temp])) {
                                    filled[k_temp] = 2; // Dilated
                                    goto next_p;
                                }
                            }
                        }
                    }
                    next_p:;
                }
            }

            free(filled); free(pointPos); free(pointNorm); free(points);
            goto flush; // Skip manual rasterization loop
        }
    }

    for (int ly = 0; ly < ds->lightmapHeight; ly++) {
        for (int lx = 0; lx < ds->lightmapWidth; lx++) {
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
            int k_temp = ly * ds->lightmapWidth + lx;
            
            if (lightAlphaMask && !lightAlphaMask[k_dst]) continue;

            // Resolve the per-texel normal for this pixel.
            vec3_t texelNormal;
            if (ds->surfaceType == MST_PATCH) {
                if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                    VectorCopy(patchMesh->verts[ly * patchMesh->width + lx].normal, texelNormal);
                } else {
                    VectorCopy(surfNormal, texelNormal);
                }
            } else {
                VectorCopy(surfNormal, texelNormal);
            }

            if (mode == RAD_FILL_BILINEAR) {
                if (ds->surfaceType == MST_PATCH) {
                    RadiosityBilinearSample(ds, lx, ly, texelNormal, tempBuffer[k_temp]);
                } else if (lx % rad_interval == 0 && ly % rad_interval == 0) {
                    continue; // Planar: sparse-grid pixels are already exact, keep them.
                } else {
                    RadiosityBilinearSample(ds, lx, ly, texelNormal, tempBuffer[k_temp]);
                }
                continue;
            }

            if (mode == RAD_FILL_VOXEL) {
                vec3_t pos, normal;
                if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                    // Fallback path if cache was missing
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + 0.5f;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + 0.5f;
                    if (!TriSoupSamplePoint(ds, st, pos, normal)) continue;
                    VectorAdd(pos, localSurfaces[surfIdx].entityOrigin, pos);
                } else if (ds->surfaceType == MST_PATCH) {
                    if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                        drawVert_t *dv = &patchMesh->verts[ly * patchMesh->width + lx];
                        VectorCopy(dv->xyz, pos);
                        VectorCopy(dv->normal, normal);
                        VectorAdd(pos, localSurfaces[surfIdx].entityOrigin, pos);
                    } else {
                        continue;
                    }
                } else {
                    VectorMA(ds->lightmapOrigin, (float)lx + 0.5f, ds->lightmapVecs[0], pos);
                    VectorMA(pos, (float)ly + 0.5f, ds->lightmapVecs[1], pos);
                    VectorAdd(pos, localSurfaces[surfIdx].entityOrigin, pos);
                    VectorCopy(surfNormal, normal);
                }

                if (!RadiosityVoxelSample(pos, normal, tempBuffer[k_temp])) {
                    RadiosityBilinearSample(ds, lx, ly, normal, tempBuffer[k_temp]);
                }
            } else {
                RadiosityBilinearSample(ds, lx, ly, texelNormal, tempBuffer[k_temp]);
            }
        }
    }

flush:


    // Flush temp buffer back to radiosityFloats for this surface
    for (int ly = 0; ly < ds->lightmapHeight; ly++) {
        for (int lx = 0; lx < ds->lightmapWidth; lx++) {
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
            int k_temp = ly * ds->lightmapWidth + lx;
            VectorCopy(tempBuffer[k_temp], &radiosityFloats[k_dst * 3]);
        }
    }

    free(tempBuffer);
    if (patchMesh) FreeMesh(patchMesh);
}

static void RadiosityReconstructThread(int surfIdx) {
    RadiosityReconstructOneSurface(surfIdx);
}

// ---------------------------------------------------------------------------
// Phase 4 — Merge
// ---------------------------------------------------------------------------

static void RadiosityMerge(const float *srcBuffer) {
    int total = numLightBytes / 3;
    for (int i = 0; i < total; i++) {
        if (lightAlphaMask && !lightAlphaMask[i]) continue;
        VectorAdd(lightFloats + i * 3, srcBuffer + i * 3, lightFloats + i * 3);
    }
}

// ---------------------------------------------------------------------------
// LightRadiosity — public entry point
// ---------------------------------------------------------------------------

void LightRadiosity(int radiosityPasses) {
    if (radiosityPasses <= 0) return;
    _printf("--- Radiosity ---\n");

    qboolean anyVoxel = rad_voxel;
    for (int i = 0; i < numDrawSurfaces; i++) {
        if (localSurfaces[i].radFillMode == RAD_FILL_VOXEL) {
            anyVoxel = qtrue;
            break;
        }
    }

    if (anyVoxel) {
        if (rad_voxel_size <= 0.0f) {
            rad_voxel_size = (float)(samplesize * rad_interval);
        }
        if (rad_angle_match > 90.0f) rad_angle_match = 90.0f;
        rad_angle_match_cos = (float)cos(rad_angle_match * (M_PI / 180.0f));
        _printf("  Voxel Grid enabled (Size: %.1f, Angle Match: %.1f deg / %.2f cos)\n", 
                rad_voxel_size, rad_angle_match, rad_angle_match_cos);
    }

    AllocateRadiosityFloats();
    memset(accumRadiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

    for (int pnum = 1; pnum <= radiosityPasses; pnum++) {
        double passStart = I_FloatTime();
        _printf("Pass %d/%d:\n", pnum, radiosityPasses);

        const float *emitSource = (pnum == 1) ? lightFloats : radiosityFloats;
        _printf("  [emit]       ");
        RadiosityEmit(emitSource);

        memset(radiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
        if (irradianceVecFloats) memset(irradianceVecFloats, 0, (numLightBytes / 3) * 9 * sizeof(float));
        _printf("  [integrate]  %d emitters generated. Starting integration...\n", g_numEmitters);
        fflush(stdout);
        RunThreadsOnIndividual(numDrawSurfaces, qtrue, RadiosityIntegrateThread);
        _printf("done\n");

        if (anyVoxel) {
            RadiosityVoxelize();
        }
        
        _printf("  [reconstruct] Reconstruction Fill ");
        RunThreadsOnIndividual(numDrawSurfaces, qtrue, RadiosityReconstructThread);
        _printf("done\n");

        for (int i = 0; i < numLightBytes / 3; i++) VectorAdd(accumRadiosityFloats + i * 3, radiosityFloats + i * 3, accumRadiosityFloats + i * 3);
        free(g_emitters); g_emitters = NULL; g_numEmitters = 0;
        _printf("  Pass %d complete (%.0f seconds)\n\n", pnum, I_FloatTime() - passStart);
    }

    _printf("--- Radiosity Merge ---\n");
    RadiosityMerge(accumRadiosityFloats);
    _printf("done\n");

    FreeRadiosityFloats();
    RadiosityVoxelReset();
}
