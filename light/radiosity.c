/*
===========================================================================
radiosity.c — Global Illumination (Radiant Energy Bounce)

Architecture:
  Phase 1 - Emit:       Scan lightFloats, turn each lit luxel into a
                        world-space polygon area emitter.
  Phase 2 - Integrate:  For each destination luxel, gather irradiance
                        from all line-of-sight emitters using an analytic
                        closed-form area integral (no Monte Carlo).
  Phase 3 - Merge:      Additively blend radiosityFloats → lightFloats.
===========================================================================
*/

#include "light.h"
#include "radiosity.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tuning parameters (managed via main.c CLI)
// ---------------------------------------------------------------------------

float rad_bounce_scale  = 0.5f;   // Energy per bounce (conserved)
float rad_color_ratio   = 0.5f;   // Greyscale vs colour bleeding
float rad_min_energy    = 1.0f;   // Min brightness for emitters
float rad_min_dist      = MIN_RAD_DISTANCE * 2;  // Singularity guard (dist clamp)
int   rad_interval      = 4;      // Sparse grid resolution (4 = 4x4)
// #define RAD_PI  3.14159265358979323846f
// #define M_PI	3.14159265358979323846
#define RAD_PI M_PI

// Amount to nudge the emitter origin off the surface along its normal.
// Prevents the emitter from self-shadowing via Embree.
#define RAD_ORIGIN_NUDGE        1.5f

// light.h/c exports
qboolean TriSoupSamplePoint(dsurface_t *ds, float st[2], vec3_t origin, vec3_t normal);

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
// Helper: test line-of-sight between two world points via Embree.
// Returns qtrue if the path is CLEAR (not occluded).
// ---------------------------------------------------------------------------

static qboolean RadVisCheck(const vec3_t from, const vec3_t to) {
    struct RTCRayHit rayhit;
    struct RTCIntersectArguments iargs;
    vec3_t  dir;
    float   len;
    int     k;

    for (k = 0; k < 3; k++)
        dir[k] = to[k] - from[k];

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
    context.tw = NULL; // We don't have a specific surface context here

    rtcInitIntersectArguments(&iargs);
    iargs.context = &context.context;

    rtcIntersect1(g_scene, &rayhit, &iargs);

    return (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) ? qtrue : qfalse;
}

