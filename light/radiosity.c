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

static radVoxel_t ***g_radVoxels = NULL;
static vec3_t g_radVoxelMins;
static int g_radVoxelDims[3];

static void RadiosityVoxelReset(void) {
    if (g_radVoxels) {
        for (int x = 0; x < g_radVoxelDims[0]; x++) {
            for (int y = 0; y < g_radVoxelDims[1]; y++) {
                for (int z = 0; z < g_radVoxelDims[2]; z++) {
                    radVoxel_t *v = g_radVoxels[x][y][z].next;
                    while(v) {
                        radVoxel_t *next = v->next;
                        free(v);
                        v = next;
                    }
                }
                free(g_radVoxels[x][y]);
            }
            free(g_radVoxels[x]);
        }
        free(g_radVoxels);
        g_radVoxels = NULL;
    }

    // Initialize dimensions based on map bounds
    for (int i = 0; i < 3; i++) {
        g_radVoxelMins[i] = dmodels[0].mins[i] - rad_voxel_size;
        float size = (dmodels[0].maxs[i] + rad_voxel_size) - g_radVoxelMins[i];
        g_radVoxelDims[i] = (int)ceil(size / rad_voxel_size);
    }

    g_radVoxels = malloc(sizeof(radVoxel_t**) * g_radVoxelDims[0]);
    for (int x = 0; x < g_radVoxelDims[0]; x++) {
        g_radVoxels[x] = malloc(sizeof(radVoxel_t*) * g_radVoxelDims[1]);
        for (int y = 0; y < g_radVoxelDims[1]; y++) {
            g_radVoxels[x][y] = calloc(g_radVoxelDims[2], sizeof(radVoxel_t));
        }
    }
}

// Helper: Add a sample to the voxel grid
static void RadiosityVoxelAdd(const vec3_t pos, const vec3_t normal, const vec3_t color) {
    int v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = (int)((pos[i] - g_radVoxelMins[i]) / rad_voxel_size);
        if (v[i] < 0 || v[i] >= g_radVoxelDims[i]) return;
    }

    radVoxel_t *head = &g_radVoxels[v[0]][v[1]][v[2]];
    
    // Find a voxel bucket with a similar normal to avoid bleeding through walls
    radVoxel_t *curr = head;
    while (curr) {
        if (curr->weight > 0 && DotProduct(curr->normal, normal) > rad_angle_match_cos) {
            VectorAdd(curr->color, color, curr->color);
            VectorAdd(curr->normal, normal, curr->normal); // Accumulate normal for averaging
            curr->weight += 1.0f;
            return;
        }
        if (!curr->next) break;
        curr = curr->next;
    }

    // Create new normal-specific bucket in this voxel
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

        mesh_t *patchMesh = NULL;
        vec3_t surfNormal;
        if (ds->surfaceType == MST_PATCH) {
            patchMesh = SubdividePatchToLightmap(ds);
        } else if (ds->surfaceType == MST_PLANAR) {
            CrossProduct(ds->lightmapVecs[0], ds->lightmapVecs[1], surfNormal);
            VectorNormalize(surfNormal, surfNormal);
        } else {
            if (ds->numVerts < 3) continue;
            VectorCopy(drawVerts[ds->firstVert].normal, surfNormal);
        }

        // ACCURATE LUXEL AREA
        float luxelArea;
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
                    VectorAdd(em->center, surfaceOrigin[i], em->center);
                } else if (ds->surfaceType == MST_PATCH) {
                    if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                        drawVert_t *dv = &patchMesh->verts[ly * patchMesh->width + lx];
                        VectorCopy(dv->normal, em->normal);
                        VectorMA(dv->xyz, RAD_ORIGIN_NUDGE, em->normal, em->center);
                        VectorAdd(em->center, surfaceOrigin[i], em->center);
                    } else {
                        VectorClear(em->normal);
                        VectorClear(em->center);
                    }
                } else {
                    VectorMA(ds->lightmapOrigin, (float)lx + (float)rad_interval * 0.5f, ds->lightmapVecs[0], em->center);
                    VectorMA(em->center, (float)ly + (float)rad_interval * 0.5f, ds->lightmapVecs[1], em->center);
                    VectorMA(em->center, RAD_ORIGIN_NUDGE, surfNormal, em->center);
                    VectorAdd(em->center, surfaceOrigin[i], em->center);
                    VectorCopy(surfNormal, em->normal);
                }
                
                em->area = luxelArea * (float)(rad_interval * rad_interval);
                VectorScale(src, rad_bounce_scale, em->color);
                for (k = 0; k < 3; k++) em->color[k] *= albedo[k];
            }
        }
        if (patchMesh) FreeMesh(patchMesh);
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

    for (int ly = 0; ly < ds->lightmapHeight; ly += rad_interval) {
        for (int lx = 0; lx < ds->lightmapWidth; lx += rad_interval) {
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
            if (lightAlphaMask && !lightAlphaMask[k_dst]) continue;

            vec3_t dst;
            if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                float st[2];
                st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + 0.5f;
                st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + 0.5f;
                if (!TriSoupSamplePoint(ds, st, dst, dstNormal)) continue;
                VectorMA(dst, RAD_ORIGIN_NUDGE, dstNormal, dst);
                VectorAdd(dst, surfaceOrigin[surfIdx], dst);
            } else if (ds->surfaceType == MST_PATCH) {
                if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                    drawVert_t *dv = &patchMesh->verts[ly * patchMesh->width + lx];
                    VectorCopy(dv->normal, dstNormal);
                    VectorMA(dv->xyz, RAD_ORIGIN_NUDGE, dstNormal, dst);
                    VectorAdd(dst, surfaceOrigin[surfIdx], dst);
                } else {
                    continue;
                }
            } else {
                VectorMA(ds->lightmapOrigin, (float)lx + 0.5f, ds->lightmapVecs[0], dst);
                VectorMA(dst, (float)ly + 0.5f, ds->lightmapVecs[1], dst);
                VectorMA(dst, RAD_ORIGIN_NUDGE, dstNormal, dst);
                VectorAdd(dst, surfaceOrigin[surfIdx], dst);
            }

            float accum[3] = {0, 0, 0};
            for (int e = 0; e < g_numEmitters; e++) {
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

                if (dist < rad_depth_min) formFactor *= rad_depth_intensity;
                else if (dist < rad_depth_max) {
                    float lerp = (dist - rad_depth_min) / (rad_depth_max - rad_depth_min);
                    formFactor *= rad_depth_intensity + (1.0f - rad_depth_intensity) * lerp;
                }
                if (formFactor > 1.0f) formFactor = 1.0f;

                if (!RadVisCheck(dst, em->center)) continue;
                VectorMA(accum, formFactor, em->color, accum);
            }

            if (accum[0] > 0 || accum[1] > 0 || accum[2] > 0) {
                ThreadLock();
                VectorAdd(&radiosityFloats[k_dst * 3], accum, &radiosityFloats[k_dst * 3]);
                ThreadUnlock();
            }
        }
    }
    if (patchMesh) FreeMesh(patchMesh);
}

