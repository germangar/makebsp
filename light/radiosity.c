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
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tuning parameters (managed via main.c CLI)
// ---------------------------------------------------------------------------

static float rad_intensity;    // Energy per bounce (conserved)
static float rad_color_ratio;     // Greyscale vs colour bleeding
float rad_min_energy    = 1.0f;   // Min brightness for emitters
float rad_voxel_size    = 0.0f;   // Adaptive default: game->defaultSampleSize * game->radiosityInterval
static float active_rad_ao_intensity = 0.0f; // Track pass-specific AO intensity
float rad_angle_match   = 60.0f;  // Angle in degrees (Default: 60)
static float rad_angle_match_cos = 0.5f;

// Amount to nudge the emitter origin off the surface along its normal.
// Prevents the emitter from self-shadowing via Embree.
#define RAD_ORIGIN_NUDGE        0.25f

#define RAD_BORDER_WIDTH 2

// Helper: Is this pixel in the 1:1 evaluated border zone?
static qboolean IsBorderPixel(int lx, int ly, int w, int h) {
    if (g_fast) return qfalse; // Preserve original uniform path when -fast is enabled
    int border = RAD_BORDER_WIDTH;
    if (w <= 2 * border || h <= 2 * border) return qtrue; // Small surface fallback
    return (lx < border || lx >= w - border || 
            ly < border || ly >= h - border);
}

// Helper: Is this pixel an aligned interior sparse sample?
static qboolean IsInteriorSparsePixel(int lx, int ly, int interval) {
    int border = g_fast ? 0 : RAD_BORDER_WIDTH;
    int int_x = lx - border;
    int int_y = ly - border;
    return (int_x % interval == 0 && int_y % interval == 0);
}

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
    vec3_t energySum;
    vec3_t weightedDeluxeSum;
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
                Q_Free(curr);
                curr = next;
            }
        }
        Q_Free(g_radVoxels);
    }

    for (int i = 0; i < 3; i++) {
        g_radVoxelMins[i] = floor(dmodels[0].mins[i] / rad_voxel_size) * rad_voxel_size - rad_voxel_size;
        float maxVal = ceil(dmodels[0].maxs[i] / rad_voxel_size) * rad_voxel_size + rad_voxel_size;
        g_radVoxelDims[i] = (int)((maxVal - g_radVoxelMins[i]) / rad_voxel_size) + 1;
    }

    size_t numHeads = (size_t)g_radVoxelDims[0] * g_radVoxelDims[1] * g_radVoxelDims[2];
    g_radVoxels = calloc(numHeads, sizeof(radVoxel_t));
}

static void RadiosityVoxelAdd(const vec3_t pos, const vec3_t normal, const vec3_t color, const vec3_t dir, const vec3_t energy) {
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
        if (dir) {
            float lum = color[0]*0.299f + color[1]*0.587f + color[2]*0.114f;
            head->weightedDeluxeSum[0] = lum * dir[0];
            head->weightedDeluxeSum[1] = lum * dir[1];
            head->weightedDeluxeSum[2] = lum * dir[2];
        } else {
            VectorClear(head->weightedDeluxeSum);
        }
        if (energy) {
            VectorCopy(energy, head->energySum);
        } else {
            VectorClear(head->energySum);
        }
        head->next = NULL;
        return;
    }

    // 2. Otherwise, find a voxel bucket with a similar normal
    radVoxel_t *curr = head;
    while (curr) {
        if (DotProduct(curr->normal, normal) > rad_angle_match_cos) {
            VectorAdd(curr->color, color, curr->color);
            VectorAdd(curr->normal, normal, curr->normal);
            if (dir) {
                float lum = color[0]*0.299f + color[1]*0.587f + color[2]*0.114f;
                curr->weightedDeluxeSum[0] += lum * dir[0];
                curr->weightedDeluxeSum[1] += lum * dir[1];
                curr->weightedDeluxeSum[2] += lum * dir[2];
            }
            if (energy) {
                VectorAdd(curr->energySum, energy, curr->energySum);
            }
            curr->weight += 1.0f;
            return;
        }
        if (!curr->next) break;
        curr = curr->next;
    }

    // 3. Create new normal-specific bucket if no match found
    radVoxel_t *newV = Q_Alloc(sizeof(radVoxel_t));
    VectorCopy(color, newV->color);
    VectorCopy(normal, newV->normal);
    newV->weight = 1.0f;
    if (dir) {
        float lum = color[0]*0.299f + color[1]*0.587f + color[2]*0.114f;
        newV->weightedDeluxeSum[0] = lum * dir[0];
        newV->weightedDeluxeSum[1] = lum * dir[1];
        newV->weightedDeluxeSum[2] = lum * dir[2];
    } else {
        VectorClear(newV->weightedDeluxeSum);
    }
    if (energy) {
        VectorCopy(energy, newV->energySum);
    } else {
        VectorClear(newV->energySum);
    }
    newV->next = NULL;
    curr->next = newV;
}


// ---------------------------------------------------------------------------
// Phase 5b: RadVisCheck deleted - its clipped-ray logic lives in the ticket
// fields (nearClip/farClip) flushed through TraceLineRanged, with the same
// untinted NULL-tw context (AlphaFilter guards tw == NULL). See plan
// //6 Phase 5, 5.3/5.6.

// ---------------------------------------------------------------------------
// Patch Subdivision Helper
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Phase 1 — Emit
// ---------------------------------------------------------------------------

static emitter_t *g_emitters     = NULL;
static int        g_numEmitters  = 0;



extern float      *accumRadiosityFloats;