// ---------------------------------------------------------------------------
// Phase 1 — Emit
//
// Scans every lit planar/patch/model luxel in lightFloats and builds an
// emitter for it.  We only handle surfaces with a valid lightmap allocation.
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

        // Only surfaces with a valid colour lightmap contribute
        if (ds->lightmapNum[0] < 0)
            continue;
        if (ds->lightmapWidth <= 0 || ds->lightmapHeight <= 0)
            continue;

        // Compute surface normal from lightmapVecs cross-product (planar/patch)
        vec3_t surfNormal;
        if (ds->surfaceType == MST_PLANAR || ds->surfaceType == MST_PATCH) {
            CrossProduct(ds->lightmapVecs[0], ds->lightmapVecs[1], surfNormal);
            VectorNormalize(surfNormal, surfNormal);
        } else {
            // Triangle soup: use the normal from drawVerts (average of first tri)
            if (ds->numVerts < 3) continue;
            VectorCopy(drawVerts[ds->firstVert].normal, surfNormal);
        }

        // World-space area of a single luxel quad on this surface
        // ||V0 cross V1|| = area of the parallelogram, /2 per triangle
        // For a square luxel winding it's just ||V0|| * ||V1||.
        float luxelArea = VectorLength(ds->lightmapVecs[0]) *
                          VectorLength(ds->lightmapVecs[1]);

        // Surface albedo (average texture colour, normalised 0-1)
        vec3_t albedo;
        float avgLum = (si->averageColor[0] + si->averageColor[1] +
                        si->averageColor[2]) / (3.0f * 255.0f);
        for (k = 0; k < 3; k++) {
            float fullColor  = si->averageColor[k] / 255.0f;
            albedo[k] = fullColor * rad_color_ratio + avgLum * (1.0f - rad_color_ratio);
            if (albedo[k] < 0.0f) albedo[k] = 0.0f;
        }

        // Iterate over the luxel grid in sparse steps
        for (ly = 0; ly < ds->lightmapHeight; ly += rad_interval) {
            for (lx = 0; lx < ds->lightmapWidth; lx += rad_interval) {
                // Flat index into lightFloats
                int k_lm = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT +
                            ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH +
                            ds->lightmapOffset[0][0] + lx;

                float *src = (float *)&srcBuffer[k_lm * 3];

                // Skip luxels that aren't part of the world geometry
                if (lightAlphaMask && !lightAlphaMask[k_lm])
                    continue;

                // Skip dark/unlit luxels — they have nothing to bounce
                float lum = src[0] + src[1] + src[2];
                if (lum < rad_min_energy)
                    continue;

                if (g_numEmitters >= capacity) {
                    capacity *= 2;
                    emitter_t *temp = realloc(g_emitters, sizeof(emitter_t) * capacity);
                    if (!temp) {
                        free(g_emitters);
                        Error("RadiosityEmit: realloc failed at %d emitters", g_numEmitters);
                    }
                    g_emitters = temp;
                }

                emitter_t *em = &g_emitters[g_numEmitters++];

                // World-space centre of this virtual patch
                if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + (float)rad_interval * 0.5f;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + (float)rad_interval * 0.5f;
                    if (!TriSoupSamplePoint(ds, st, em->center, em->normal)) {
                        // If sampling fails (e.g. edge of triangle), use a safe fallback
                        for (k = 0; k < 3; k++) em->center[k] = 0;
                        VectorClear(em->normal);
                    }
                    // Apply nudge and offset
                    for (k = 0; k < 3; k++) {
                        em->center[k] += em->normal[k] * RAD_ORIGIN_NUDGE;
                        em->center[k] += surfaceOrigin[i][k];
                    }
                } else {
                    for (k = 0; k < 3; k++) {
                        em->center[k] = ds->lightmapOrigin[k]
                                       + ((float)lx + (float)rad_interval * 0.5f) * ds->lightmapVecs[0][k]
                                       + ((float)ly + (float)rad_interval * 0.5f) * ds->lightmapVecs[1][k]
                                       + surfNormal[k] * RAD_ORIGIN_NUDGE;
                        em->center[k] += surfaceOrigin[i][k];
                    }
                    VectorCopy(surfNormal, em->normal);
                }
                
                // One sparse emitter represents a rad_interval x rad_interval block area
                em->area = luxelArea * (float)(rad_interval * rad_interval);

                // Effective bounce colour = source * albedo * globalScale
                for (k = 0; k < 3; k++)
                    em->color[k] = src[k] * albedo[k] * rad_bounce_scale;

            }
        }
    }

    _printf("    %d emitters generated from lit luxels\n", g_numEmitters);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Phase 2 — Integrate (per-surface thread worker)
//
// For each destination luxel on a surface, accumulate irradiance from all
// visible emitters using the analytic area-light form-factor formula.
// ---------------------------------------------------------------------------