static void RadiosityIntegrateThread(int surfIdx) {
    RadiosityIntegrateOneSurface(surfIdx);
}

// ---------------------------------------------------------------------------
// Phase 2.5 — Voxelize
// ---------------------------------------------------------------------------

static void RadiosityVoxelize(void) {
    _printf("  [voxelize]   Unified world-space splatting... ");
    RadiosityVoxelReset();

    for (int s = 0; s < numDrawSurfaces; s++) {
        dsurface_t *ds = &drawSurfaces[s];
        if (ds->lightmapNum[0] < 0) continue;

        mesh_t *patchMesh = NULL;
        vec3_t surfNormal;
        if (ds->surfaceType == MST_PATCH) {
            patchMesh = SubdividePatchToLightmap(ds);
        } else if (ds->numVerts > 0) {
            VectorCopy(drawVerts[ds->firstVert].normal, surfNormal);
        } else {
            VectorSet(surfNormal, 0, 0, 1);
        }

        for (int ly = 0; ly < ds->lightmapHeight; ly += rad_interval) {
            for (int lx = 0; lx < ds->lightmapWidth; lx += rad_interval) {
                int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
                if (lightAlphaMask && !lightAlphaMask[k_dst]) continue;

                if (radiosityFloats[k_dst * 3] == 0 && radiosityFloats[k_dst * 3 + 1] == 0 && radiosityFloats[k_dst * 3 + 2] == 0) continue;

                vec3_t pos, normal;
                if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + 0.5f;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + 0.5f;
                    if (!TriSoupSamplePoint(ds, st, pos, normal)) continue;
                    VectorAdd(pos, surfaceOrigin[s], pos);
                } else if (ds->surfaceType == MST_PATCH) {
                    if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                        drawVert_t *dv = &patchMesh->verts[ly * patchMesh->width + lx];
                        VectorCopy(dv->xyz, pos);
                        VectorCopy(dv->normal, normal);
                        VectorAdd(pos, surfaceOrigin[s], pos);
                    } else {
                        continue;
                    }
                } else {
                    VectorMA(ds->lightmapOrigin, (float)lx + 0.5f, ds->lightmapVecs[0], pos);
                    VectorMA(pos, (float)ly + 0.5f, ds->lightmapVecs[1], pos);
                    VectorAdd(pos, surfaceOrigin[s], pos);
                    VectorCopy(surfNormal, normal);
                }
                RadiosityVoxelAdd(pos, normal, &radiosityFloats[k_dst * 3]);
            }
        }
        if (patchMesh) FreeMesh(patchMesh);
    }

    for (int x = 0; x < g_radVoxelDims[0]; x++) {
        for (int y = 0; y < g_radVoxelDims[1]; y++) {
            for (int z = 0; z < g_radVoxelDims[2]; z++) {
                radVoxel_t *v = g_radVoxels[x][y][z].next;
                while (v) {
                    if (v->weight > 0) {
                        VectorScale(v->color, 1.0f / v->weight, v->color);
                        VectorNormalize(v->normal, v->normal); // Normalize the averaged surface normal
                    }
                    v = v->next;
                }
            }
        }
    }
    _printf("done\n");
}