static void RadiosityEmit(const float *srcBuffer, qboolean isFirstPass) {
    int             i, k, lx, ly;
    int             capacity = 4096;


    g_numEmitters = 0;
    g_emitters    = Q_Alloc(sizeof(emitter_t) * capacity);

    if (!g_emitters)
        Error("RadiosityEmit: malloc failed");

    for (i = 0; i < numDrawSurfaces; i++) {
        dsurface_t   *ds = &drawSurfaces[i];
        shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

        if (ds->lightmapNum[0] < 0) continue;
        if (ds->lightmapWidth <= 0 || ds->lightmapHeight <= 0) continue;
        if (si->surfaceFlags & SURF_SKY) continue;
        

        localSurfaces[i].emitterStart = g_numEmitters;
        float maxIntensity = 0;
        float totalArea = 0;

        mesh_t *patchMesh = NULL;
        vec3_t surfNormal = {0,0,0};
        vec3_t lightmapOrigin;
        
        VectorCopy(ds->lightmapOrigin, lightmapOrigin);
        if (ds->surfaceType == MST_PATCH) {
            patchMesh = localSurfaces[i].patchMesh;
        } else if (ds->surfaceType == MST_PLANAR) {
            VectorCopy(ds->lightmapVecs[2], surfNormal);
            if (VectorNormalize(surfNormal, surfNormal) < 0.0001f) {
                VectorCopy(drawVerts[ds->firstVert].normal, surfNormal);
            }
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[0], lightmapOrigin);
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[1], lightmapOrigin);
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
        for (k = 0; k < 3; k++) {
            albedo[k] = si->averageColor[k] / 255.0f;
        }

        // handle vertexcolor overrides so they become part of radiosity
        if (localSurfaces[i].overrideVertexColor) {
            // Unconditional 50/50 blend with the base texture albedo
            for (k = 0; k < 3; k++) {
                albedo[k] = albedo[k] * 0.5f + localSurfaces[i].vertexColor[k] * 0.5f;
            }
        }

        int step = (ds->surfaceType == MST_TRIANGLE_SOUP) ? localSurfaces[i].radInterval : 1;

        for (ly = 0; ly < ds->lightmapHeight; ly += step) {
            for (lx = 0; lx < ds->lightmapWidth; lx += step) {
                qboolean is_border = qfalse;
                if (ds->surfaceType != MST_TRIANGLE_SOUP) {
                    is_border = IsBorderPixel(lx, ly, ds->lightmapWidth, ds->lightmapHeight);
                    if (!is_border && !IsInteriorSparsePixel(lx, ly, localSurfaces[i].radInterval)) {
                        continue;
                    }
                }

                int k_lm = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
                float *src = (float *)&srcBuffer[k_lm * 3];

                if (lightAlphaMask && !lightAlphaMask[k_lm]) continue;
                if (src[0] + src[1] + src[2] < rad_min_energy) continue;

                if (g_numEmitters >= capacity) {
                    capacity *= 2;
                    g_emitters = realloc(g_emitters, sizeof(emitter_t) * capacity);
                }

                emitter_t *em = &g_emitters[g_numEmitters++];

                float st_x = (float)lx;
                float st_y = (float)ly;
                if (ds->surfaceType == MST_TRIANGLE_SOUP || !is_border) {
                    st_x += (float)localSurfaces[i].radInterval * 0.5f;
                    st_y += (float)localSurfaces[i].radInterval * 0.5f;
                } else {
                    st_x += 0.5f;
                    st_y += 0.5f;
                }

                if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + st_x;
                    st[1] = (float)ds->lightmapOffset[0][1] + st_y;
                    if (!TriSoupSamplePoint(ds, st, em->center, em->normal, NULL)) {
                        VectorClear(em->center); VectorClear(em->normal);
                    }
                    VectorMA(em->center, RAD_ORIGIN_NUDGE, em->normal, em->center);
                    VectorAdd(em->center, localSurfaces[i].entityOrigin, em->center);
                } else if (ds->surfaceType == MST_PATCH) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + st_x;
                    st[1] = (float)ds->lightmapOffset[0][1] + st_y;
                    if (PatchSamplePoint(patchMesh, st, em->center, em->normal, NULL)) {
                        VectorMA(em->center, RAD_ORIGIN_NUDGE, em->normal, em->center);
                        VectorAdd(em->center, localSurfaces[i].entityOrigin, em->center);
                    } else {
                        VectorClear(em->normal);
                        VectorClear(em->center);
                    }
                } else {
                    VectorMA(lightmapOrigin, st_x, ds->lightmapVecs[0], em->center);
                    VectorMA(em->center, st_y, ds->lightmapVecs[1], em->center);
                    VectorMA(em->center, RAD_ORIGIN_NUDGE, surfNormal, em->center);
                    VectorAdd(em->center, localSurfaces[i].entityOrigin, em->center);
                    VectorCopy(surfNormal, em->normal);
                }
                
                if (ds->surfaceType == MST_TRIANGLE_SOUP || !is_border) {
                    em->area = luxelArea * (float)(localSurfaces[i].radInterval * localSurfaces[i].radInterval);
                } else {
                    em->area = luxelArea * 1.0f;
                }
                VectorScale(src, rad_intensity, em->color);
                
                // If deluxe mapping is active, apply the receiver-side falloff correction.
                // When deluxe mapping is off, the srcBuffer already contains the fully attenuated color, so we skip it.
                if (game->deluxeMap) {
                    float *prevDir = isFirstPass ? &deluxeFloats[k_lm * 3] : &radiosityDeluxeFloats[k_lm * 3];
                    float prevCos = DotProduct(em->normal, prevDir);
                    if (prevCos < 0.0f) prevCos = 0.0f;
                    else if (prevCos > 1.0f) prevCos = 1.0f;

                    // Attenuate the physical emitter color
                    VectorScale(em->color, prevCos, em->color);
                }

                // Apply sky tint to the incident light before it hits the wall
                if (ambient_enabled && rad_color_ratio < 1.0f) {
                    float srcLum = em->color[0] * 0.299f + em->color[1] * 0.587f + em->color[2] * 0.114f;
                    vec3_t tintedLight;
                    float skyLum = skyColor[0] * 0.299f + skyColor[1] * 0.587f + skyColor[2] * 0.114f;
                    
                    if (skyLum > 0.0001f) {
                        for (k = 0; k < 3; k++) {
                            tintedLight[k] = (skyColor[k] / skyLum) * srcLum;
                        }
                    } else {
                        VectorCopy(em->color, tintedLight);
                    }
                    
                    for (k = 0; k < 3; k++) {
                        em->color[k] = em->color[k] * rad_color_ratio + tintedLight[k] * (1.0f - rad_color_ratio);
                    }
                }

                for (k = 0; k < 3; k++) em->color[k] *= albedo[k];

                totalArea += em->area;
                float lum = em->color[0] > em->color[1] ? (em->color[0] > em->color[2] ? em->color[0] : em->color[2]) : (em->color[1] > em->color[2] ? em->color[1] : em->color[2]);
                if (lum > maxIntensity) maxIntensity = lum;
            }
        }
        

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

// Phase 5b ticket batching for the integrate family.
#define RADIOSITY_TICKET_BATCH 128

/*
================
TicketConsumeRadiosityEmitterTickets

Ticket wavefront (Phase 5b, Stage 3): consume radiosity emitter tickets in
generation order — a STRAIGHT fold with no group logic (the caller owns the
per-dst accumulators and the ThreadLock'd write blocks, which stay exactly
where they were). The visibility outcome (the old RadVisCheck, now the
flush's clipped ray) gates the accumulation: clear → AccumulateContribution
with the contribution prepared at generation; blocked → nothing.

`rejected` is never produced here: after a clear visibility check the old
code always accumulated. `deluxeMinAngle` = the dst surface's shader value
(the vertex path passes 0.0f, as the old code did).
================
*/
static void TicketConsumeRadiosityEmitterTickets(lightTicket_t *tickets, int n,
                                                 vec3_t accum, vec3_t accumDeluxe, vec3_t accumEnergy,
                                                 qboolean deluxe, float deluxeMinAngle)
{
    int i;

    for (i = 0; i < n; i++)
    {
        lightTicket_t *t = &tickets[i];

        if (!t->trace.passSolid)
        {
            // Phase 5b-revised: apply the averaged-alpha / lightfilter tint
            // accumulated by the geometry filter (lightgrid-style) to the
            // prepared contribution before accumulating.
            for (int c = 0; c < 3; c++)
                t->cont.irradiance[c] *= t->trace.filter[c];

            AccumulateContribution(accum, deluxe ? accumDeluxe : NULL, deluxe ? accumEnergy : NULL,
                                   &t->cont, t->normal, deluxeMinAngle);

            #pragma omp atomic
            c_ticketsContributed++;
        }
        else
        {
            #pragma omp atomic
            c_ticketsBlocked++;
        }
    }
}