static void RadiosityIntegrateOneSurface(int surfIdx) {
    dsurface_t   *ds = &drawSurfaces[surfIdx];

    if (ds->lightmapNum[0] < 0)
        return;
    if (ds->lightmapWidth <= 0 || ds->lightmapHeight <= 0)
        return;
    if (g_numEmitters <= 0 || !g_emitters)
        return;

    // Destination surface normal
    vec3_t dstNormal;
    if (ds->surfaceType == MST_PLANAR || ds->surfaceType == MST_PATCH) {
        CrossProduct(ds->lightmapVecs[0], ds->lightmapVecs[1], dstNormal);
        if (VectorNormalize(dstNormal, dstNormal) < 0.0001f) {
            VectorCopy(drawVerts[ds->firstVert].normal, dstNormal);
        }
    } else {
        if (ds->numVerts < 3) return;
        VectorCopy(drawVerts[ds->firstVert].normal, dstNormal);
    }

    int lx, ly;
    for (ly = 0; ly < ds->lightmapHeight; ly += rad_interval) {
        for (lx = 0; lx < ds->lightmapWidth; lx += rad_interval) {

            // Flat index into radiosityFloats
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT +
                         ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH +
                         ds->lightmapOffset[0][0] + lx;

            // Bounds guard for safety
            if (k_dst < 0 || k_dst >= numLightBytes / 3)
                continue;

            // Only integrate into luxels that the alpha mask covers
            if (lightAlphaMask && !lightAlphaMask[k_dst])
                continue;

            // World-space destination point
            vec3_t dst;
            if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                float st[2];
                st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + 0.5f;
                st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + 0.5f;
                if (!TriSoupSamplePoint(ds, st, dst, dstNormal)) {
                    continue;
                }
                for (int k = 0; k < 3; k++) {
                    dst[k] += dstNormal[k] * RAD_ORIGIN_NUDGE;
                    dst[k] += surfaceOrigin[surfIdx][k];
                }
            } else {
                for (int k = 0; k < 3; k++) {
                    dst[k] = ds->lightmapOrigin[k]
                            + ((float)lx + 0.5f) * ds->lightmapVecs[0][k]
                            + ((float)ly + 0.5f) * ds->lightmapVecs[1][k]
                            + dstNormal[k] * RAD_ORIGIN_NUDGE;
                    dst[k] += surfaceOrigin[surfIdx][k];
                }
            }

            // Accumulate irradiance from all emitters
            float accum[3] = {0, 0, 0};
            // _printf("  luxel %d %d\n", lx, ly);

            for (int e = 0; e < g_numEmitters; e++) {
                emitter_t *em = &g_emitters[e];

                // --- Direction from destination to emitter ---
                vec3_t ray;
                for (int k = 0; k < 3; k++)
                    ray[k] = em->center[k] - dst[k];

                float dist = VectorLength(ray);
                if (dist < 0.001f)
                    continue;

                // Normalise direction
                vec3_t rayDir;
                VectorScale(ray, 1.0f / dist, rayDir);

                // --- Backface culls ---
                // The emitter must be facing the destination (hemisphere check)
                float cosEmit = -DotProduct(em->normal, rayDir);
                if (cosEmit <= 0.0f)
                    continue;

                // The destination must be facing the emitter
                float cosDst = DotProduct(dstNormal, rayDir);
                if (cosDst <= 0.0f)
                    continue;

                // --- Singularity guard (Source Engine trick) ---
                // Clamp distance so nearby emitters can't blow up the integral.
                float distClamped = dist < rad_min_dist ? rad_min_dist : dist;

                // --- Analytic form-factor (double-cosine) ---
                // Lambertian area-to-point transfer:
                //   dE = (L * area * cosEmit * cosDst) / (π * dist²)
                float formFactor = (em->area * cosEmit * cosDst) /
                                   (RAD_PI * distClamped * distClamped);

                // --- Graduate attenuation (Min Distance soft-clamping) ---
                // MIN_RAD_DISTANCE = Hard floor (0 light).
                // Scale from 0 at MIN_RAD_DISTANCE up to 1.0 (full light) at rad_min_dist.
                if (dist < MIN_RAD_DISTANCE) {
                    formFactor = 0.0f;
                } else if (dist < rad_min_dist) {
                    float factor = (dist - MIN_RAD_DISTANCE) / (rad_min_dist - MIN_RAD_DISTANCE);
                    formFactor *= factor;
                }

                // Cap the form factor to prevent "leakage" at grazing angles
                if (formFactor > 1.0f) formFactor = 1.0f;

                // --- Visibility test (single Embree shadow ray) ---
                if (!RadVisCheck(dst, em->center))
                    continue;

                // --- Accumulate ---
                for (int k = 0; k < 3; k++)
                    accum[k] += em->color[k] * formFactor;
            }

            // Write accumulated GI into the isolated radiosity buffer
            if (accum[0] > 0 || accum[1] > 0 || accum[2] > 0) {
                if (k_dst >= 0 && k_dst < numLightBytes / 3) {
                    ThreadLock();
                    radiosityFloats[k_dst * 3 + 0] += accum[0];
                    radiosityFloats[k_dst * 3 + 1] += accum[1];
                    radiosityFloats[k_dst * 3 + 2] += accum[2];
                    ThreadUnlock();
                }
            }
        }
    }
}

// Thread entry point — called via RunThreadsOnIndividual
static void RadiosityIntegrateThread(int surfIdx) {
    RadiosityIntegrateOneSurface(surfIdx);
}

// ---------------------------------------------------------------------------
// Phase 3 — Reconstruction (Bilinear Fill)
//
// Reconstructs full-resolution GI from the 4x4 sparse grid computed in Phase 2.
// ---------------------------------------------------------------------------