// ---------------------------------------------------------------------------
// Phase 3 — Reconstruction (Trilinear Interpolated Sampling)
// ---------------------------------------------------------------------------

// Helper: Perform local 2D bilinear interpolation from the sparse integrated grid.
static void RadiosityBilinearSample(dsurface_t *ds, int lx, int ly, vec3_t outColor) {
    int x0 = (lx / rad_interval) * rad_interval;
    int x1 = x0 + rad_interval;
    int y0 = (ly / rad_interval) * rad_interval;
    int y1 = y0 + rad_interval;

    if (x1 >= ds->lightmapWidth)  x1 = x0;
    if (y1 >= ds->lightmapHeight) y1 = y0;

    float fx = (float)(lx - x0) / (float)rad_interval;
    float fy = (float)(ly - y0) / (float)rad_interval;

    float *p00 = &radiosityFloats[((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0) * 3];
    float *p10 = &radiosityFloats[((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1) * 3];
    float *p01 = &radiosityFloats[((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0) * 3];
    float *p11 = &radiosityFloats[((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1) * 3];

    for (int k = 0; k < 3; k++) {
        float row0 = p00[k] * (1.0f - fx) + p10[k] * fx;
        float row1 = p01[k] * (1.0f - fx) + p11[k] * fx;
        outColor[k] = row0 * (1.0f - fy) + row1 * fy;
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
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                int nx = v[0] + dx, ny = v[1] + dy, nz = v[2] + dz;
                if (nx < 0 || nx >= g_radVoxelDims[0] || ny < 0 || ny >= g_radVoxelDims[1] || nz < 0 || nz >= g_radVoxelDims[2]) continue;
                
                radVoxel_t *curr = g_radVoxels[nx][ny][nz].next;
                while (curr) {
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

    radFillMode_t mode = GetSurfaceExtraRadFillMode(surfIdx);

    if (mode == RAD_FILL_DEFAULT) {
        mode = rad_voxel ? RAD_FILL_VOXEL : RAD_FILL_BILINEAR;
    }

    mesh_t *patchMesh = NULL;
    vec3_t surfNormal;
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

    for (int ly = 0; ly < ds->lightmapHeight; ly++) {
        for (int lx = 0; lx < ds->lightmapWidth; lx++) {
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
            int k_temp = ly * ds->lightmapWidth + lx;
            
            // Default to the original Phase 2 integrated value
            VectorCopy(&radiosityFloats[k_dst * 3], tempBuffer[k_temp]);

            if (lightAlphaMask && !lightAlphaMask[k_dst]) continue;

            // If bilinear mode, preserve original precise irradiance at grid intersection points
            if (mode == RAD_FILL_BILINEAR && (lx % rad_interval == 0 && ly % rad_interval == 0)) {
                continue; 
            }

            if (mode == RAD_FILL_VOXEL) {
                vec3_t pos, normal;
                if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + 0.5f;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + 0.5f;
                    if (!TriSoupSamplePoint(ds, st, pos, normal)) continue;
                    VectorAdd(pos, surfaceOrigin[surfIdx], pos);
                } else if (ds->surfaceType == MST_PATCH) {
                    if (patchMesh && lx < patchMesh->width && ly < patchMesh->height) {
                        drawVert_t *dv = &patchMesh->verts[ly * patchMesh->width + lx];
                        VectorCopy(dv->xyz, pos);
                        VectorCopy(dv->normal, normal);
                        VectorAdd(pos, surfaceOrigin[surfIdx], pos);
                    } else {
                        continue;
                    }
                } else {
                    VectorMA(ds->lightmapOrigin, (float)lx + 0.5f, ds->lightmapVecs[0], pos);
                    VectorMA(pos, (float)ly + 0.5f, ds->lightmapVecs[1], pos);
                    VectorAdd(pos, surfaceOrigin[surfIdx], pos);
                    VectorCopy(surfNormal, normal);
                }

                if (!RadiosityVoxelSample(pos, normal, tempBuffer[k_temp])) {
                    RadiosityBilinearSample(ds, lx, ly, tempBuffer[k_temp]);
                }
            } else {
                RadiosityBilinearSample(ds, lx, ly, tempBuffer[k_temp]);
            }
        }
    }

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

    if (!embree) {
        _printf("WARNING: Radiosity is only supported with the Embree backend. Skipping.\n");
        return;
    }

    qboolean anyVoxel = rad_voxel;
    for (int i = 0; i < numDrawSurfaces; i++) {
        if (GetSurfaceExtraRadFillMode(i) == RAD_FILL_VOXEL) {
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

    _printf("  [merge]      Finalizing cumulative GI merging... ");
    RadiosityMerge(accumRadiosityFloats);
    _printf("done\n");

    FreeRadiosityFloats();
    RadiosityVoxelReset();
}