static void RadiosityIntegrateOneSurface(int surfIdx) {
    dsurface_t   *ds = &drawSurfaces[surfIdx];
    shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

    if (ds->lightmapNum[0] < 0 || ds->lightmapWidth <= 0 || ds->lightmapHeight <= 0 || g_numEmitters <= 0) return;
    if (si->surfaceFlags & SURF_SKY) return;

    int surf_rad_interval = localSurfaces[surfIdx].radInterval;

    if (ds->surfaceType != MST_PATCH && ds->surfaceType != MST_PLANAR && ds->numVerts < 3) return;

    voxelPoint_t *points = NULL;
    int numPoints = 0;
    if (g_fast && ds->surfaceType == MST_TRIANGLE_SOUP) {
        points = VoxelCache_Load(surfIdx, &numPoints);
    }

    // Ticket wavefront (Phase 5b): ONE batch per call (hoisted — the dst
    // loops can be millions of texels times radiosityPasses); each dst texel
    // flushes its own group before the ThreadLock'd write.
    lightTicketBatch_t batch;
    TicketBatch_Init(&batch, RADIOSITY_TICKET_BATCH);

    // Phase 5b-revised (jal approved): lightgrid-style averaged-alpha
    // filtering for the visibility rays. The old RadVisCheck used an untinted
    // NULL-tw context whose geometry-filter reads hit adjacent stack garbage
    // (UB). With isLightgrid the filter accumulates averaged alpha/
    // lightfilter tints into each ticket's trace.filter, which the consume
    // applies to the contribution (TicketConsumeRadiosityEmitterTickets).
    traceWork_t tw;
    memset(&tw, 0, sizeof(tw));
    tw.ignoreSurface = -1;
    tw.isLightgrid = qtrue;
    tw.patchshadows = patchshadows;
    int scale = upscale ? 2 : 1;
    int step = (ds->surfaceType == MST_TRIANGLE_SOUP) ? surf_rad_interval : 1;

    for (int ly = 0; ly < ds->lightmapHeight; ly += step) {
        for (int lx = 0; lx < ds->lightmapWidth; lx += step) {
            if (ds->surfaceType != MST_TRIANGLE_SOUP) {
                // Determine native lx/ly for IsBorderPixel checks
                if (!IsBorderPixel(lx, ly, ds->lightmapWidth, ds->lightmapHeight) && 
                    !IsInteriorSparsePixel(lx, ly, surf_rad_interval)) {
                    continue;
                }
            }

            int py = ds->lightmapOffset[0][1] + ly;
            int px = ds->lightmapOffset[0][0] + lx;
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + py) * LIGHTMAP_WIDTH + px;
            
            if (unreachableMask && BITMAP_TEST(unreachableMask, k_dst)) {
                continue;
            }

            int k_upscale = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT * scale + py * scale) * LIGHTMAP_WIDTH * scale + px * scale;
            if (texelNormals[k_upscale][0] == 0.0f && texelNormals[k_upscale][1] == 0.0f && texelNormals[k_upscale][2] == 0.0f)
                continue;

            vec3_t dst, dstNormal;
            for (int c = 0; c < 3; c++) {
                dst[c] = texelOrigins[k_upscale][c];
                dstNormal[c] = texelNormals[k_upscale][c];
            }

            // Note: We no longer modify lightAlphaMask here, as PrecacheTexelGeometry handles all geometry evaluation.

            // ---------------------------------------------------------------
            // Trisoup: original path — bake cosDst into scalar, write radiosityFloats.
            // ---------------------------------------------------------------
            if (ds->surfaceType == MST_TRIANGLE_SOUP) {
                float accum[3] = {0, 0, 0};
                float accumDeluxe[3], accumEnergy[3];
                VectorCopy(dstNormal, accumDeluxe);
                VectorClear(accumEnergy);
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
                        vec3_t v_to_dst; VectorSubtract(dst, localSurfaces[s].origin, v_to_dst);
                        if (DotProduct(v_to_dst, g_emitters[localSurfaces[s].emitterStart].normal) < -localSurfaces[s].radius) continue;
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

                        float distClamped = dist < game->rad_ao_min ? game->rad_ao_min : dist;
                        float formFactorBase = (em->area * cosEmit) / (M_PI * distClamped * distClamped);
                        
                        float factor = 1.0f - active_rad_ao_intensity;
                        if (dist <= game->rad_ao_min) {
                            factor = 1.0f - active_rad_ao_intensity;
                        } else if (dist < game->rad_ao_min + game->rad_ao_max) {
                            float lerp = game->rad_ao_max > 0.0f ? (dist - game->rad_ao_min) / game->rad_ao_max : 1.0f;
                            formFactorBase *= factor + (1.0f - factor) * lerp;
                        }
                        if (formFactorBase * cosDst > 1.0f) formFactorBase = 1.0f / cosDst;
                        
                        // Precise intensity cull: check if brightest color component * formFactor < threshold
                        float maxColor = em->color[0] > em->color[1] ? (em->color[0] > em->color[2] ? em->color[0] : em->color[2]) : (em->color[1] > em->color[2] ? em->color[1] : em->color[2]);
                        if (formFactorBase * cosDst * maxColor <= MIN_RADIOSITY_EMITTER_ADD) continue;

                        // Ticket wavefront (Phase 5b): everything except the
                        // visibility check is pure and moves to generation —
                        // the full contribution is prepared here (the old
                        // code built it after RadVisCheck; same expressions,
                        // same order), and the consume applies the clipped
                        // ray's outcome.
                        {
                            lightTicket_t t;
                            lightTicket_t *slot;
                            memset(&t, 0, sizeof(t));
                            t.light = NULL;
                            VectorCopy(dst, t.origin);
                            VectorCopy(em->center, t.end);
                            VectorCopy(dstNormal, t.normal);
                            t.nearClip = RAD_ORIGIN_NUDGE * 0.5f;              // old RadVisCheck tnear
                            t.farClip = dist - RAD_ORIGIN_NUDGE * 0.5f;        // old tfar (dist >= 0.001 culled above)
                            t.testAll = qfalse;
                            t.applyColorFilter = qfalse;
                            t.normalValid = qtrue;
                            t.texelIdx = k_dst;
                            t.ssIdx = 0;
                            t.cont.light = NULL;
                            VectorCopy(rayDir, t.cont.dir);
                            if (game->deluxeMap) {
                                float clampCos = cosDst < 0.01f ? 0.01f : cosDst;
                                VectorScale(em->color, formFactorBase / clampCos, t.cont.irradiance);
                            } else {
                                VectorScale(em->color, formFactorBase, t.cont.irradiance);
                            }
                            t.cont.angle = cosDst;
                            t.cont.isGlow = qfalse;
                            t.cont.familyCount = 1;
                            if (batch.count >= batch.capacity)
                            {
                                int fn = TicketBatch_FlushTraces(&batch, &tw);
                                TicketConsumeRadiosityEmitterTickets(batch.tickets, fn,
                                                                     accum, accumDeluxe, accumEnergy,
                                                                     game->deluxeMap ? qtrue : qfalse, si->deluxeMinAngle);
                            }
                            slot = TicketBatch_Append(&batch);
                            if (slot)
                                *slot = t;
                            else
                            {
                                #pragma omp atomic
                                c_ticketAppendWarnings++;
                            }
                        }
                    }
                }
                {
                    int n = TicketBatch_FlushTraces(&batch, &tw);
                    TicketConsumeRadiosityEmitterTickets(batch.tickets, n,
                                                         accum, accumDeluxe, accumEnergy,
                                                         game->deluxeMap ? qtrue : qfalse, si->deluxeMinAngle);
                }
                if (accum[0] > 0 || accum[1] > 0 || accum[2] > 0) {
                    ThreadLock();
                    VectorAdd(&radiosityFloats[k_dst * 3], accum, &radiosityFloats[k_dst * 3]);
                    if (game->deluxeMap) {
                        VectorCopy(accumDeluxe, &radiosityDeluxeFloats[k_dst * 3]);
                        VectorCopy(accumEnergy, &radiosityEnergyFloats[k_dst * 3]);
                    }
                    ThreadUnlock();
                }
            } else {
                // ---------------------------------------------------------------
                // Planar / Patch: irradiance vector path.
                // ---------------------------------------------------------------
                float accum[3] = {0,0,0};
                float accumDeluxe[3], accumEnergy[3];
                VectorCopy(dstNormal, accumDeluxe);
                VectorClear(accumEnergy);

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
                        vec3_t v_to_dst; VectorSubtract(dst, localSurfaces[s].origin, v_to_dst);
                        if (DotProduct(v_to_dst, g_emitters[localSurfaces[s].emitterStart].normal) < -localSurfaces[s].radius) continue;
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

                        float distClamped = dist < game->rad_ao_min ? game->rad_ao_min : dist;
                        float formFactorBase = (em->area * cosEmit) / (M_PI * distClamped * distClamped);
                        float factor = 1.0f - active_rad_ao_intensity;
                        if (dist <= game->rad_ao_min) {
                            formFactorBase *= factor;
                        } else if (dist < game->rad_ao_min + game->rad_ao_max) {
                            float lerp = game->rad_ao_max > 0.0f ? (dist - game->rad_ao_min) / game->rad_ao_max : 1.0f;
                            formFactorBase *= factor + (1.0f - factor) * lerp;
                        }
                        if (formFactorBase * cosDst > 1.0f) formFactorBase = 1.0f / cosDst;
                        
                        // Precise intensity cull: use specialized radiosity threshold
                        float maxColor = em->color[0] > em->color[1] ? (em->color[0] > em->color[2] ? em->color[0] : em->color[2]) : (em->color[1] > em->color[2] ? em->color[1] : em->color[2]);
                        if (formFactorBase * cosDst * maxColor <= MIN_RADIOSITY_EMITTER_ADD) continue;

                        // Ticket wavefront (Phase 5b): same conversion as the
                        // trisoup branch — full contribution prepared at
                        // generation, clipped-ray visibility at consume.
                        {
                            lightTicket_t t;
                            lightTicket_t *slot;
                            memset(&t, 0, sizeof(t));
                            t.light = NULL;
                            VectorCopy(dst, t.origin);
                            VectorCopy(em->center, t.end);
                            VectorCopy(dstNormal, t.normal);
                            t.nearClip = RAD_ORIGIN_NUDGE * 0.5f;              // old RadVisCheck tnear
                            t.farClip = dist - RAD_ORIGIN_NUDGE * 0.5f;        // old tfar (dist >= 0.001 culled above)
                            t.testAll = qfalse;
                            t.applyColorFilter = qfalse;
                            t.normalValid = qtrue;
                            t.texelIdx = k_dst;
                            t.ssIdx = 0;
                            t.cont.light = NULL;
                            VectorCopy(rayDir, t.cont.dir);
                            if (game->deluxeMap) {
                                float clampCos = cosDst < 0.01f ? 0.01f : cosDst;
                                VectorScale(em->color, formFactorBase / clampCos, t.cont.irradiance);
                            } else {
                                VectorScale(em->color, formFactorBase, t.cont.irradiance);
                            }
                            t.cont.angle = cosDst;
                            t.cont.isGlow = qfalse;
                            t.cont.familyCount = 1;
                            if (batch.count >= batch.capacity)
                            {
                                int fn = TicketBatch_FlushTraces(&batch, &tw);
                                TicketConsumeRadiosityEmitterTickets(batch.tickets, fn,
                                                                     accum, accumDeluxe, accumEnergy,
                                                                     game->deluxeMap ? qtrue : qfalse, si->deluxeMinAngle);
                            }
                            slot = TicketBatch_Append(&batch);
                            if (slot)
                                *slot = t;
                            else
                            {
                                #pragma omp atomic
                                c_ticketAppendWarnings++;
                            }
                        }
                    }
                }

                {
                    int n = TicketBatch_FlushTraces(&batch, &tw);
                    TicketConsumeRadiosityEmitterTickets(batch.tickets, n,
                                                         accum, accumDeluxe, accumEnergy,
                                                         game->deluxeMap ? qtrue : qfalse, si->deluxeMinAngle);
                }
                if (accum[0] > 0 || accum[1] > 0 || accum[2] > 0) {
                    ThreadLock();
                    VectorAdd(&radiosityFloats[k_dst * 3], accum, &radiosityFloats[k_dst * 3]);
                    if (game->deluxeMap) {
                        VectorCopy(accumDeluxe, &radiosityDeluxeFloats[k_dst * 3]);
                        VectorCopy(accumEnergy, &radiosityEnergyFloats[k_dst * 3]);
                    }
                    ThreadUnlock();
                }
            }
        }
    }
    
    TicketBatch_Free(&batch);

    if (points) Q_Free(points);
}