static void RadiosityBilinearFillOneSurface(int surfIdx) {
    dsurface_t *ds = &drawSurfaces[surfIdx];
    int lx, ly;

    if (ds->lightmapNum[0] < 0)
        return;

    for (ly = 0; ly < ds->lightmapHeight; ly++) {
        for (lx = 0; lx < ds->lightmapWidth; lx++) {
            // Already computed grid point
            if (lx % rad_interval == 0 && ly % rad_interval == 0)
                continue;

            int k_dst_pix = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
            if (k_dst_pix < 0 || k_dst_pix >= numLightBytes / 3)
                continue;
            if (lightAlphaMask && !lightAlphaMask[k_dst_pix])
                continue;

            // Identification of the 4 surrounding sparse grid points
            int x0 = (lx / rad_interval) * rad_interval;
            int x1 = x0 + rad_interval;
            int y0 = (ly / rad_interval) * rad_interval;
            int y1 = y0 + rad_interval;

            // Clamp to surface bounds
            if (x1 >= ds->lightmapWidth)  x1 = x0;
            if (y1 >= ds->lightmapHeight) y1 = y0;

            float fx = (float)(lx - x0) / (float)rad_interval;
            float fy = (float)(ly - y0) / (float)rad_interval;

            float *p00 = &radiosityFloats[((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0) * 3];
            float *p10 = &radiosityFloats[((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1) * 3];
            float *p01 = &radiosityFloats[((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0) * 3];
            float *p11 = &radiosityFloats[((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1) * 3];

            int k_idx_dst = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx);
            if (k_idx_dst < 0 || k_idx_dst >= numLightBytes / 3)
                continue;

            int k_dst = k_idx_dst * 3;
            
            for (int k = 0; k < 3; k++) {
                float row0 = p00[k] * (1.0f - fx) + p10[k] * fx;
                float row1 = p01[k] * (1.0f - fx) + p11[k] * fx;
                radiosityFloats[k_dst + k] = row0 * (1.0f - fy) + row1 * fy;
            }
        }
    }
}

static void RadiosityBilinearFillThread(int surfIdx) {
    RadiosityBilinearFillOneSurface(surfIdx);
}

static void RadiosityReconstruct(void) {
    _printf("  [reconstruct] ");
    RunThreadsOnIndividual(numDrawSurfaces, qtrue, RadiosityBilinearFillThread);
    _printf("done\n");
}

// ---------------------------------------------------------------------------
// Phase 4 — Merge
// ---------------------------------------------------------------------------

static void RadiosityMerge(const float *srcBuffer) {
    int total = numLightBytes / 3;
    for (int i = 0; i < total; i++) {
        if (lightAlphaMask && !lightAlphaMask[i])
            continue;
        lightFloats[i * 3 + 0] += srcBuffer[i * 3 + 0];
        lightFloats[i * 3 + 1] += srcBuffer[i * 3 + 1];
        lightFloats[i * 3 + 2] += srcBuffer[i * 3 + 2];
    }
}

// ---------------------------------------------------------------------------
// LightRadiosity — public entry point
// ---------------------------------------------------------------------------

void LightRadiosity(int radiosityPasses) {
    if (radiosityPasses <= 0)
        return;

    _printf("--- Radiosity ---\n");

    if (!embree) {
        _printf("WARNING: Radiosity is only supported with the Embree backend.\n");
        _printf("Skipping radiosity (direct lighting results kept).\n");
        return;
    }

    _printf("%d radiosity pass%s requested\n",
            radiosityPasses, radiosityPasses > 1 ? "es" : "");

    AllocateRadiosityFloats();
    memset(accumRadiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

    for (int pnum = 1; pnum <= radiosityPasses; pnum++) {
        double passStart = I_FloatTime();
        _printf("Pass %d/%d:\n", pnum, radiosityPasses);

        // Source selection: First pass uses direct light; others use previous bounce
        const float *emitSource = (pnum == 1) ? lightFloats : radiosityFloats;

        _printf("  [emit]       ");
        RadiosityEmit(emitSource);

        // Zero destination buffer for current integration
        memset(radiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

        _printf("  [integrate]  %d emitters generated. Starting integration...\n", g_numEmitters);
        fflush(stdout);
        RunThreadsOnIndividual(numDrawSurfaces, qtrue, RadiosityIntegrateThread);
        _printf("done\n");
        fflush(stdout);

        RadiosityReconstruct();

        // Accumulate current bounce into total GI sum
        for (int i = 0; i < numLightBytes / 3; i++) {
            accumRadiosityFloats[i * 3 + 0] += radiosityFloats[i * 3 + 0];
            accumRadiosityFloats[i * 3 + 1] += radiosityFloats[i * 3 + 1];
            accumRadiosityFloats[i * 3 + 2] += radiosityFloats[i * 3 + 2];
        }

        // Cleanup emitters for this pass
        free(g_emitters);
        g_emitters    = NULL;
        g_numEmitters = 0;

        double passEnd = I_FloatTime();
        _printf("  Pass %d complete (%.0f seconds)\n\n", pnum, passEnd - passStart);
        fflush(stdout);
    }

    // Final Stage: Merge the totally accumulated GI into the lightmap
    _printf("  [merge]      Finalizing cumulative GI merging... ");
    RadiosityMerge(accumRadiosityFloats);
    _printf("done\n");

    FreeRadiosityFloats();
}