static void RadiosityIntegrateVertexSurface(int surfIdx) {
    dsurface_t *ds = &drawSurfaces[surfIdx];
    if (ds->numVerts == 0) return;

    // Ticket wavefront (Phase 5b): one batch per call. Phase 5b-revised:
    // lightgrid-style averaged-alpha filtering (same tw as the texel path —
    // jal approved applying it to the vertex path too).
    lightTicketBatch_t batch;
    TicketBatch_Init(&batch, RADIOSITY_TICKET_BATCH);

    traceWork_t tw;
    memset(&tw, 0, sizeof(tw));
    tw.ignoreSurface = -1;
    tw.isLightgrid = qtrue;
    tw.patchshadows = patchshadows;

    for (int i = 0; i < ds->numVerts; i++) {
        int vIdx = ds->firstVert + i;
        vec3_t dst, dstNormal;
        
        VectorCopy(drawVerts[vIdx].xyz, dst);
        VectorCopy(drawVerts[vIdx].normal, dstNormal);
        VectorAdd(dst, localSurfaces[surfIdx].entityOrigin, dst);
        VectorMA(dst, RAD_ORIGIN_NUDGE, dstNormal, dst);

        float accum[3] = {0, 0, 0};

        for (int s = 0; s < numDrawSurfaces; s++) {
            if (localSurfaces[s].emitterCount == 0) continue;

            // Cull A: Distance
            vec3_t v_to_surf; VectorSubtract(localSurfaces[s].origin, dst, v_to_surf);
            float dist_to_surf = VectorLength(v_to_surf);
            if (dist_to_surf > localSurfaces[s].maxReach + localSurfaces[s].radius) continue;

            // Cull B: Receiver Plane
            if (DotProduct(v_to_surf, dstNormal) < -localSurfaces[s].radius) continue;

            // Cull C: Emitter Plane
            if (drawSurfaces[s].surfaceType == MST_PLANAR) {
                vec3_t v_to_dst; VectorSubtract(dst, localSurfaces[s].origin, v_to_dst);
                if (DotProduct(v_to_dst, g_emitters[localSurfaces[s].emitterStart].normal) < -localSurfaces[s].radius) continue;
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

                float distClamped = dist < game->rad_ao_min ? game->rad_ao_min : dist;
                float formFactorBase = (em->area * cosEmit) / (M_PI * distClamped * distClamped);
                
                float factor = 1.0f - active_rad_ao_intensity;
                if (dist <= game->rad_ao_min) {
                    factor = 1.0f - active_rad_ao_intensity;
                } else if (dist < game->rad_ao_min + game->rad_ao_max) {
                    float lerp = game->rad_ao_max > 0.0f ? (dist - game->rad_ao_min) / game->rad_ao_max : 1.0f;
                    formFactorBase *= factor + (1.0f - factor) * lerp;
                }
                if (formFactorBase * cosDst > 1.0f) formFactorBase = 1.0f / cosDst;
                
                float maxColor = em->color[0] > em->color[1] ? (em->color[0] > em->color[2] ? em->color[0] : em->color[2]) : (em->color[1] > em->color[2] ? em->color[1] : em->color[2]);
                if (formFactorBase * cosDst * maxColor <= MIN_RADIOSITY_EMITTER_ADD) continue;

                // Ticket wavefront (Phase 5b): same conversion; the vertex
                // path has no deluxe accumulators (the old accumulate passed
                // NULL/NULL and 0.0f).
                {
                    lightTicket_t t;
                    lightTicket_t *slot;
                    memset(&t, 0, sizeof(t));
                    t.light = NULL;
                    VectorCopy(dst, t.origin);
                    VectorCopy(em->center, t.end);
                    VectorCopy(dstNormal, t.normal);
                    t.nearClip = RAD_ORIGIN_NUDGE * 0.5f;              // old RadVisCheck tnear
                    t.farClip = dist - RAD_ORIGIN_NUDGE * 0.5f;        // old tfar (dist >= 0.001 culled above)
                    t.testAll = qfalse;
                    t.applyColorFilter = qfalse;
                    t.normalValid = qtrue;
                    t.texelIdx = vIdx;
                    t.ssIdx = 0;
                    t.cont.light = NULL;
                    VectorCopy(rayDir, t.cont.dir);
                    VectorScale(em->color, formFactorBase, t.cont.irradiance);
                    t.cont.angle = cosDst;
                    t.cont.isGlow = qfalse;
                    t.cont.familyCount = 1;
                    if (batch.count >= batch.capacity)
                    {
                        int fn = TicketBatch_FlushTraces(&batch, &tw);
                        TicketConsumeRadiosityEmitterTickets(batch.tickets, fn,
                                                             accum, accum, accum,
                                                             qfalse, 0.0f);
                    }
                    slot = TicketBatch_Append(&batch);
                    if (slot)
                        *slot = t;
                    else
                    {
                        #pragma omp atomic
                        c_ticketAppendWarnings++;
                    }
                }
            }
        }

        {
            int n = TicketBatch_FlushTraces(&batch, &tw);
            TicketConsumeRadiosityEmitterTickets(batch.tickets, n,
                                                 accum, accum, accum,
                                                 qfalse, 0.0f);
        }
        if (accum[0] > 0 || accum[1] > 0 || accum[2] > 0) {
            ThreadLock();
            VectorAdd(&radiosityVertexFloats[vIdx * 3], accum, &radiosityVertexFloats[vIdx * 3]);
            ThreadUnlock();
        }
    }

    TicketBatch_Free(&batch);
}

static void RadiosityIntegrateThread(int surfIdx) {
    int realSurfIndex = surfaceWorkOrder[surfIdx];
    dsurface_t *ds = &drawSurfaces[realSurfIndex];
    
    int surfWeight = ds->numVerts;
    if (ds->lightmapNum[0] >= 0) {
        surfWeight += ds->lightmapWidth * ds->lightmapHeight;
    }
    
    if (ds->lightmapNum[0] >= 0) {
        RadiosityIntegrateOneSurface(realSurfIndex);
    }
    
    RadiosityIntegrateVertexSurface(realSurfIndex);
    
    ThreadCompletedWeighted(surfWeight);
}

// ---------------------------------------------------------------------------
static void RadiosityVoxelize(const float *srcFloats, const float *srcDeluxe, const float *srcEnergy) {
    _printf("  [voxelize]   Unified world-space splatting... ");
    RadiosityVoxelReset();

    for (int s = 0; s < numDrawSurfaces; s++) {
        dsurface_t *ds = &drawSurfaces[s];
        shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

        if (ds->lightmapNum[0] < 0) continue;
        if (si->surfaceFlags & SURF_SKY) continue;
        
        int surf_rad_interval = localSurfaces[s].radInterval;

        mesh_t *patchMesh = NULL;
        vec3_t surfNormal = {0,0,0};
        vec3_t lightmapOrigin;
        
        VectorCopy(ds->lightmapOrigin, lightmapOrigin);
        if (ds->surfaceType == MST_PATCH) {
            patchMesh = localSurfaces[s].patchMesh;
        } else if (ds->surfaceType == MST_PLANAR) {
            VectorCopy(ds->lightmapVecs[2], surfNormal);
            if (VectorNormalize(surfNormal, surfNormal) < 0.0001f) {
                VectorCopy(drawVerts[ds->firstVert].normal, surfNormal);
            }
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[0], lightmapOrigin);
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[1], lightmapOrigin);
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

                    if (lx % surf_rad_interval == 0 && ly % surf_rad_interval == 0) {
                        if (lightAlphaMask && !lightAlphaMask[pIdx]) continue;
                        if (srcFloats[pIdx * 3] == 0 && srcFloats[pIdx * 3 + 1] == 0 && srcFloats[pIdx * 3 + 2] == 0) continue;
                        
                        vec3_t pos, normal;
                        VectorCopy(points[i].pos, pos);
                        VectorCopy(points[i].normal, normal);
                        VectorAdd(pos, localSurfaces[s].entityOrigin, pos);
                        RadiosityVoxelAdd(pos, normal, &srcFloats[pIdx * 3], game->deluxeMap ? &srcDeluxe[pIdx * 3] : NULL, game->deluxeMap ? &srcEnergy[pIdx * 3] : NULL);
                    }
                }
                Q_Free(points);
                
                continue;
            }
        }

        int step = (ds->surfaceType == MST_TRIANGLE_SOUP) ? surf_rad_interval : 1;

        for (int ly = 0; ly < ds->lightmapHeight; ly += step) {
            for (int lx = 0; lx < ds->lightmapWidth; lx += step) {
                if (ds->surfaceType != MST_TRIANGLE_SOUP) {
                    if (!IsBorderPixel(lx, ly, ds->lightmapWidth, ds->lightmapHeight) && 
                        !IsInteriorSparsePixel(lx, ly, surf_rad_interval)) {
                        continue;
                    }
                }

                int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ly) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + lx;
                if (lightAlphaMask && !lightAlphaMask[k_dst]) continue;

                if (srcFloats[k_dst * 3] == 0 && srcFloats[k_dst * 3 + 1] == 0 && srcFloats[k_dst * 3 + 2] == 0) continue;

                vec3_t pos, normal;
                if (ds->surfaceType == MST_PATCH) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)lx + 0.5f;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)ly + 0.5f;
                    if (PatchSamplePoint(patchMesh, st, pos, normal, NULL)) {
                        VectorAdd(pos, localSurfaces[s].entityOrigin, pos);
                    } else {
                        continue;
                    }
                } else {
                    VectorMA(ds->lightmapOrigin, (float)lx, ds->lightmapVecs[0], pos);
                    VectorMA(pos, (float)ly, ds->lightmapVecs[1], pos);
                    VectorAdd(pos, localSurfaces[s].entityOrigin, pos);
                    VectorCopy(surfNormal, normal);
                }
                RadiosityVoxelAdd(pos, normal, &srcFloats[k_dst * 3], game->deluxeMap ? &srcDeluxe[k_dst * 3] : NULL, game->deluxeMap ? &srcEnergy[k_dst * 3] : NULL);
            }
        }
        
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

// Helper: Bilinearly interpolate from the sparse grid
static void RadiosityBilinearSample(dsurface_t *ds, int lx, int ly, int surf_rad_interval, const vec3_t normal, vec3_t outColor, vec3_t outDir, vec3_t outEnergy) {
    int border = g_fast ? 0 : RAD_BORDER_WIDTH;
    int w = ds->lightmapWidth;
    int h = ds->lightmapHeight;

    int x0 = border + ((lx - border) / surf_rad_interval) * surf_rad_interval;
    int x1 = x0 + surf_rad_interval;
    if (x1 >= w) x1 = w - 1;
    if (x0 < 0) x0 = 0;

    int y0 = border + ((ly - border) / surf_rad_interval) * surf_rad_interval;
    int y1 = y0 + surf_rad_interval;
    if (y1 >= h) y1 = h - 1;
    if (y0 < 0) y0 = 0;

    float fx = (x1 == x0) ? 0.0f : (float)(lx - x0) / (float)(x1 - x0);
    float fy = (y1 == y0) ? 0.0f : (float)(ly - y0) / (float)(y1 - y0);

    int k00 = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0);
    int k10 = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1);
    int k01 = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0);
    int k11 = ((ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1);

    int idx00 = k00 * 3;
    int idx10 = k10 * 3;
    int idx01 = k01 * 3;
    int idx11 = k11 * 3;
    for (int c = 0; c < 3; c++) {
        float row0 = radiosityFloats[idx00 + c] * (1.0f - fx) + radiosityFloats[idx10 + c] * fx;
        float row1 = radiosityFloats[idx01 + c] * (1.0f - fx) + radiosityFloats[idx11 + c] * fx;
        outColor[c] = row0 * (1.0f - fy) + row1 * fy;
    }

    if (outDir) {
        float lum00 = radiosityFloats[idx00]*0.299f + radiosityFloats[idx00+1]*0.587f + radiosityFloats[idx00+2]*0.114f;
        float lum10 = radiosityFloats[idx10]*0.299f + radiosityFloats[idx10+1]*0.587f + radiosityFloats[idx10+2]*0.114f;
        float lum01 = radiosityFloats[idx01]*0.299f + radiosityFloats[idx01+1]*0.587f + radiosityFloats[idx01+2]*0.114f;
        float lum11 = radiosityFloats[idx11]*0.299f + radiosityFloats[idx11+1]*0.587f + radiosityFloats[idx11+2]*0.114f;
        vec3_t tempDir;
        for (int c = 0; c < 3; c++) {
            float row0 = (radiosityDeluxeFloats[idx00 + c] * lum00) * (1.0f - fx) + (radiosityDeluxeFloats[idx10 + c] * lum10) * fx;
            float row1 = (radiosityDeluxeFloats[idx01 + c] * lum01) * (1.0f - fx) + (radiosityDeluxeFloats[idx11 + c] * lum11) * fx;
            tempDir[c] = row0 * (1.0f - fy) + row1 * fy;
        }
        if (VectorNormalize(tempDir, outDir) < 0.0001f) {
            VectorCopy(normal, outDir);
        }
    }

    if (outEnergy) {
        for (int c = 0; c < 3; c++) {
            float row0 = radiosityEnergyFloats[idx00 + c] * (1.0f - fx) + radiosityEnergyFloats[idx10 + c] * fx;
            float row1 = radiosityEnergyFloats[idx01 + c] * (1.0f - fx) + radiosityEnergyFloats[idx11 + c] * fx;
            outEnergy[c] = row0 * (1.0f - fy) + row1 * fy;
        }
    }
}


static qboolean RadiosityVoxelSample(const vec3_t pos, const vec3_t normal, vec3_t outColor, vec3_t outDir, vec3_t outEnergy) {
    int v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = (int)((pos[i] - g_radVoxelMins[i]) / rad_voxel_size);
        if (v[i] < 0 || v[i] >= g_radVoxelDims[i]) return qfalse;
    }

    vec3_t totalColor = {0,0,0};
    vec3_t totalDeluxe = {0,0,0};
    vec3_t totalEnergy = {0,0,0};
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
                        if (outDir) VectorMA(totalDeluxe, w, curr->weightedDeluxeSum, totalDeluxe);
                        if (outEnergy) VectorMA(totalEnergy, w, curr->energySum, totalEnergy);
                        totalWeight += w;
                    }
                    curr = curr->next;
                }
            }
        }
    }


    if (totalWeight < 0.0001f) return qfalse;

    VectorScale(totalColor, 1.0f / totalWeight, outColor);
    if (outDir) {
        if (VectorNormalize(totalDeluxe, outDir) < 0.0001f) {
            VectorCopy(normal, outDir);
        }
    }
    if (outEnergy) {
        // energySum was already a total across the voxel bucket. We use a weighted average 
        // to approximate the spatial distribution similar to color.
        VectorScale(totalEnergy, 1.0f / totalWeight, outEnergy);
    }
    return qtrue;
}

// Ticket wavefront (Phase 5a, Stage 3): consume lightgrid bounce probes in
// generation order. The old inline body, verbatim, with t->trace and t->dir
// standing in for the local trace/dir:
//   hit + hitFraction < 1 → invDir = -dir → RadiosityVoxelSample(trace.hit,
//   invDir, ...) → accumulate + hits++.
// Counter mapping (invariant-safe): contributed = accumulated; blocked =
// hit geometry but no voxel data at that point; rejected = missed (void).
static void TicketConsumeRadiosityGridProbes(lightTicket_t *tickets, int n,
                                             vec3_t accumColor, int *hits)
{
    int i;

    for (i = 0; i < n; i++)
    {
        lightTicket_t *t = &tickets[i];

        if (t->trace.passSolid && t->trace.hitFraction < 1.0f)
        {
            vec3_t invDir;
            VectorScale(t->dir, -1.0f, invDir);

            vec3_t hitColor = {0, 0, 0};
            if (RadiosityVoxelSample(t->trace.hit, invDir, hitColor, NULL, NULL))
            {
                VectorAdd(accumColor, hitColor, accumColor);
                (*hits)++;

                #pragma omp atomic
                c_ticketsContributed++;
            }
            else
            {
                #pragma omp atomic
                c_ticketsBlocked++;
            }
        }
        else
        {
            #pragma omp atomic
            c_ticketsRejected++;
        }
    }
}

static void RadiosityIntegrateGridThread(int num) {
    if (!gridData32) return;

    vec3_t origin;
    int mod = num;
    int z = mod / (gridBounds[0] * gridBounds[1]);
    mod -= z * (gridBounds[0] * gridBounds[1]);
    int y = mod / gridBounds[0];
    mod -= y * gridBounds[0];
    int x = mod;

    origin[0] = gridMins[0] + x * gridSize[0];
    origin[1] = gridMins[1] + y * gridSize[1];
    origin[2] = gridMins[2] + z * gridSize[2];

    if (gridInvalidMask[num] & GRID_FLAG_INVALID) {
        return;
    }

    if (gridInvalidMask[num] & GRID_FLAG_SOLID) {
        // Lattice point sits inside solid geometry: gather the bounce at the
        // open point resolved by the grid classification pass (the same
        // origin TraceGrid and the MAO pass use), so solid-but-reachable
        // cells receive radiosity too.
        VectorCopy(&gridResolvedOrigins[num * 3], origin);
    }

    int numSamples = 64; 
    float phi = 3.14159265359f * (3.0f - sqrtf(5.0f));
    
    vec3_t accumColor = {0, 0, 0};
    int hits = 0;

    traceWork_t tw;
    memset(&tw, 0, sizeof(tw));
    tw.ignoreSurface = -1;
    tw.isLightgrid = qtrue;
    tw.patchshadows = patchshadows;

    // Ticket wavefront (Phase 5a): one group per grid point — all 64 bounce
    // probes generated, flushed in order, then consumed. Standard ray params
    // (testAll = qtrue, default near/far) so the flush is the exact old call.
    lightTicketBatch_t batch;
    #pragma omp atomic
    c_ticketGroups++;
    TicketBatch_Init(&batch, numSamples);

    for (int i = 0; i < numSamples; i++) {
        float sampleY = 1.0f - (i / (float)(numSamples - 1)) * 2.0f;
        float radius = sqrtf(1.0f - sampleY * sampleY);
        float theta = phi * i;
        
        vec3_t dir;
        dir[0] = cosf(theta) * radius;
        dir[1] = sinf(theta) * radius;
        dir[2] = sampleY;

        vec3_t end;
        VectorMA(origin, 65536.0f, dir, end); 

        // Ticket wavefront: generate the probe; the consume applies the hit
        // outcome (voxel sample at the hit point). t->dir carries the probe
        // direction (the consume derives invDir from it).
        {
            lightTicket_t t;
            lightTicket_t *slot;
            memset(&t, 0, sizeof(t));
            t.light = NULL;
            VectorCopy(origin, t.origin);
            VectorCopy(end, t.end);
            VectorCopy(dir, t.dir);
            t.testAll = qtrue;               // closest-hit: needs trace.hit
            t.applyColorFilter = qfalse;
            t.normalValid = qtrue;
            t.texelIdx = num;
            t.ssIdx = i;
            slot = TicketBatch_Append(&batch);
            if (slot)
                *slot = t;
            else
            {
                #pragma omp atomic
                c_ticketAppendWarnings++;
            }
        }
    }

    // Stage 2+3: flush all probes (tw = &tw, preserving isLightgrid for the
    // average-alpha filter path), then consume.
    {
        int n = TicketBatch_FlushTraces(&batch, &tw);
        TicketConsumeRadiosityGridProbes(batch.tickets, n, accumColor, &hits);
    }
    TicketBatch_Free(&batch);

    if (hits > 0) {
        float factor = 1.0f / numSamples;
        radiosityGridColors[num * 3 + 0] = accumColor[0] * factor;
        radiosityGridColors[num * 3 + 1] = accumColor[1] * factor;
        radiosityGridColors[num * 3 + 2] = accumColor[2] * factor;
    }
}

static void RadiosityReconstructOneSurface(int surfIdx) {
    dsurface_t *ds = &drawSurfaces[surfIdx];
    if (ds->lightmapNum[0] < 0) return;

    int surf_rad_interval = localSurfaces[surfIdx].radInterval;

    int scale = upscale ? 2 : 1;
    int numPixels = ds->lightmapWidth * ds->lightmapHeight;
    if (numPixels <= 0) {
        return;
    }

    vec3_t *tempColor = Q_Alloc(numPixels * sizeof(vec3_t));
    vec3_t *tempDeluxe = NULL;
    vec3_t *tempEnergy = NULL;
    if (game->deluxeMap) {
        tempDeluxe = Q_Alloc(numPixels * sizeof(vec3_t));
        tempEnergy = Q_Alloc(numPixels * sizeof(vec3_t));
    }
    
    if (!tempColor || (game->deluxeMap && (!tempDeluxe || !tempEnergy))) {
        _printf("WARNING: RadiosityReconstruct: failed to allocate memory for surface %d (%dx%d, %d bytes). Surface texels will be blank.\n",
                surfIdx, ds->lightmapWidth, ds->lightmapHeight,
                (int)(numPixels * sizeof(vec3_t)));
        if (tempColor) Q_Free(tempColor);
        if (tempDeluxe) Q_Free(tempDeluxe);
        if (tempEnergy) Q_Free(tempEnergy);
        return;
    }

    // Initialize temp buffers with existing values for all surfaces
    for (int ly = 0; ly < ds->lightmapHeight; ly++) {
        for (int lx = 0; lx < ds->lightmapWidth; lx++) {
            int py = ds->lightmapOffset[0][1] + ly;
            int px = ds->lightmapOffset[0][0] + lx;
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + py) * LIGHTMAP_WIDTH + px;
            int k_temp = ly * ds->lightmapWidth + lx;
            VectorCopy(&radiosityFloats[k_dst * 3], tempColor[k_temp]);
            if (game->deluxeMap) {
                VectorCopy(&radiosityDeluxeFloats[k_dst * 3], tempDeluxe[k_temp]);
                VectorCopy(&radiosityEnergyFloats[k_dst * 3], tempEnergy[k_temp]);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Optimized Triangle Soup Path: Cache-driven + Inclusive Dilation
    // -----------------------------------------------------------------------
    if (ds->surfaceType == MST_TRIANGLE_SOUP) {
        int numPoints = 0;
        voxelPoint_t *points = VoxelCache_Load(surfIdx, &numPoints);
        if (points) {
            byte *filled = calloc(numPixels, 1);
            vec3_t *pointPos = Q_Alloc(numPixels * sizeof(vec3_t));
            vec3_t *pointNorm = Q_Alloc(numPixels * sizeof(vec3_t));

            if (!filled || !pointPos || !pointNorm) {
                _printf("WARNING: RadiosityReconstruct: failed to allocate point buffers for trisoup surface %d (%dx%d). Falling back to rasterization.\n",
                        surfIdx, ds->lightmapWidth, ds->lightmapHeight);
                if (filled) free(filled);
                if (pointPos) Q_Free(pointPos);
                if (pointNorm) Q_Free(pointNorm);
                Q_Free(points);
            } else {
                // Step 1: Exact hits via Voxel Cache
                for (int i = 0; i < numPoints; i++) {
                    int pIdx = points[i].pixelIndex;
                    int lmLocal = pIdx % (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT);
                    int native_lx = lmLocal % LIGHTMAP_WIDTH - ds->lightmapOffset[0][0];
                    int native_ly = lmLocal / LIGHTMAP_WIDTH - ds->lightmapOffset[0][1];
                    
                    int k_temp = native_ly * ds->lightmapWidth + native_lx;

                    if (k_temp < 0 || k_temp >= numPixels) continue;

                    VectorCopy(points[i].pos, pointPos[k_temp]);
                    VectorCopy(points[i].normal, pointNorm[k_temp]);
                    VectorAdd(pointPos[k_temp], localSurfaces[surfIdx].entityOrigin, pointPos[k_temp]);

                    if (RadiosityVoxelSample(pointPos[k_temp], pointNorm[k_temp], tempColor[k_temp], game->deluxeMap ? tempDeluxe[k_temp] : NULL, game->deluxeMap ? tempEnergy[k_temp] : NULL)) {
                        filled[k_temp] = 1;
                        // Unmask if it was masked
                        if (lightAlphaMask && lightAlphaMask[pIdx] == 0) {
                            lightAlphaMask[pIdx] = ds->surfaceType;
                        }
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
                                    if (RadiosityVoxelSample(pointPos[kn], pointNorm[kn], tempColor[k_temp], game->deluxeMap ? tempDeluxe[k_temp] : NULL, game->deluxeMap ? tempEnergy[k_temp] : NULL)) {
                                        filled[k_temp] = 2; // Dilated
                                        // Unmask if it was masked
                                        // Note: We no longer modify lightAlphaMask here, as PrecacheTexelGeometry handles all geometry evaluation.
                                        goto next_p;
                                    }
                                }
                            }
                        }
                        next_p:;
                    }
                }

                free(filled); Q_Free(pointPos); Q_Free(pointNorm); Q_Free(points);
                goto flush; // Skip manual rasterization loop
            }
        } else {
            _printf("WARNING: RadiosityReconstruct: VoxelCache_Load returned NULL for trisoup surface %d. Falling back to rasterization.\n", surfIdx);
        }
    }

    for (int ly = 0; ly < ds->lightmapHeight; ly++) {
        for (int lx = 0; lx < ds->lightmapWidth; lx++) {
            int py = ds->lightmapOffset[0][1] + ly;
            int px = ds->lightmapOffset[0][0] + lx;
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + py) * LIGHTMAP_WIDTH + px;
            int k_temp = ly * ds->lightmapWidth + lx;
            
            if (unreachableMask && BITMAP_TEST(unreachableMask, k_dst)) {
                continue;
            }

            // Skip if masked, unless it's a triangle soup (which we might unmask)
            if (lightAlphaMask && lightAlphaMask[k_dst] != ds->surfaceType) {
                if (ds->surfaceType != MST_TRIANGLE_SOUP) continue;
            }

            // Resolve the per-texel normal for this pixel from precache
            int k_upscale = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT * scale + py * scale) * LIGHTMAP_WIDTH * scale + px * scale;
            if (texelNormals[k_upscale][0] == 0.0f && texelNormals[k_upscale][1] == 0.0f && texelNormals[k_upscale][2] == 0.0f)
                continue;

            vec3_t texelNormal;
            VectorCopy(texelNormals[k_upscale], texelNormal);

            if (ds->surfaceType != MST_TRIANGLE_SOUP) {
                // Determine native lx/ly for IsBorderPixel checks
                if (IsBorderPixel(lx, ly, ds->lightmapWidth, ds->lightmapHeight) || 
                    IsInteriorSparsePixel(lx, ly, surf_rad_interval)) {
                    continue; // Already evaluated at exact resolution, keep it.
                }
                RadiosityBilinearSample(ds, lx, ly, surf_rad_interval, texelNormal, tempColor[k_temp], game->deluxeMap ? tempDeluxe[k_temp] : NULL, game->deluxeMap ? tempEnergy[k_temp] : NULL);
                continue;
            }

            // Fallback path if cache was missing
            vec3_t pos;
            VectorCopy(texelOrigins[k_upscale], pos);
            // Note: We no longer modify lightAlphaMask here, as PrecacheTexelGeometry handles all geometry evaluation.

            if (RadiosityVoxelSample(pos, texelNormal, tempColor[k_temp], game->deluxeMap ? tempDeluxe[k_temp] : NULL, game->deluxeMap ? tempEnergy[k_temp] : NULL)) {
                // Keep sample
            }
        }
    }

flush:


    // Flush temp buffer back to radiosityFloats for this surface
    for (int ly = 0; ly < ds->lightmapHeight; ly++) {
        for (int lx = 0; lx < ds->lightmapWidth; lx++) {
            int py = ds->lightmapOffset[0][1] + ly;
            int px = ds->lightmapOffset[0][0] + lx;
            int k_dst = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + py) * LIGHTMAP_WIDTH + px;
            int k_temp = ly * ds->lightmapWidth + lx;
            VectorCopy(tempColor[k_temp], &radiosityFloats[k_dst * 3]);
            if (game->deluxeMap) {
                VectorCopy(tempDeluxe[k_temp], &radiosityDeluxeFloats[k_dst * 3]);
                VectorCopy(tempEnergy[k_temp], &radiosityEnergyFloats[k_dst * 3]);
            }
        }
    }



    Q_Free(tempColor);
    if (tempDeluxe) Q_Free(tempDeluxe);
    if (tempEnergy) Q_Free(tempEnergy);
    
}

static void RadiosityReconstructThread(int surfIdx) {
    int realSurfIndex = surfaceWorkOrder[surfIdx];
    dsurface_t *ds = &drawSurfaces[realSurfIndex];
    int surfWeight = (ds->lightmapNum[0] >= 0) ? (ds->lightmapWidth * ds->lightmapHeight) : 1;
    
    RadiosityReconstructOneSurface(realSurfIndex);
    
    ThreadCompletedWeighted(surfWeight);
}

// ---------------------------------------------------------------------------
// Phase 4 — Merge
// ---------------------------------------------------------------------------

static void RadiosityMerge(const float *srcBuffer) {
    int total = numLightBytes / 3;
    for (int i = 0; i < total; i++) {
        if (lightAlphaMask && !lightAlphaMask[i]) continue;
        
        if (game->deluxeMap) {
            vec3_t radDir;
            if (VectorNormalize(&accumRadiosityDeluxeSum[i*3], radDir) < 0.0001f) {
                VectorCopy(&normalFloats[i*3], radDir);
            }

            // Exaggerate the radiosity bent normal away from the surface normal
            if (deluxeRadiosityExaggerate > 1.0f) {
                float w = DotProduct(&normalFloats[i*3], radDir);
                vec3_t tangent;
                
                // Extract tangent
                for (int c = 0; c < 3; c++) {
                    tangent[c] = radDir[c] - (normalFloats[i*3+c] * w);
                }
                
                // Scale up the tangent to bend it further
                for (int c = 0; c < 3; c++) {
                    tangent[c] *= deluxeRadiosityExaggerate;
                }
                
                // Reconstruct and re-normalize
                for (int c = 0; c < 3; c++) {
                    radDir[c] = (normalFloats[i*3+c] * w) + tangent[c];
                }
                
                if (VectorNormalize(radDir, radDir) < 0.0001f) {
                    VectorCopy(&normalFloats[i*3], radDir);
                }

                // Clamp to maximum 75 degrees (cos(75) ≈ 0.2588190f, sin(75) ≈ 0.9659258f)
                float newW = DotProduct(&normalFloats[i*3], radDir);
                float maxCos = 0.258819045f; // cos(75)
                float maxSin = 0.965925826f; // sin(75)
                if (newW < maxCos) {
                    vec3_t pureTangent;
                    for (int c = 0; c < 3; c++) {
                        pureTangent[c] = radDir[c] - (normalFloats[i*3+c] * newW);
                    }
                    if (VectorNormalize(pureTangent, pureTangent) > 0.0001f) {
                        for (int c = 0; c < 3; c++) {
                            radDir[c] = (normalFloats[i*3+c] * maxCos) + (pureTangent[c] * maxSin);
                        }
                        VectorNormalize(radDir, radDir);
                    }
                }
            }
            MergeAccumulatedState(
                &lightFloats[i * 3], &deluxeFloats[i * 3], &energyFloats[i * 3],
                &srcBuffer[i * 3], radDir, &accumRadiosityEnergyFloats[i * 3],
                &normalFloats[i * 3], -1.0f
            );
        } else {
            VectorAdd(lightFloats + i * 3, srcBuffer + i * 3, lightFloats + i * 3);
        }
    }

    // Merge Vertices
    if (radiosityVertexFloats && internalDrawVerts) {
        for (int i = 0; i < numDrawVerts; i++) {
            internalDrawVerts[i].color[0][0] += radiosityVertexFloats[i * 3 + 0];
            internalDrawVerts[i].color[0][1] += radiosityVertexFloats[i * 3 + 1];
            internalDrawVerts[i].color[0][2] += radiosityVertexFloats[i * 3 + 2];
        }
    }

    // Merge Grid
    if (radiosityGridColors && gridData32 && numGridPoints > 0) {
        for (int i = 0; i < numGridPoints; i++) {
            gridData32[i].ambient[0][0] += radiosityGridColors[i * 3 + 0] * game->rad_gridscale;
            gridData32[i].ambient[0][1] += radiosityGridColors[i * 3 + 1] * game->rad_gridscale;
            gridData32[i].ambient[0][2] += radiosityGridColors[i * 3 + 2] * game->rad_gridscale;
            // Note: Since we gather isotropically from a sphere, all accumulated light is ambient
        }
    }
}

// ---------------------------------------------------------------------------
// LightRadiosity — public entry point
// ---------------------------------------------------------------------------

void LightRadiosity(void) {
    int radiosityPasses = game->radiosityPasses;
    rad_intensity = game->radiosityIntensity * 0.5f; // arbitrary rescaling so we default to 1.0
    rad_color_ratio = game->radiosityColorRatio;

    if (radiosityPasses <= 0 || rad_intensity <= 0.0f) return;
    _printf("--- Radiosity ---\n");

    if (rad_voxel_size <= 0.0f) {
        rad_voxel_size = (float)(game->defaultSampleSize * game->radiosityInterval);
    }
    if (rad_angle_match > 90.0f) rad_angle_match = 90.0f;
    rad_angle_match_cos = (float)cos(rad_angle_match * (M_PI / 180.0f));
    _printf("  Voxel Grid enabled (Size: %.1f, Angle Match: %.1f deg / %.2f cos)\n", 
            rad_voxel_size, rad_angle_match, rad_angle_match_cos);


    AllocateRadiosityFloats();
    memset(accumRadiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

    for (int pnum = 1; pnum <= radiosityPasses; pnum++) {
        double passStart = I_FloatTime();
        _printf("Pass %d/%d:\n", pnum, radiosityPasses);

        // AO Proximity-Fade Strategy: Keep AO on Pass 1 for sharp direct crevice shadows,
        // but bypass on Pass 2+ to let diffuse multi-bounce light naturally wash out
        // the corner crevices and prevent dark seams.
        active_rad_ao_intensity = (pnum == 1) ? game->rad_ao_intensity : 0.0f;

        const float *emitSource = (pnum == 1) ? lightFloats : radiosityFloats;
        _printf("  [emit]   ");
        RadiosityEmit(emitSource, pnum == 1);

        memset(radiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
        if (game->deluxeMap) {
            memset(radiosityDeluxeFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
            memset(radiosityEnergyFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
        }
        _printf("  [integrate]  ");
        fflush(stdout);
        RunThreadsOnWeighted(numDrawSurfaces, numTotalLuxels, qtrue, RadiosityIntegrateThread);
        _printf("done\n");

        RadiosityVoxelize(radiosityFloats, radiosityDeluxeFloats, radiosityEnergyFloats);

        
        _printf("  [reconstruct] Fill ");
        fflush(stdout);
        RunThreadsOnWeighted(numDrawSurfaces, numTotalLuxels, qtrue, RadiosityReconstructThread);
        _printf("done\n");

        if (game->deluxeMap) {
            for (int i = 0; i < numLightBytes / 3; i++) {
                if (lightAlphaMask && !lightAlphaMask[i]) continue;
                VectorAdd(accumRadiosityFloats + i * 3, radiosityFloats + i * 3, accumRadiosityFloats + i * 3);
                
                float lum = radiosityFloats[i*3]*0.299f + radiosityFloats[i*3+1]*0.587f + radiosityFloats[i*3+2]*0.114f;
                accumRadiosityDeluxeSum[i*3+0] += lum * radiosityDeluxeFloats[i*3+0];
                accumRadiosityDeluxeSum[i*3+1] += lum * radiosityDeluxeFloats[i*3+1];
                accumRadiosityDeluxeSum[i*3+2] += lum * radiosityDeluxeFloats[i*3+2];
                VectorAdd(&accumRadiosityEnergyFloats[i*3], &radiosityEnergyFloats[i*3], &accumRadiosityEnergyFloats[i*3]);
            }
        } else {
            for (int i = 0; i < numLightBytes / 3; i++) VectorAdd(accumRadiosityFloats + i * 3, radiosityFloats + i * 3, accumRadiosityFloats + i * 3);
        }
        Q_Free(g_emitters); g_emitters = NULL; g_numEmitters = 0;
        _printf("  Pass %d complete (%.0f seconds)\n\n", pnum, I_FloatTime() - passStart);
    }

    if (numGridPoints > 0) {
        _printf("--- Radiosity Lightgrid ---\n");
        _printf("  [voxelize] Building final cache with fully accumulated bounce light...\n");
        RadiosityVoxelize(accumRadiosityFloats, accumRadiosityDeluxeSum, accumRadiosityEnergyFloats);
        
        _printf("  [integrate] ");
        fflush(stdout);
        RunThreadsOnIndividual(numGridPoints, qtrue, RadiosityIntegrateGridThread);
        _printf("done\n");
    }

    _printf("--- Radiosity Merge ---\n");
    if (radiosityonly) {
        memset(lightFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
        if (game->deluxeMap) {
            memset(deluxeFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
            memset(energyFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
        }
    }
    RadiosityMerge(accumRadiosityFloats);
    _printf("done\n");

    FreeRadiosityFloats();
    RadiosityVoxelReset();
}
