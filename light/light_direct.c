#include "light.h"
#include "../common/imagelib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

int *surfaceWorkOrder;
extern int *texelSurfaceDebug;

// ---------------------------------------------------------------------------
// Ticket wavefront instrumentation (Phase 0 — print/count only).
// Filled by the ticket-wavefront conversion in later phases; see
// studies/wavefront_ticket_embree_plan.md §5.1. Integer event counts only:
// updates use #pragma omp atomic, so totals are exact regardless of order and
// output determinism is unaffected. No float sums ever use these.
// ---------------------------------------------------------------------------

static int c_ticketsGenerated;
static int c_ticketsSubmitted;
static int c_ticketsBlocked;
static int c_ticketsContributed;
static int c_ticketsRejected;
static int c_ticketGroups;
static int c_ticketFlushes;
static int c_groupOverflowGrow;
static int c_ticketStrandWarnings;  // TEMP guard: batch non-empty at texel start
static int c_ticketAppendWarnings;  // TEMP guard: Append returned NULL (reserve failed)

// ---------------------------------------------------------------------------
// Ticket batch helpers (Phase 1). Declared in light.h, unused until the
// Phase 3 conversion of TraceLights(). The trace loop below is the exact
// call TraceLine() will make per ticket; consumption (finish/accumulate)
// is added in Phases 2/3 between the trace loop and count reset.
// ---------------------------------------------------------------------------

void TicketBatch_Init(lightTicketBatch_t *batch, int capacity)
{
    batch->tickets = Q_Alloc((size_t)capacity * sizeof(lightTicket_t));
    if (!batch->tickets) {
        Error("TicketBatch_Init: failed to allocate %d tickets", capacity);
    }
    batch->count = 0;
    batch->capacity = capacity;
}

lightTicket_t *TicketBatch_Append(lightTicketBatch_t *batch)
{
    if (batch->count >= batch->capacity) {
        return NULL;
    }
    // Phase 0 instrumentation: integer event count, atomic (worker threads).
    #pragma omp atomic
    c_ticketsGenerated++;
    return &batch->tickets[batch->count++];
}

int TicketBatch_FlushTraces(lightTicketBatch_t *batch, traceWork_t *tw)
{
    int i;
    int n = batch->count;

    for (i = 0; i < n; i++) {
        lightTicket_t *t = &batch->tickets[i];
        // Exact same call the direct path makes today; testAll comes from the
        // ticket (sun qtrue, lights qfalse) so the flush loop matches both.
        TraceLine(t->origin, t->end, &t->trace, t->testAll, tw);
        #pragma omp atomic
        c_ticketsSubmitted++;
    }

    // Phase 3a: the consume step lives in TraceLights (TicketConsumeTickets),
    // reading tickets[0..n) AFTER this call and BEFORE any new Append — the
    // buffer is not overwritten until then, so returning the traced count is
    // safe. Reset happens here so a fresh group starts appending at 0.
    batch->count = 0;

    #pragma omp atomic
    c_ticketFlushes++;

    return n;
}

void TicketBatch_Free(lightTicketBatch_t *batch)
{
    // Existing per-surface code frees Q_Alloc'd memory with plain free()
    // (e.g. light_direct.c frees localLights/tw) — same discipline, no new
    // allocation rules introduced in this phase.
    free(batch->tickets);
    batch->tickets = NULL;
    batch->count = 0;
    batch->capacity = 0;
}

// 8-point Rotated Grid (tilted ~26.6 degrees)
static const float ssPattern8[][2] = {
    {0.000f, 0.000f}, // center
    {-0.354f, -0.854f},
    {0.354f, -0.354f},
    {0.854f, 0.146f},
    {0.354f, 0.646f},
    {-0.146f, 0.354f},
    {-0.646f, -0.146f},
    {-0.854f, 0.354f},
};
#define SS_PATTERN8_COUNT 8

// 16-point Halton(2,3) quasi-random sequence
static const float ssPattern16[][2] = {
    {0.000f, 0.000f}, // center
    {0.000f, -0.333f},
    {-0.500f, 0.333f},
    {0.500f, -0.778f},
    {-0.750f, -0.111f},
    {0.250f, 0.556f},
    {-0.250f, -0.556f},
    {0.750f, 0.111f},
    {-0.875f, 0.778f},
    {0.125f, -0.926f},
    {-0.375f, -0.259f},
    {0.625f, 0.407f},
    {-0.625f, -0.704f},
    {0.375f, -0.037f},
    {-0.125f, 0.630f},
    {0.875f, -0.481f},
};
#define SS_PATTERN16_COUNT 16

/*
===============================================================

LIGHT TRACING EXECUTION

===============================================================
*/

float CalculateSpecificFalloff(float dot, shadingModel_t falloff, float bias)
{
    float val = (dot > 1.0f) ? 1.0f : dot;

    // Apply uniform soft bias to all falloff types
    val = val * (1.0f - bias) + bias;
    if (val < 0.0f)
        val = 0.0f;

    if (falloff == SHADING_MODEL_HALFLAMBERT)
    {
        return val * val;
    }
    else if (falloff == SHADING_MODEL_UNREAL)
    {
        // Unreal angular part is standard Lambert
        return val;
    }
    else if (falloff == SHADING_MODEL_QUADRATIC)
    {
        val = 1.0f - val;
        return 1.0f - (val * val);
    }
    else if (falloff == SHADING_MODEL_DOUBLEQUADRATIC)
    {
        val = 1.0f - val;
        return 1.0f - (val * val * val);
    }
    return val;
}

float CalculateShadingModel(float dot)
{
    return CalculateSpecificFalloff(dot, game->shadingModel, shadingModelSoftBias);
}

/*
=================
TriSoupSamplePoint

Finds the position and normal for a lightmap sample point (st in pixel space)
on a triangle soup surface using barycentric interpolation.
=================
*/
qboolean TriSoupSamplePoint(dsurface_t *ds, float st[2], vec3_t origin, vec3_t normal, vec3_t outCentroid)
{
    int j, k;
    float st0[2], st1[2], st2[2];
    float area, w0, w1, w2;
    float bestExtrapolatedDistSq = 999999.0f;
    vec3_t bestExtrapOrigin;
    vec3_t bestExtrapNormal;
    vec3_t bestExtrapCentroid;
    VectorClear(bestExtrapCentroid);

    for (j = 0; j < ds->numIndexes; j += 3)
    {
        int i0 = drawIndexes[ds->firstIndex + j];
        int i1 = drawIndexes[ds->firstIndex + j + 1];
        int i2 = drawIndexes[ds->firstIndex + j + 2];

        drawVert_t *v0 = &drawVerts[ds->firstVert + i0];
        drawVert_t *v1 = &drawVerts[ds->firstVert + i1];
        drawVert_t *v2 = &drawVerts[ds->firstVert + i2];

        st0[0] = v0->lightmap[0][0] * LIGHTMAP_WIDTH;
        st0[1] = v0->lightmap[0][1] * LIGHTMAP_HEIGHT;
        st1[0] = v1->lightmap[0][0] * LIGHTMAP_WIDTH;
        st1[1] = v1->lightmap[0][1] * LIGHTMAP_HEIGHT;
        st2[0] = v2->lightmap[0][0] * LIGHTMAP_WIDTH;
        st2[1] = v2->lightmap[0][1] * LIGHTMAP_HEIGHT;

        // Fast Bounding Box rejection
        float mins[2], maxs[2];
        mins[0] = st0[0] < st1[0] ? (st0[0] < st2[0] ? st0[0] : st2[0]) : (st1[0] < st2[0] ? st1[0] : st2[0]);
        mins[1] = st0[1] < st1[1] ? (st0[1] < st2[1] ? st0[1] : st2[1]) : (st1[1] < st2[1] ? st1[1] : st2[1]);
        maxs[0] = st0[0] > st1[0] ? (st0[0] > st2[0] ? st0[0] : st2[0]) : (st1[0] > st2[0] ? st1[0] : st2[0]);
        maxs[1] = st0[1] > st1[1] ? (st0[1] > st2[1] ? st0[1] : st2[1]) : (st1[1] > st2[1] ? st1[1] : st2[1]);

        if (st[0] < mins[0] - GUTTER || st[0] > maxs[0] + GUTTER ||
            st[1] < mins[1] - GUTTER || st[1] > maxs[1] + GUTTER)
        {
            continue;
        }

        if (PointInTriangle(st[0], st[1], st0, st1, st2))
        {
            // Calculate barycentric coordinates
            area = (st1[1] - st2[1]) * (st0[0] - st2[0]) +
                   (st2[0] - st1[0]) * (st0[1] - st2[1]);
            if (fabs(area) < 0.0001f)
                continue;

            w0 = ((st1[1] - st2[1]) * (st[0] - st2[0]) +
                  (st2[0] - st1[0]) * (st[1] - st2[1])) /
                 area;
            w1 = ((st2[1] - st0[1]) * (st[0] - st2[0]) +
                  (st0[0] - st2[0]) * (st[1] - st2[1])) /
                 area;
            w2 = 1.0f - w0 - w1;

            for (k = 0; k < 3; k++)
            {
                origin[k] = w0 * v0->xyz[k] + w1 * v1->xyz[k] + w2 * v2->xyz[k];
                normal[k] =
                    w0 * v0->normal[k] + w1 * v1->normal[k] + w2 * v2->normal[k];
            }
            VectorNormalize(normal, normal);
            if (outCentroid)
            {
                outCentroid[0] = (v0->xyz[0] + v1->xyz[0] + v2->xyz[0]) / 3.0f;
                outCentroid[1] = (v0->xyz[1] + v1->xyz[1] + v2->xyz[1]) / 3.0f;
                outCentroid[2] = (v0->xyz[2] + v1->xyz[2] + v2->xyz[2]) / 3.0f;
            }
            return qtrue;
        }

        // Extrapolation (Tracing Dilation): if not inside, check if we are within the gutter distance
        // We now allow this for ALL trisoups (including planar-derived) because xatlas introduces padding gaps
        // that must be mathematically bridged by the raytracer to prevent dead sub-pixels in Upscale.
        // if (!isPlanar)
        {
            float dSq, dMin = 999999.0f;
            float t, bestT = 0.0f;
            int edgeBest = -1;

            dSq = DistanceSqToSegment(st[0], st[1], st0, st1, &t);
            if (dSq < dMin)
            {
                dMin = dSq;
                edgeBest = 0;
                bestT = t;
            }
            dSq = DistanceSqToSegment(st[0], st[1], st1, st2, &t);
            if (dSq < dMin)
            {
                dMin = dSq;
                edgeBest = 1;
                bestT = t;
            }
            dSq = DistanceSqToSegment(st[0], st[1], st2, st0, &t);
            if (dSq < dMin)
            {
                dMin = dSq;
                edgeBest = 2;
                bestT = t;
            }

            // Check if better than previous extrapolation
            if (edgeBest >= 0 && dMin < bestExtrapolatedDistSq)
            {
                float ct = bestT;
                if (ct < 0.0f) ct = 0.0f;
                if (ct > 1.0f) ct = 1.0f;

                if (edgeBest == 0) {
                    w0 = 1.0f - ct; w1 = ct; w2 = 0.0f;
                } else if (edgeBest == 1) {
                    w0 = 0.0f; w1 = 1.0f - ct; w2 = ct;
                } else {
                    w0 = ct; w1 = 0.0f; w2 = 1.0f - ct;
                }

                for (k = 0; k < 3; k++)
                {
                    bestExtrapOrigin[k] = w0 * v0->xyz[k] + w1 * v1->xyz[k] + w2 * v2->xyz[k];
                    bestExtrapNormal[k] =
                        w0 * v0->normal[k] + w1 * v1->normal[k] + w2 * v2->normal[k];
                }
                VectorNormalize(bestExtrapNormal, bestExtrapNormal);
                bestExtrapCentroid[0] = (v0->xyz[0] + v1->xyz[0] + v2->xyz[0]) / 3.0f;
                bestExtrapCentroid[1] = (v0->xyz[1] + v1->xyz[1] + v2->xyz[1]) / 3.0f;
                bestExtrapCentroid[2] = (v0->xyz[2] + v1->xyz[2] + v2->xyz[2]) / 3.0f;
                bestExtrapolatedDistSq = dMin;
            }
        }
    }

    // If we found no exact match but found a valid extrapolation, use it.
    // Clamp to 1 texel distance to prevent pushing the origin into adjacent concave walls.
    float maxExtrapDistSq = (float)GUTTER * GUTTER * 1.0f;

    if (bestExtrapolatedDistSq < maxExtrapDistSq)
    {
        VectorCopy(bestExtrapOrigin, origin);
        VectorCopy(bestExtrapNormal, normal);
        if (outCentroid)
        {
            VectorCopy(bestExtrapCentroid, outCentroid);
        }
        return qtrue;
    }

    return qfalse;
}

/*
=================
PlanarSamplePointInside

Checks if a 2D lightmap sample point (st in pixel space) is strictly inside
the triangles of a MST_PLANAR surface.
=================
*/
qboolean PlanarSamplePointInside(dsurface_t *ds, float st[2], vec3_t outCentroid)
{
    int j;
    float st0[2], st1[2], st2[2];
    float area, w0, w1, w2;
    float bestExtrapolatedDistSq = 999999.0f;
    vec3_t bestExtrapCentroid;
    VectorClear(bestExtrapCentroid);

    if (ds->numIndexes <= 0)
    {
        if (outCentroid && ds->numVerts > 0)
        {
            VectorClear(outCentroid);
            for (j = 0; j < ds->numVerts; j++)
                VectorAdd(outCentroid, drawVerts[ds->firstVert + j].xyz, outCentroid);
            VectorScale(outCentroid, 1.0f / ds->numVerts, outCentroid);
        }
        return qtrue; // Fallback if no indices are present
    }

    for (j = 0; j < ds->numIndexes; j += 3)
    {
        int i0 = drawIndexes[ds->firstIndex + j];
        int i1 = drawIndexes[ds->firstIndex + j + 1];
        int i2 = drawIndexes[ds->firstIndex + j + 2];

        drawVert_t *v0 = &drawVerts[ds->firstVert + i0];
        drawVert_t *v1 = &drawVerts[ds->firstVert + i1];
        drawVert_t *v2 = &drawVerts[ds->firstVert + i2];

        st0[0] = v0->lightmap[0][0] * LIGHTMAP_WIDTH;
        st0[1] = v0->lightmap[0][1] * LIGHTMAP_HEIGHT;
        st1[0] = v1->lightmap[0][0] * LIGHTMAP_WIDTH;
        st1[1] = v1->lightmap[0][1] * LIGHTMAP_HEIGHT;
        st2[0] = v2->lightmap[0][0] * LIGHTMAP_WIDTH;
        st2[1] = v2->lightmap[0][1] * LIGHTMAP_HEIGHT;

        // Fast Bounding Box rejection
        float mins[2], maxs[2];
        mins[0] = st0[0] < st1[0] ? (st0[0] < st2[0] ? st0[0] : st2[0]) : (st1[0] < st2[0] ? st1[0] : st2[0]);
        mins[1] = st0[1] < st1[1] ? (st0[1] < st2[1] ? st0[1] : st2[1]) : (st1[1] < st2[1] ? st1[1] : st2[1]);
        maxs[0] = st0[0] > st1[0] ? (st0[0] > st2[0] ? st0[0] : st2[0]) : (st1[0] > st2[0] ? st1[0] : st2[0]);
        maxs[1] = st0[1] > st1[1] ? (st0[1] > st2[1] ? st0[1] : st2[1]) : (st1[1] > st2[1] ? st1[1] : st2[1]);

        if (st[0] < mins[0] - (float)GUTTER || st[0] > maxs[0] + (float)GUTTER ||
            st[1] < mins[1] - (float)GUTTER || st[1] > maxs[1] + (float)GUTTER)
        {
            continue;
        }

        area = (st1[0] - st0[0]) * (st2[1] - st0[1]) -
               (st2[0] - st0[0]) * (st1[1] - st0[1]);
        if (fabs(area) < 0.0001f)
            continue;

        w0 = ((st1[1] - st2[1]) * (st[0] - st2[0]) +
              (st2[0] - st1[0]) * (st[1] - st2[1])) /
             area;
        w1 = ((st2[1] - st0[1]) * (st[0] - st2[0]) +
              (st0[0] - st2[0]) * (st[1] - st2[1])) /
             area;
        w2 = 1.0f - w0 - w1;

        if (w0 >= -0.0001f && w1 >= -0.0001f && w2 >= -0.0001f)
        {
            if (outCentroid)
            {
                outCentroid[0] = (v0->xyz[0] + v1->xyz[0] + v2->xyz[0]) / 3.0f;
                outCentroid[1] = (v0->xyz[1] + v1->xyz[1] + v2->xyz[1]) / 3.0f;
                outCentroid[2] = (v0->xyz[2] + v1->xyz[2] + v2->xyz[2]) / 3.0f;
            }
            return qtrue; // strictly inside
        }
        
        // Dilation: if not inside, check if we are within the gutter distance
        {
            float dSq, dMin = 999999.0f;
            float t;

            dSq = DistanceSqToSegment(st[0], st[1], st0, st1, &t);
            if (dSq < dMin) dMin = dSq;

            dSq = DistanceSqToSegment(st[0], st[1], st1, st2, &t);
            if (dSq < dMin) dMin = dSq;

            dSq = DistanceSqToSegment(st[0], st[1], st2, st0, &t);
            if (dSq < dMin) dMin = dSq;

            if (dMin < bestExtrapolatedDistSq)
            {
                bestExtrapCentroid[0] = (v0->xyz[0] + v1->xyz[0] + v2->xyz[0]) / 3.0f;
                bestExtrapCentroid[1] = (v0->xyz[1] + v1->xyz[1] + v2->xyz[1]) / 3.0f;
                bestExtrapCentroid[2] = (v0->xyz[2] + v1->xyz[2] + v2->xyz[2]) / 3.0f;
                bestExtrapolatedDistSq = dMin;
            }
        }
    }

    if (bestExtrapolatedDistSq < 999999.0f)
    {
        if (outCentroid)
            VectorCopy(bestExtrapCentroid, outCentroid);
        return qtrue;
    }

    return qfalse;
}

/*
=================
PatchSamplePoint

Finds the position and normal for a lightmap sample point (st in pixel space)
on a Bezier patch mesh using barycentric interpolation on the subdivided quads.
=================
*/
qboolean PatchSamplePoint(mesh_t *mesh, float st[2], vec3_t origin, vec3_t normal, vec3_t outCentroid)
{
    int mx, my, k;
    qboolean found = qfalse;
    vec3_t bestExtrapOrigin, bestExtrapNormal;
    vec3_t bestExtrapCentroid;
    VectorClear(bestExtrapCentroid);
    float bestExtrapDistSq = 999999.0f;

    for (my = 0; my < mesh->height - 1; my++)
    {
        for (mx = 0; mx < mesh->width - 1; mx++)
        {
            drawVert_t *v00 = &mesh->verts[my * mesh->width + mx];
            drawVert_t *v10 = &mesh->verts[my * mesh->width + mx + 1];
            drawVert_t *v01 = &mesh->verts[(my + 1) * mesh->width + mx];
            drawVert_t *v11 = &mesh->verts[(my + 1) * mesh->width + mx + 1];

            float st00[2] = {v00->lightmap[0][0] * LIGHTMAP_WIDTH, v00->lightmap[0][1] * LIGHTMAP_HEIGHT};
            float st10[2] = {v10->lightmap[0][0] * LIGHTMAP_WIDTH, v10->lightmap[0][1] * LIGHTMAP_HEIGHT};
            float st01[2] = {v01->lightmap[0][0] * LIGHTMAP_WIDTH, v01->lightmap[0][1] * LIGHTMAP_HEIGHT};
            float st11[2] = {v11->lightmap[0][0] * LIGHTMAP_WIDTH, v11->lightmap[0][1] * LIGHTMAP_HEIGHT};

            // Fast Bounding Box rejection
            float mins[2], maxs[2];
            mins[0] = st00[0];
            maxs[0] = st00[0];
            if (st10[0] < mins[0])
                mins[0] = st10[0];
            if (st10[0] > maxs[0])
                maxs[0] = st10[0];
            if (st01[0] < mins[0])
                mins[0] = st01[0];
            if (st01[0] > maxs[0])
                maxs[0] = st01[0];
            if (st11[0] < mins[0])
                mins[0] = st11[0];
            if (st11[0] > maxs[0])
                maxs[0] = st11[0];

            mins[1] = st00[1];
            maxs[1] = st00[1];
            if (st10[1] < mins[1])
                mins[1] = st10[1];
            if (st10[1] > maxs[1])
                maxs[1] = st10[1];
            if (st01[1] < mins[1])
                mins[1] = st01[1];
            if (st01[1] > maxs[1])
                maxs[1] = st01[1];
            if (st11[1] < mins[1])
                mins[1] = st11[1];
            if (st11[1] > maxs[1])
                maxs[1] = st11[1];

            if (st[0] < mins[0] - GUTTER || st[0] > maxs[0] + GUTTER ||
                st[1] < mins[1] - GUTTER || st[1] > maxs[1] + GUTTER)
            {
                continue;
            }

            // Check if ST is within this quad's bounds (using a small epsilon)
            if (st[0] >= mins[0] - 0.001f && st[0] <= maxs[0] + 0.001f &&
                st[1] >= mins[1] - 0.001f && st[1] <= maxs[1] + 0.001f)
            {

                float u = (maxs[0] > mins[0]) ? (st[0] - mins[0]) / (maxs[0] - mins[0]) : 0.0f;
                float v = (maxs[1] > mins[1]) ? (st[1] - mins[1]) / (maxs[1] - mins[1]) : 0.0f;

                // Clamp to [0, 1] for safety
                if (u < 0.0f)
                    u = 0.0f;
                if (u > 1.0f)
                    u = 1.0f;
                if (v < 0.0f)
                    v = 0.0f;
                if (v > 1.0f)
                    v = 1.0f;

                float norm_u = u;
                float norm_v = v;

                for (k = 0; k < 3; k++)
                {
                    if (v > u) {
                        origin[k] = (1.0f - v) * v00->xyz[k] +
                                    (v - u) * v01->xyz[k] +
                                    u * v11->xyz[k];
                    } else {
                        origin[k] = (1.0f - u) * v00->xyz[k] +
                                    (u - v) * v10->xyz[k] +
                                    v * v11->xyz[k];
                    }

                    normal[k] = (1.0f - norm_u) * (1.0f - norm_v) * v00->normal[k] +
                                norm_u * (1.0f - norm_v) * v10->normal[k] +
                                (1.0f - norm_u) * norm_v * v01->normal[k] +
                                norm_u * norm_v * v11->normal[k];
                }
                if (outCentroid)
                {
                    for (k = 0; k < 3; k++)
                    {
                        outCentroid[k] = (v00->xyz[k] + v10->xyz[k] + v01->xyz[k] + v11->xyz[k]) * 0.25f;
                    }
                }
                found = qtrue;
                break;
            }

            // Extrapolation / Dilation fallback: track closest quad by center distance
            float center_s = (mins[0] + maxs[0]) * 0.5f;
            float center_t = (mins[1] + maxs[1]) * 0.5f;
            float dSq = (st[0] - center_s) * (st[0] - center_s) + (st[1] - center_t) * (st[1] - center_t);

            if (dSq < bestExtrapDistSq)
            {
                bestExtrapDistSq = dSq;
                float u = (maxs[0] > mins[0]) ? (st[0] - mins[0]) / (maxs[0] - mins[0]) : 0.0f;
                float v = (maxs[1] > mins[1]) ? (st[1] - mins[1]) / (maxs[1] - mins[1]) : 0.0f;

                float norm_u = u;
                float norm_v = v;

                for (k = 0; k < 3; k++)
                {
                    if (v > u) {
                        bestExtrapOrigin[k] = (1.0f - v) * v00->xyz[k] +
                                              (v - u) * v01->xyz[k] +
                                              u * v11->xyz[k];
                    } else {
                        bestExtrapOrigin[k] = (1.0f - u) * v00->xyz[k] +
                                              (u - v) * v10->xyz[k] +
                                              v * v11->xyz[k];
                    }

                    bestExtrapNormal[k] = (1.0f - norm_u) * (1.0f - norm_v) * v00->normal[k] +
                                          norm_u * (1.0f - norm_v) * v10->normal[k] +
                                          (1.0f - norm_u) * norm_v * v01->normal[k] +
                                          norm_u * norm_v * v11->normal[k];
                    
                    bestExtrapCentroid[k] = (v00->xyz[k] + v10->xyz[k] + v01->xyz[k] + v11->xyz[k]) * 0.25f;
                }
            }
        }
        if (found)
            break;
    }

    if (!found)
    {
        // Allow extrapolation if within reasonable gutter distance
        if (bestExtrapDistSq < (float)GUTTER * GUTTER * 4.0f)
        {
            for (k = 0; k < 3; k++)
            {
                origin[k] = bestExtrapOrigin[k];
                normal[k] = bestExtrapNormal[k];
            }
            if (outCentroid)
            {
                VectorCopy(bestExtrapCentroid, outCentroid);
            }
            VectorNormalize(normal, normal);
            return qtrue;
        }
        else
        {
            return qfalse;
        }
    }

    VectorNormalize(normal, normal);
    return qtrue;
}

/*
================
PointToPolygonFormFactor
================
*/
float PointToPolygonFormFactor(const vec3_t point, const vec3_t normal,
                               const winding_t *w)
{
    vec3_t triVector, triNormal;
    int i, j;
    vec3_t dirs[MAX_POINTS_ON_WINDING];
    float total;
    float dot, angle, facing;

    for (i = 0; i < w->numpoints; i++)
    {
        VectorSubtract(w->points[i], point, dirs[i]);
        VectorNormalize(dirs[i], dirs[i]);
    }

    // duplicate first vertex to avoid mod operation
    VectorCopy(dirs[0], dirs[i]);

    total = 0;
    for (i = 0; i < w->numpoints; i++)
    {
        j = i + 1;
        dot = DotProduct(dirs[i], dirs[j]);

        // roundoff can cause slight creep, which gives an IND from acos
        if (dot > 1.0)
        {
            dot = 1.0;
        }
        else if (dot < -1.0)
        {
            dot = -1.0;
        }

        angle = acos(dot);
        CrossProduct(dirs[i], dirs[j], triVector);
        if (VectorNormalize(triVector, triNormal) < 0.0001)
        {
            continue;
        }
        facing = DotProduct(normal, triNormal);
        total += facing * angle;

        if (total > 6.3 || total < -6.3)
        {
            static qboolean printed;

            if (!printed)
            {
                printed = qtrue;
                _printf("WARNING: bad PointToPolygonFormFactor: %f at %1.1f %1.1f "
                        "%1.1f from %1.1f %1.1f %1.1f\n",
                        total, w->points[i][0], w->points[i][1], w->points[i][2], point[0], point[1],
                        point[2]);
            }
            return 0;
        }
    }

    total /= 2 * 3.141592657; // now in the range of 0 to 1 over the entire
                              // incoming hemisphere

    return total;
}

/*
================
SunToPoint

Returns an amount of light to add at the point (grid)

Ticket wavefront (Phase 2): split into prepare/finish at the TraceLine() call.
The wrapper keeps the exact original behavior — nothing moved across the
trace boundary (golden trace-count invariant).
================
*/
static qboolean TicketPrepare_Sun(const vec3_t origin, lightTicket_t *t,
                                  qboolean applyColorFilter)
{
    if (!numSkyBrushes || !hasSun)
    {
        return qfalse;
    }

    t->light = NULL;
    VectorCopy(origin, t->origin);
    VectorMA(origin, MAX_WORLD_COORD * 2, sunDirection, t->end);
    t->dist = 0.0f;
    t->add = 0.0f;
    t->angle = 0.0f;
    t->applyColorFilter = applyColorFilter;
    t->testAll = qtrue;

    return qtrue;
}

// Sun-plane prepare (Phase 3a): the facing culls + falloff angle that Phase 2
// kept in the SunToPlane wrapper, followed by the sun body prepare. Used by
// BOTH the SunToPlane wrapper and the Phase 3 ticket generator, so the culls
// and angle exist in exactly one place. t->angle carries the falloff; the
// finish still does NOT set out->angle (TraceGrid relies on that).
static qboolean TicketPrepare_SunPlane(const vec3_t origin, const vec3_t normal,
                                       lightTicket_t *t, qboolean applyColorFilter,
                                       traceWork_t *tw)
{
    float angle;

    if (!numSkyBrushes || !hasSun)
    {
        return qfalse;
    }

    // if the sun is behind the surface
    if (tw->forceFrontOnly)
    {
        if (DotProduct(normal, sunDirection) < -0.125f)
        {
            return qfalse; // facing away
        }
    }
    else if (game->sunShadingModel != SHADING_MODEL_HALFLAMBERT &&
             game->sunShadingModel != SHADING_MODEL_LAMBERT)
    {
        if (DotProduct(normal, sunDirection) <= 0)
        {
            return qfalse; // facing away
        }
    }

    angle = CalculateSpecificFalloff(DotProduct(normal, sunDirection), game->sunShadingModel, sunSoftBias);
    if (angle <= 0)
    {
        return qfalse; // facing away
    }

    // sun body prepare (re-checks sky, computes the far end point)
    if (!TicketPrepare_Sun(origin, t, applyColorFilter))
    {
        return qfalse;
    }
    t->angle = angle;
    VectorCopy(normal, t->normal);

    return qtrue;
}

static qboolean TicketFinish_Sun(lightTicket_t *t, contribution_t *out)
{
    // If the ray hit a solid occluder in Embree, it cannot be the sky!
    if (t->trace.passSolid)
    {
        return qfalse;
    }

    // We reached the void/sky without being blocked by a solid wall.
    // So we add sunlight.
    if (!t->applyColorFilter)
    {
        t->trace.filter[0] = t->trace.filter[1] = t->trace.filter[2] = 1.0f;
    }

    VectorCopy(sunDirection, out->dir);
    out->light = NULL;
    out->irradiance[0] = t->trace.filter[0] * sunLight[0];
    out->irradiance[1] = t->trace.filter[1] * sunLight[1];
    out->irradiance[2] = t->trace.filter[2] * sunLight[2];
    out->isGlow = qfalse;
    out->ambientClampScale = 1.0f;
    out->ambientAbsMax = sunGridMaxAmbient;
    out->directAbsMax = sunGridMaxDirect;
    out->gridAmbientScale = sunGridAmbientScale;
    out->gridDirectScale = sunGridDirectScale;
    out->familyCount = 1;

    return qtrue;
}

qboolean SunToPoint(const vec3_t origin, traceWork_t *tw, contribution_t *out,
                    qboolean applyColorFilter)
{
    lightTicket_t t;

    memset(&t, 0, sizeof(t));

    if (!TicketPrepare_Sun(origin, &t, applyColorFilter))
    {
        return qfalse;
    }

    TraceLine(t.origin, t.end, &t.trace, t.testAll, tw);

    return TicketFinish_Sun(&t, out);
}

/*
================
SunToPlane
Returns an amount of light to add at the texel (surface)

Ticket wavefront (Phase 3a): the facing culls + falloff angle moved into
TicketPrepare_SunPlane (shared with the Phase 3 ticket generator); the
wrapper is now prepare/trace/finish with the angle carried in the ticket.
================
*/
qboolean SunToPlane(const vec3_t origin, const vec3_t normal,
                    contribution_t *out, qboolean applyColorFilter,
                    traceWork_t *tw)
{
    lightTicket_t t;

    memset(&t, 0, sizeof(t));

    if (!TicketPrepare_SunPlane(origin, normal, &t, applyColorFilter, tw))
    {
        return qfalse;
    }

    TraceLine(t.origin, t.end, &t.trace, t.testAll, tw);

    if (TicketFinish_Sun(&t, out))
    {
        out->angle = t.angle;
        return qtrue;
    }

    return qfalse;
}

/*
========================
MergeAccumulatedState
========================
*/
void MergeAccumulatedState(vec3_t color, vec3_t dir, vec3_t energy,
                           const vec3_t addColor, const vec3_t addDir,
                           const vec3_t addEnergy, const vec3_t normal, float deluxeMinAngle)
{
    int i;
    vec3_t currentRadiance, addedRadiance, targetRadiance;
    for (i = 0; i < 3; i++)
    {
        currentRadiance[i] = color[i];
        addedRadiance[i] = addColor[i];
        targetRadiance[i] = currentRadiance[i] + addedRadiance[i];
    }

    // Step 2: Vector Blending (Luminance-weighted)
    float lumCurrent = currentRadiance[0] * 0.299f + currentRadiance[1] * 0.587f + currentRadiance[2] * 0.114f;
    float lumAdded = addedRadiance[0] * 0.299f + addedRadiance[1] * 0.587f + addedRadiance[2] * 0.114f;

    vec3_t vNew;
    for (i = 0; i < 3; i++)
        vNew[i] = lumCurrent * dir[i] + lumAdded * addDir[i];

    vec3_t dirNew;
    // Persistence Check: Only allow the direction to shift if the total energy 
    // is above the stability threshold. If it's too dim, we keep the previous 
    // direction (which might be a dilated vector or the normal) to prevent 
    // unstable "jitter" or the loss of high-quality dilated vectors.
    if (lumCurrent + lumAdded > MIN_DELUXE_ENERGY)
    {
        if (VectorNormalize(vNew, dirNew) < 0.0001f)
        {
            VectorCopy(normal, dirNew);
        }
    }
    else
    {
        VectorCopy(dir, dirNew);
    }

    // Step 3: In-Game Falloff Verification
    float w = DotProduct(normal, dirNew);
    if (w < 0.01f) w = 0.01f;

    float lumTarget = targetRadiance[0] * 0.299f + targetRadiance[1] * 0.587f + targetRadiance[2] * 0.114f;

    vec3_t energyNew;
    for (i = 0; i < 3; i++)
        energyNew[i] = energy[i] + addEnergy[i];
    float lumEnergyNew = energyNew[0] * 0.299f + energyNew[1] * 0.587f + energyNew[2] * 0.114f;

    // Step 4: Bending (if cap violated)
    if (lumEnergyNew > MIN_DELUXE_ENERGY && (lumTarget / w) > lumEnergyNew)
    {
        float wNeeded = lumTarget / lumEnergyNew;
        if (wNeeded > 1.0f) wNeeded = 1.0f;

        // 8-iteration binary search
        float tLo = 0.0f, tHi = 1.0f;
        for (int iter = 0; iter < 8; iter++)
        {
            float tMid = (tLo + tHi) * 0.5f;
            vec3_t candidate;
            for (i = 0; i < 3; i++)
                candidate[i] = dirNew[i] * (1.0f - tMid) + normal[i] * tMid;
            VectorNormalize(candidate, candidate);
            float wCandidate = DotProduct(normal, candidate);
            if (wCandidate < wNeeded)
                tLo = tMid;
            else
                tHi = tMid;
        }

        // Final corrected direction
        for (i = 0; i < 3; i++)
            dirNew[i] = dirNew[i] * (1.0f - tHi) + normal[i] * tHi;
        VectorNormalize(dirNew, dirNew);
        w = DotProduct(normal, dirNew);
        if (w < 0.01f) w = 0.01f;
    }

    // Step 4.5: Angle Floor (if enabled)
    float effDeluxeMinAngle = (deluxeMinAngle >= 0.0f) ? deluxeMinAngle : game->deluxeMinAngle;
    if (effDeluxeMinAngle > 0.0f)
    {
        // deluxeMinAngle is the angle TO THE SURFACE.
        // The angle to the normal is 90 - deluxeMinAngle.
        // cos(90 - A) = sin(A).
        float minCos = sin(effDeluxeMinAngle * (M_PI / 180.0f));
        if (w < minCos)
        {
            float wNeeded = minCos;
            float tLo = 0.0f, tHi = 1.0f;
            for (int iter = 0; iter < 8; iter++)
            {
                float tMid = (tLo + tHi) * 0.5f;
                vec3_t candidate;
                for (i = 0; i < 3; i++)
                    candidate[i] = dirNew[i] * (1.0f - tMid) + normal[i] * tMid;
                VectorNormalize(candidate, candidate);
                float wCandidate = DotProduct(normal, candidate);
                if (wCandidate < wNeeded)
                    tLo = tMid;
                else
                    tHi = tMid;
            }

            // Final corrected direction for angle floor
            for (i = 0; i < 3; i++)
                dirNew[i] = dirNew[i] * (1.0f - tHi) + normal[i] * tHi;
            VectorNormalize(dirNew, dirNew);
            w = DotProduct(normal, dirNew);
            if (w < 0.01f) w = 0.01f;
        }
    }

    // Step 5: Commit
    for (i = 0; i < 3; i++)
        color[i] = targetRadiance[i];

    VectorCopy(dirNew, dir);
    VectorCopy(energyNew, energy);
}

void AccumulateContribution(vec3_t color, vec3_t dir, vec3_t energy, const contribution_t *cont, const vec3_t normal, float deluxeMinAngle)
{
    if (!color)
        return;

    // Standard path: no deluxe data, just accumulate color
    if (!dir || !energy)
    {
        color[0] += cont->irradiance[0] * cont->angle;
        color[1] += cont->irradiance[1] * cont->angle;
        color[2] += cont->irradiance[2] * cont->angle;
        return;
    }

    // --- V2 Deluxe Iterative Algorithm ---
    vec3_t addColor, addDir, addEnergy;
    
    float angle = cont->angle;
    if (angle < 0.0f) angle = 0.0f;

    addColor[0] = cont->irradiance[0] * angle;
    addColor[1] = cont->irradiance[1] * angle;
    addColor[2] = cont->irradiance[2] * angle;

    if (cont->isGlow) {
        VectorCopy(normal, addDir);
    } else {
        VectorCopy(cont->dir, addDir);
    }

    VectorCopy(cont->irradiance, addEnergy);

    MergeAccumulatedState(color, dir, energy, addColor, addDir, addEnergy, normal, deluxeMinAngle);
}

/*
========================
LightContributionToPoint

Ticket wavefront (Phase 2): split into prepare/finish at the TraceLine() call.
Prepare = exactly the pre-trace logic of the original (area :906-941 /
point :1083-1173); finish = exactly the post-trace logic (area :950-1077 /
point :1177-1196). Nothing moved across the trace boundary — rejects that
today happen post-trace (form factor, glow, min_light_add) stay in finish,
so the golden trace count is unchanged.
========================
*/
static qboolean TicketPrepare_Light(const light_t *light, const vec3_t origin,
                                    const vec3_t normal, lightTicket_t *t)
{
    vec3_t n;
    float dist;

    // area light (Exact Point-To-Polygon Form Factor)
    if (light->type == emit_area)
    {
        float d;

        // instant reach check
        VectorSubtract(light->origin, origin, n);
        dist = VectorLength(n);
        if (dist > light->reach)
        {
            return qfalse;
        }

        // see if the light is behind the receiver's normal
        if (normal && DotProduct(n, normal) < 0)
        {
            if (!light->twosided)
            {
                // only cull if the shader doesn't explicitly allow back-glow
                if (!light->si || light->si->surfaceLightGlow <= 0.0f)
                {
                    return qfalse;
                }
            }
        }

        // see if the point is behind the light
        d = DotProduct(origin, light->normal) - light->dist;
        if (!light->twosided)
        {
            if (d < 1)
            {
                return qfalse;
            }
        }

        VectorSubtract(light->origin, origin, n);
        if (VectorNormalize(n, n) == 0)
        {
            return qfalse;
        }

        t->light = light;
        VectorCopy(origin, t->origin);
        VectorCopy(light->origin, t->end);
        t->dist = dist;
        VectorCopy(n, t->dir);
        if (normal) VectorCopy(normal, t->normal); // NULL on the grid path (no consume there)
        t->add = 0.0f;
        t->angle = 0.0f;
        t->applyColorFilter = qfalse;
        t->testAll = qfalse;

        return qtrue;
    }

    // point or spotlight logic
    if (light->type == emit_point || light->type == emit_spotlight)
    {
        float add = 0;
        float surfaceAngle = 1.0f;
        float coneScale = 1.0f;

        VectorSubtract(light->origin, origin, n);
        dist = VectorNormalize(n, t->dir);
        if (dist > light->reach)
        {
            return qfalse;
        }
        if (dist < 16)
        {
            dist = 16;
        }

        // surface falloff
        if (normal)
        {
            surfaceAngle = CalculateShadingModel(DotProduct(normal, t->dir));
            if (surfaceAngle <= 0)
            {
                return qfalse;
            }
        }

        add = CalculateAttenuation(light, dist, light->attenuationModel, light->prestep);

        // Early distance cull: skip expensive spotlight vector math if distance alone kills it
        if (add <= MIN_LIGHT_ADD)
        {
            return qfalse;
        }

        if (light->type == emit_spotlight)
        {
            float softness;
            float safetyFloor;
            float distByNormal;
            float sampleRadius;
            vec3_t pointAtDist;
            vec3_t distToSample;
            float radiusAtDist;

            distByNormal = -DotProduct(t->dir, light->normal) * dist;
            if (distByNormal < 0)
            {
                return qfalse;
            }
            VectorMA(light->origin, distByNormal, light->normal, pointAtDist);
            radiusAtDist = light->radiusByDist * distByNormal;
            VectorSubtract(origin, pointAtDist, distToSample);
            sampleRadius = VectorLength(distToSample);

            if (sampleRadius >= radiusAtDist)
            {
                return qfalse;
            }

            if (light->coneSoftness > 0.0f)
            {
                softness = SPOTLIGHT_SOFTNESS_RANGE * light->coneSoftness;
                if (softness < 0.01f)
                    softness = 0.01f;

                coneScale = (radiusAtDist - sampleRadius) / softness;

                // Safety Floor: ensure peak intensity at center doesn't drop below 20%
                // of what a standard linear cone would provide at that radius.
                safetyFloor = 0.05f * (radiusAtDist - sampleRadius) / radiusAtDist;

                if (coneScale < safetyFloor)
                    coneScale = safetyFloor;

                if (coneScale > 1.0f)
                    coneScale = 1.0f;
            }

            add *= coneScale;
        }

        // Second MIN_LIGHT_ADD reject (original :1247-1250): the spotlight
        // cone scale can drop `add` below the threshold after the first
        // reject passed. This is a PRE-trace reject — dropping it changes
        // the trace set (found via the Phase 2 gate bisect).
        if (add <= MIN_LIGHT_ADD)
        {
            return qfalse;
        }

        t->light = light;
        VectorCopy(origin, t->origin);
        VectorCopy(light->origin, t->end);
        t->dist = dist;
        t->add = add;
        t->angle = surfaceAngle;
        if (normal) VectorCopy(normal, t->normal); // NULL on the grid path (no consume there)
        t->applyColorFilter = qfalse;
        t->testAll = qfalse;

        return qtrue;
    }

    return qfalse;
}

static qboolean TicketFinish_Light(lightTicket_t *t, const vec3_t origin,
                                   const vec3_t normal, contribution_t *out)
{
    const light_t *light = t->light;

    // area light finish
    if (light->type == emit_area)
    {
        // Original :1022-1025 — the area branch's occlusion reject lives
        // immediately after its TraceLine. Without it, blocked area rays
        // would contribute light through walls (filter stays 1,1,1).
        if (t->trace.passSolid)
        {
            return qfalse;
        }

        float factor;
        vec3_t n;
        float angle;
        float formFactorBase;
        float outAngle;
        qboolean outIsGlow = qfalse;

        // The original set out->dir (== n) pre-trace and reads it below;
        // same value, set here from the prepared ticket before any read.
        VectorCopy(t->dir, out->dir);
        VectorCopy(t->dir, n);

        // Liquid surfaces act as omnidirectional glowing volumes, not flat Lambertian emitters.
        // They emit "plain light" in all directions without cosine falloff.
        if (light->si && (light->si->contents & (CONTENTS_LAVA | CONTENTS_SLIME |
                                                 CONTENTS_WATER | CONTENTS_FOG)))
        {
            vec3_t toLight;
            VectorSubtract(light->origin, origin, toLight);
            float distToLightSq = DotProduct(toLight, toLight);
            factor = light->area / (distToLightSq + light->area);
        }
        else
        {
            // Standard Lambertian area light
            factor = PointToPolygonFormFactor(origin, n, light->w);
            if (factor <= 0)
            {
                if (light->twosided)
                {
                    factor = -factor;
                }
                else
                {
                    return qfalse;
                }
            }
        }

        angle = (factor < 0.0f) ? 0.0f : factor;
        if (angle <= 0)
        {
            return qfalse;
        }

        if (normal)
        {
            float dot = DotProduct(normal, out->dir);
            float receiveAngle = (dot < 0.0f) ? 0.0f : dot;
            if (receiveAngle <= 0)
            {
                // NOTICE: If we allow this light to go through it must not contribute to light direction (deluxemaps). It's a "glow"

                // for some light sources allow a small fraction of contribution on backfaces creating a "glow effect"
                float glowFactor = 0.0f;
                if (light->si)
                {
                    if (light->si->surfaceLightGlow >= 0.0f)
                    {
                        glowFactor = light->si->surfaceLightGlow;
                    }
                    else
                    {
                        // defaults if not explicitly configured
                        if (light->si->contents & CONTENTS_LAVA)
                        {
                            glowFactor = 0.25f;
                        }
                        else if (light->si->contents & CONTENTS_SLIME)
                        {
                            glowFactor = 0.10f;
                        }
                    }
                }

                if (glowFactor > 0.0f)
                {
                    formFactorBase = angle * glowFactor;
                    outAngle = 1.0f;
                    outIsGlow = qtrue;
                }
                else
                {
                    return qfalse;
                }
            }
            else
            {
                formFactorBase = angle / receiveAngle;
                outAngle = receiveAngle;
            }
        }
        else
        {
            formFactorBase = angle;
            outAngle = 1.0f;
        }

        /* Cutoff on the un-faded contribution first, then the fade: the reverse order hard-clips mid-ramp. */
        out->irradiance[0] = light->emitColor[0] * formFactorBase * t->trace.filter[0];
        out->irradiance[1] = light->emitColor[1] * formFactorBase * t->trace.filter[1];
        out->irradiance[2] = light->emitColor[2] * formFactorBase * t->trace.filter[2];

        float maxAdd = out->irradiance[0];
        if (out->irradiance[1] > maxAdd) maxAdd = out->irradiance[1];
        if (out->irradiance[2] > maxAdd) maxAdd = out->irradiance[2];

        if (maxAdd <= light->min_light_add)
        {
            return qfalse;
        }

        if (light->attnSoftnessRange > 0.0f)
        {
            float fadeStartDist = light->reach - light->attnSoftnessRange;
            if (t->dist > fadeStartDist)
            {
                float fadeScale = (light->reach - t->dist) / light->attnSoftnessRange;
                out->irradiance[0] *= fadeScale;
                out->irradiance[1] *= fadeScale;
                out->irradiance[2] *= fadeScale;
            }
        }

        out->angle = outAngle;
        out->isGlow = outIsGlow;
        out->light = light;
        int fCount = light->familyCount;
        if (fCount <= 0) fCount = 1;
        out->ambientClampScale = 1.0f / (float)fCount;
        out->ambientAbsMax = light->gridMaxAmbient;
        out->directAbsMax = light->gridMaxDirect;
        out->gridAmbientScale = light->gridAmbientScale;
        out->gridDirectScale = light->gridDirectScale;
        out->familyCount = fCount;
        return qtrue;
    }

    // point/spotlight finish (prepare guarantees point/spot here)
    if (t->trace.passSolid)
    {
        return qfalse;
    }

    VectorCopy(t->dir, out->dir);
    out->angle = t->angle;
    out->isGlow = qfalse;
    out->irradiance[0] = t->add * light->color[0] * t->trace.filter[0];
    out->irradiance[1] = t->add * light->color[1] * t->trace.filter[1];
    out->irradiance[2] = t->add * light->color[2] * t->trace.filter[2];
    out->light = light;

    int fCount = light->familyCount;
    if (fCount <= 0) fCount = 1;
    out->ambientClampScale = 1.0f / (float)fCount;
    out->ambientAbsMax = light->gridMaxAmbient;
    out->directAbsMax = light->gridMaxDirect;
    out->gridAmbientScale = light->gridAmbientScale;
    out->gridDirectScale = light->gridDirectScale;
    out->familyCount = fCount;

    return qtrue;
}

qboolean LightContributionToPoint(const light_t *light, const vec3_t origin,
                                  const vec3_t normal, contribution_t *out,
                                  traceWork_t *tw)
{
    lightTicket_t t;

    memset(&t, 0, sizeof(t));

    if (!TicketPrepare_Light(light, origin, normal, &t))
    {
        return qfalse;
    }

    TraceLine(t.origin, t.end, &t.trace, t.testAll, tw);

    return TicketFinish_Light(&t, origin, normal, out);
}

/*
========================
LightingAtSample
========================
*/
void LightingAtSample(const vec3_t origin, const vec3_t normal, vec3_t color,
                      vec3_t dir, vec3_t energy,
                      qboolean testOcclusion, qboolean forceSunLight,
                      qboolean applyColorFilter, light_t **lightList,
                      int numLights, traceWork_t *tw, float deluxeMinAngle)
{
    light_t *light;
    contribution_t cont;
    int i;

    VectorClear(color);

    // Initialize deluxe state if provided
    if (dir)
        VectorCopy(normal, dir);
    if (energy)
        VectorClear(energy);

    // trace directly to the sun FIRST among all actual light sources
    if (testOcclusion || forceSunLight)
    {
        if (SunToPlane(origin, normal, &cont, applyColorFilter, tw))
        {
            AccumulateContribution(color, dir, energy, &cont, normal, deluxeMinAngle);
        }
    }

    if (lightList)
    {
        for (i = 0; i < numLights; i++)
        {
            light = lightList[i];
            if (LightContributionToPoint(light, origin, normal, &cont, tw))
            {
                if (game->deluxeMap && light->noDeluxeInfluence && dir)
                {
                    VectorCopy(dir, cont.dir);
                    cont.isGlow = qfalse;
                }
                AccumulateContribution(color, dir, energy, &cont, normal, deluxeMinAngle);
            }
        }
    }
    else
    {
        for (light = lights; light; light = light->next)
        {
            if (LightContributionToPoint(light, origin, normal, &cont, tw))
            {
                if (game->deluxeMap && light->noDeluxeInfluence && dir)
                {
                    VectorCopy(dir, cont.dir);
                    cont.isGlow = qfalse;
                }
                AccumulateContribution(color, dir, energy, &cont, normal, deluxeMinAngle);
            }
        }
    }
}

/*
=============
VertexLighting

Vertex lighting will completely ignore occlusion, because
shadows would not be resolvable anyway.
=============
*/
void VertexLighting(dsurface_t *ds, qboolean testOcclusion,
                    qboolean forceSunLight, float scale, light_t **lightList,
                    int numLights, traceWork_t *tw)
{
    int i;
    drawVert_t *dv;
    vec3_t sample, normal;
    float max;

    VectorCopy(ds->lightmapVecs[2], normal);

    // generate vertex lighting
    for (i = 0; i < ds->numVerts; i++)
    {
        vec3_t v_origin;
        dv = &drawVerts[ds->firstVert + i];

        if (ds->patchWidth || ds->surfaceType == MST_TRIANGLE_SOUP)
        {
            VectorMA(dv->xyz, SAMPLE_NUDGE, dv->normal, v_origin);
        }
        else
        {
            VectorMA(dv->xyz, SAMPLE_NUDGE, normal, v_origin);
        }

        if (BoxInSolid(v_origin, 4.0f, qfalse))
        {
            VectorClear(sample);
        }
        else
        {
            if (ds->patchWidth || ds->surfaceType == MST_TRIANGLE_SOUP)
            {
                LightingAtSample(v_origin, dv->normal, sample, NULL, NULL, testOcclusion,
                                 forceSunLight, qfalse, lightList, numLights, tw, 0.0f);
            }
            else
            {
                LightingAtSample(v_origin, normal, sample, NULL, NULL, testOcclusion,
                                 forceSunLight, qfalse, lightList, numLights, tw, 0.0f);
            }
        }

        if (scale >= 0)
            VectorScale(sample, scale, sample);

        // clamp with color normalization
        max = sample[0];
        if (sample[1] > max)
        {
            max = sample[1];
        }
        if (sample[2] > max)
        {
            max = sample[2];
        }
        if (max > 255)
        {
            VectorScale(sample, 255 / max, sample);
        }

        // save the high-precision result only
        if (internalDrawVerts)
        {
            internalDrawVerts[ds->firstVert + i].color[0][0] += sample[0];
            internalDrawVerts[ds->firstVert + i].color[0][1] += sample[1];
            internalDrawVerts[ds->firstVert + i].color[0][2] += sample[2];
        }
    }
}

/*
=================
LinearSubdivideMesh

For extra lighting, just midpoint one of the axis.
The edges are clamped at the original edges.
=================
*/
mesh_t *LinearSubdivideMesh(mesh_t *in)
{
    int i, j;
    mesh_t *out;
    drawVert_t *v1, *v2, *vout;

    out = Q_Alloc(sizeof(*out));

    out->width = in->width * 2;
    out->height = in->height;
    out->verts = Q_Alloc(out->width * out->height * sizeof(*out->verts));
    for (j = 0; j < in->height; j++)
    {
        out->verts[j * out->width + 0] = in->verts[j * in->width + 0];
        out->verts[j * out->width + out->width - 1] =
            in->verts[j * in->width + in->width - 1];
        for (i = 1; i < out->width - 1; i += 2)
        {
            v1 = in->verts + j * in->width + (i >> 1);
            v2 = v1 + 1;
            vout = out->verts + j * out->width + i;

            vout->xyz[0] = 0.75 * v1->xyz[0] + 0.25 * v2->xyz[0];
            vout->xyz[1] = 0.75 * v1->xyz[1] + 0.25 * v2->xyz[1];
            vout->xyz[2] = 0.75 * v1->xyz[2] + 0.25 * v2->xyz[2];

            vout->normal[0] = 0.75 * v1->normal[0] + 0.25 * v2->normal[0];
            vout->normal[1] = 0.75 * v1->normal[1] + 0.25 * v2->normal[1];
            vout->normal[2] = 0.75 * v1->normal[2] + 0.25 * v2->normal[2];

            VectorNormalize(vout->normal, vout->normal);

            vout++;

            vout->xyz[0] = 0.25 * v1->xyz[0] + 0.75 * v2->xyz[0];
            vout->xyz[1] = 0.25 * v1->xyz[1] + 0.75 * v2->xyz[1];
            vout->xyz[2] = 0.25 * v1->xyz[2] + 0.75 * v2->xyz[2];

            vout->normal[0] = 0.25 * v1->normal[0] + 0.75 * v2->normal[0];
            vout->normal[1] = 0.25 * v1->normal[1] + 0.75 * v2->normal[1];
            vout->normal[2] = 0.25 * v1->normal[2] + 0.75 * v2->normal[2];

            VectorNormalize(vout->normal, vout->normal);
        }
    }

    FreeMesh(in);

    return out;
}


/*
=============
PrecacheTexelGeometry
=============
*/
void PrecacheTexelGeometryThread(int i)
{
    int x, y, k;
    dsurface_t *ds;
    vec3_t lightmapOrigin, lightmapVecs[3];
    ds = &drawSurfaces[i];
    int surfWeight = ds->numVerts;
    if (ds->lightmapNum[0] >= 0) {
        surfWeight += ds->lightmapWidth * ds->lightmapHeight;
    }

    if (ds->lightmapNum[0] < 0) {
        ThreadCompletedWeighted(surfWeight);
        return;
    }

    int currentGutter = upscale ? (GUTTER * 2) : GUTTER;
    int scale = upscale ? 2 : 1;

    int sampleWidth = ds->lightmapWidth * scale + currentGutter * 2;
    int sampleHeight = ds->lightmapHeight * scale + currentGutter * 2;

    if (ds->surfaceType != MST_PATCH)
    {
        VectorCopy(ds->lightmapVecs[2], lightmapVecs[2]);
        VectorCopy(ds->lightmapOrigin, lightmapOrigin);
        if (ds->surfaceType == MST_PLANAR)
        {
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[0], lightmapOrigin);
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[1], lightmapOrigin);
        }
        VectorCopy(ds->lightmapVecs[0], lightmapVecs[0]);
        VectorCopy(ds->lightmapVecs[1], lightmapVecs[1]);
    }

    for (x = 0; x < sampleWidth; x++)
    {
        for (y = 0; y < sampleHeight; y++)
        {
            int py = ds->lightmapOffset[0][1] * scale + y - currentGutter;
            int px = ds->lightmapOffset[0][0] * scale + x - currentGutter;

            if (px < 0 || px >= LIGHTMAP_WIDTH * scale || py < 0 || py >= LIGHTMAP_HEIGHT * scale)
                continue;

            int idx = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT * scale + py) * LIGHTMAP_WIDTH * scale + px;

            float step = 1.0f / (float)scale;
            float u = ((float)(x - currentGutter) + 0.5f) * step;
            float v = ((float)(y - currentGutter) + 0.5f) * step;
            
            vec3_t origin, normal, centroid;
            qboolean hit = qtrue;
            float st[2];
            mesh_t *mesh = localSurfaces[i].patchMesh;
            
            st[0] = (float)ds->lightmapOffset[0][0] + u;
            st[1] = (float)ds->lightmapOffset[0][1] + v;

            if (ds->surfaceType == MST_TRIANGLE_SOUP)
            {
                if (!TriSoupSamplePoint(ds, st, origin, normal, centroid))
                    hit = qfalse;
            }
            else if (ds->surfaceType == MST_PATCH)
            {
                if (!mesh) {
                    hit = qfalse;
                } else {
                    if (!PatchSamplePoint(mesh, st, origin, normal, centroid))
                        hit = qfalse;
                }
            }
            else
            {
                for (k = 0; k < 3; k++)
                {
                    origin[k] = lightmapOrigin[k] + u * lightmapVecs[0][k] + v * lightmapVecs[1][k];
                    normal[k] = lightmapVecs[2][k];
                }
                
                // Validate if this texel actually falls inside the planar polygon
                if (!PlanarSamplePointInside(ds, st, centroid))
                    hit = qfalse;
            }

            if (hit)
            {
                float texelSize = localSurfaces[i].sampleSize;
                if (texelSize < 1.0f) texelSize = (float)game->defaultSampleSize;
                texelSize /= scale;
                
                float margin = texelSize * 1.5f;
                if (BoxInSolid(origin, margin, qfalse)) {
                    hit = qfalse; // Cull deeply buried texel
                }
            }

            if (hit)
            {
                vec3_t toCentroid;
                vec3_t nudgeOffset;
                VectorClear(nudgeOffset);

                VectorSubtract(centroid, origin, toCentroid);
                float dist = VectorLength(toCentroid);
                if (dist > 0.001f) {
                    VectorScale(toCentroid, 1.0f / dist, toCentroid);
                    float nudgeDist = SAMPLE_NUDGE;
                    if (nudgeDist > dist * 0.5f) {
                        nudgeDist = dist * 0.5f;
                    }
                    VectorScale(toCentroid, nudgeDist, nudgeOffset);
                }

                for (k = 0; k < 3; k++)
                {
                    texelOrigins[idx][k] = origin[k] + normal[k] * SAMPLE_NUDGE + nudgeOffset[k] + localSurfaces[i].entityOrigin[k];
                    texelNormals[idx][k] = normal[k];
                }
            }
            else
            {
                for (k = 0; k < 3; k++)
                {
                    texelOrigins[idx][k] = 0.0f;
                    texelNormals[idx][k] = 0.0f;
                }
            }

            if (texelSurfaceDebug[idx] != -1 && texelSurfaceDebug[idx] != i)
            {
                _printf("WARNING: Texel overlap! Surface %d overwriting %d at px=%d py=%d (lmNum=%d)\n",
                        i, texelSurfaceDebug[idx], px, py, ds->lightmapNum[0]);
            }
            texelSurfaceDebug[idx] = i;
        }
    }

    ThreadCompletedWeighted(surfWeight);
}

void PrecacheTexelGeometry(void)
{
    int i;
    numTotalLuxels = 0;
    
    for (i = 0; i < numDrawSurfaces; i++) {
        int weight = drawSurfaces[i].numVerts;
        if (drawSurfaces[i].lightmapNum[0] >= 0)
            weight += drawSurfaces[i].lightmapWidth * drawSurfaces[i].lightmapHeight;
        numTotalLuxels += weight;
    }

    _printf("--- PrecacheTexelGeometry ---\n");
    RunThreadsOnWeighted(numDrawSurfaces, numTotalLuxels, qtrue, PrecacheTexelGeometryThread);
    _printf("\n");
}

/*
================
TicketConsumeTickets

Ticket wavefront (Phase 3b, Stage 3): consume a flushed batch of tickets in
order — finish each ticket, fold the per-group sub-state into the caller's
per-texel accumulators on group boundaries, and classify the counters.

Group state machine: chunk = one texel, so groups are delimited by ssIdx
(Phase 3b-widening must extend detection to texelIdx changes). The caller
guarantees no empty groups in the stream (empty groups fold inline in the
generation stage).

PARITY NOTES (see plan §6 Phase 3 audit corrections):
- The per-group subDir initializes to the sample NORMAL, not zeros
  (LightingAtSample does VectorCopy(normal, dir)) — replicating that here
  with the group's first ticket's normal.
- The fold belongs to the PREVIOUS group: curGroupNormal is tracked per
  group and updated on group change.
- Failed finishes count blocked/rejected and NOTHING ELSE (no override,
  no accumulate — the old code is guarded by the success return).
- The noDeluxeInfluence override applies to lights only, never the sun,
  and only when the deluxe accumulators exist.
- The sun's cont.angle is set here from the ticket (replicating the old
  SunToPlane post-finish set); TicketFinish_Sun deliberately does not set
  it (TraceGrid relies on that).
================
*/
static void TicketConsumeTickets(lightTicket_t *tickets, int n,
                                 vec3_t accumColor, vec3_t accumDir,
                                 vec3_t accumEnergy, vec3_t accumNormal,
                                 qboolean deluxe, float deluxeMinAngle)
{
    contribution_t cont;
    vec3_t subColor, subDir, subEnergy;
    vec3_t curGroupNormal;
    qboolean haveGroup = qfalse;
    int groupSS = 0;
    int i;

    for (i = 0; i < n; i++)
    {
        lightTicket_t *t = &tickets[i];
        qboolean ok;

        if (!haveGroup || t->ssIdx != groupSS)
        {
            // fold the previous group's sub-state, then start a new group
            if (haveGroup)
            {
                if (deluxe)
                {
                    VectorAdd(accumColor, subColor, accumColor);
                    VectorAdd(accumDir, subDir, accumDir);
                    VectorAdd(accumEnergy, subEnergy, accumEnergy);
                    VectorAdd(accumNormal, curGroupNormal, accumNormal);
                }
                else
                {
                    VectorAdd(accumColor, subColor, accumColor);
                }
            }

            // sub-state init exactly as LightingAtSample does it
            if (deluxe)
            {
                VectorClear(subColor);
                VectorCopy(t->normal, subDir);      // subDir starts as the sample normal!
                VectorClear(subEnergy);
            }
            else
            {
                VectorClear(subColor);
            }
            VectorCopy(t->normal, curGroupNormal);
            groupSS = t->ssIdx;
            haveGroup = qtrue;
        }

        if (t->light == NULL)
        {
            // sun ticket
            ok = TicketFinish_Sun(t, &cont);
            if (ok)
            {
                cont.angle = t->angle;
            }
        }
        else
        {
            ok = TicketFinish_Light(t, t->origin, t->normal, &cont);
        }

        if (ok)
        {
            #pragma omp atomic
            c_ticketsContributed++;

            if (t->light != NULL && game->deluxeMap && t->light->noDeluxeInfluence && deluxe)
            {
                VectorCopy(subDir, cont.dir);
                cont.isGlow = qfalse;
            }

            AccumulateContribution(subColor, deluxe ? subDir : NULL, deluxe ? subEnergy : NULL,
                                   &cont, curGroupNormal, deluxeMinAngle);
        }
        else
        {
            if (t->trace.passSolid)
            {
                #pragma omp atomic
                c_ticketsBlocked++;
            }
            else
            {
                #pragma omp atomic
                c_ticketsRejected++;
            }
        }
    }

    // fold the final group
    if (haveGroup)
    {
        if (deluxe)
        {
            VectorAdd(accumColor, subColor, accumColor);
            VectorAdd(accumDir, subDir, accumDir);
            VectorAdd(accumEnergy, subEnergy, accumEnergy);
            VectorAdd(accumNormal, curGroupNormal, accumNormal);
        }
        else
        {
            VectorAdd(accumColor, subColor, accumColor);
        }
    }
}

/*
=============
TraceLights
=============
*/
void TraceLights(int num)
{
    int i, j, k;
    int realSurfIndex;
    dsurface_t *ds;
    light_t *light;
    float d;
    vec3_t v;
    double base[3];
    vec3_t origin, normal;
    traceWork_t *tw;
    lightTicketBatch_t batch;   // Ticket wavefront (Phase 3b)
    byte **occluded = NULL;
    byte *occluded_data = NULL;
    vec3_t **color = NULL;
    vec3_t *color_data = NULL;
    mesh_t *mesh = NULL;
    int sampleWidth, sampleHeight;
    int extW, extH;
    vec3_t lightmapOrigin, lightmapVecs[2];
    int surfWeight;
    light_t **localLights;
    int numLocalLights = 0;
    float wrapThreshold = 0.0f;

    tw = Q_Alloc(sizeof(traceWork_t));
    if (!tw)
        Error("Failed to allocate TraceLights memory (traceWork_t)");
    memset(tw, 0, sizeof(traceWork_t));
    tw->ignoreSurface = -1;

    realSurfIndex = surfaceWorkOrder[num];
    ds = &drawSurfaces[realSurfIndex];
    shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

    surfWeight = (ds->lightmapNum[0] >= 0) ? (ds->lightmapWidth * ds->lightmapHeight) : 1;

    // Build local light list for this surface
    localLights = Q_Alloc(numLights * sizeof(light_t *));
    numLocalLights = 0;

    if (shadingModelSoftBias > 0.0f && shadingModelSoftBias < 1.0f)
    {
        wrapThreshold = -shadingModelSoftBias / (1.0f - shadingModelSoftBias);
        if (wrapThreshold < -1.0f)
            wrapThreshold = -1.0f;
    }
    else if (shadingModelSoftBias >= 1.0f)
    {
        wrapThreshold = -1.0f;
    }
    else
    {
        wrapThreshold = 0.0f;
    }

    // Pass 1: Lights that are processed first
    for (light = lights; light; light = light->next)
    {
        // If deluxeSort is ENABLED: Process standard lights first, no-influence lights LATER
        // If deluxeSort is DISABLED: Process no-influence lights first (Legacy/Anchor behavior)
        if (deluxeSort)
        {
            if (light->noDeluxeInfluence) continue;
        }
        else
        {
            if (!light->noDeluxeInfluence) continue;
        }

        // 1. Distance check
        VectorSubtract(light->origin, localSurfaces[realSurfIndex].origin, v);
        d = VectorLength(v);
        if (d > light->reach + localSurfaces[realSurfIndex].radius)
        {
            continue;
        }

        // 2. Normal check (terminator)
        if (ds->surfaceType == MST_PLANAR || ds->surfaceType == MST_TRIANGLE_SOUP)
        {
            // If we have a constant or average normal, we can cull
            // For now, let's use the surface normal for MST_PLANAR
            if (ds->surfaceType == MST_PLANAR)
            {
                if (light->type == emit_area)
                {
                    // Area lights always use standard lambertian falloff.
                }
                else if (d > 0.001f)
                {
                    VectorSubtract(light->origin, localSurfaces[realSurfIndex].origin, v);
                    VectorScale(v, 1.0f / d, v); // Safely normalize using precomputed distance

                    // Unified culling: use CalculateShadingModel on the "best possible" dot product for this surface
                    float bestDot = DotProduct(v, ds->lightmapVecs[2]) + (localSurfaces[realSurfIndex].radius / d);
                    if (CalculateShadingModel(bestDot) <= 0)
                    {
                        continue;
                    }
                }
            }
        }

        localLights[numLocalLights++] = light;
    }

    // Pass 2: Lights that are processed last
    for (light = lights; light; light = light->next)
    {
        // If deluxeSort is ENABLED: Process no-influence lights now
        // If deluxeSort is DISABLED: Process standard lights now
        if (deluxeSort)
        {
            if (!light->noDeluxeInfluence) continue;
        }
        else
        {
            if (light->noDeluxeInfluence) continue;
        }

        // 1. Distance check
        VectorSubtract(light->origin, localSurfaces[realSurfIndex].origin, v);
        d = VectorLength(v);
        if (d > light->reach + localSurfaces[realSurfIndex].radius)
        {
            continue;
        }

        // 2. Normal check (terminator)
        if (ds->surfaceType == MST_PLANAR || ds->surfaceType == MST_TRIANGLE_SOUP)
        {
            // If we have a constant or average normal, we can cull
            // For now, let's use the surface normal for MST_PLANAR
            if (ds->surfaceType == MST_PLANAR)
            {
                if (light->type == emit_area)
                {
                    // Area lights always use standard lambertian falloff.
                }
                else if (d > 0.001f)
                {
                    VectorSubtract(light->origin, localSurfaces[realSurfIndex].origin, v);
                    VectorScale(v, 1.0f / d, v); // Safely normalize using precomputed distance

                    // Unified culling: use CalculateShadingModel on the "best possible" dot product for this surface
                    float bestDot = DotProduct(v, ds->lightmapVecs[2]) + (localSurfaces[realSurfIndex].radius / d);
                    if (CalculateShadingModel(bestDot) <= 0)
                    {
                        continue;
                    }
                }
            }
        }

        localLights[numLocalLights++] = light;
    }

    // vertex-lit triangle model if no lightmap allocated
    if (ds->surfaceType == MST_TRIANGLE_SOUP && ds->lightmapNum[0] == -1)
    {
        tw->ignoreSurface = realSurfIndex;
        VertexLighting(ds, !si->noVertexShadows, si->forceSunLight, 1.0, localLights, numLocalLights, tw);
        free(localLights);
        free(tw);
        ThreadCompletedWeighted(surfWeight);
        return;
    }

    if (ds->lightmapNum[0] == -1)
    {
        free(localLights);
        free(tw);
        ThreadCompletedWeighted(surfWeight);
        return; // doesn't need lighting at all
    }

    if (!novertexlighting && !nodirect)
    {
        // calculate the vertex lighting for gouraud shade mode
        tw->ignoreSurface = realSurfIndex;
        VertexLighting(ds, si->vertexShadows, si->forceSunLight, si->vertexScale,
                       localLights, numLocalLights, tw);
    }

    if (ds->lightmapNum[0] < 0)
    {
        free(localLights);
        free(tw);
        ThreadCompletedWeighted(surfWeight);
        return; // doesn't need lightmap lighting
    }

    int use_upscale = localSurfaces[realSurfIndex].upscale > 1;
    int isDilated = use_upscale || (ds->surfaceType == MST_TRIANGLE_SOUP);

    tw->patchshadows = patchshadows;
    tw->forceFrontOnly = qtrue;

    int scale = use_upscale ? UPSCALE_FACTOR : 1;
    int currentGutter = isDilated ? (GUTTER * scale) : 0;

    if (ds->surfaceType == MST_PATCH)
    {
        mesh = localSurfaces[realSurfIndex].patchMesh;


        // We don't need to manually subdivide further; we will interpolate in the loop.
        sampleWidth = ds->lightmapWidth * scale + currentGutter * 2;
        sampleHeight = ds->lightmapHeight * scale + currentGutter * 2;
    }
    else
    {
        VectorCopy(ds->lightmapVecs[2], normal);
        VectorCopy(ds->lightmapOrigin, lightmapOrigin);

        if (ds->surfaceType == MST_PLANAR)
        {
            // Shift origin from center to the top-left edge to unify math with Patches/TriSoup
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[0], lightmapOrigin);
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[1], lightmapOrigin);
        }

        if (!isDilated)
        {
            VectorCopy(ds->lightmapVecs[0], lightmapVecs[0]);
            VectorCopy(ds->lightmapVecs[1], lightmapVecs[1]);
            sampleWidth = ds->lightmapWidth;
            sampleHeight = ds->lightmapHeight;
        }
        else
        {
            // sample at a closer spacing for antialiasing
            if (use_upscale)
            {
                float invScale = 1.0f / (float)scale;
                VectorScale(ds->lightmapVecs[0], invScale, lightmapVecs[0]);
                VectorScale(ds->lightmapVecs[1], invScale, lightmapVecs[1]);
            }
            else
            {
                VectorCopy(ds->lightmapVecs[0], lightmapVecs[0]);
                VectorCopy(ds->lightmapVecs[1], lightmapVecs[1]);
            }

            sampleWidth = ds->lightmapWidth * scale + currentGutter * 2;
            sampleHeight = ds->lightmapHeight * scale + currentGutter * 2;
        }
    }

    extW = sampleWidth;
    extH = sampleHeight;

    occluded = Q_Alloc(extW * sizeof(byte *));
    occluded_data = Q_Alloc(extW * extH * sizeof(byte));
    color = Q_Alloc(extW * sizeof(vec3_t *));
    color_data = Q_Alloc(extW * extH * sizeof(vec3_t));
    byte *sampleHit_data = Q_Alloc(extW * extH * sizeof(byte));
    byte **sampleHit = Q_Alloc(extW * sizeof(byte *));

    // Deluxe arrays (direction + energy per texel)
    vec3_t *deluxe_data = NULL;
    vec3_t **deluxe = NULL;
    vec3_t *lmenergy_data = NULL;
    vec3_t **lmenergy = NULL;
    vec3_t *normalArray_data = NULL;
    vec3_t **normalArray = NULL;
    if (deluxeFloats)
    {
        deluxe_data = Q_Alloc(extW * extH * sizeof(vec3_t));
        deluxe = Q_Alloc(extW * sizeof(vec3_t *));
        lmenergy_data = Q_Alloc(extW * extH * sizeof(vec3_t));
        lmenergy = Q_Alloc(extW * sizeof(vec3_t *));
        normalArray_data = Q_Alloc(extW * extH * sizeof(vec3_t));
        normalArray = Q_Alloc(extW * sizeof(vec3_t *));
    }

    if (!occluded || !occluded_data || !color || !color_data || !sampleHit || !sampleHit_data)
    {
        _printf("WARNING: Failed to allocate TraceLights memory for surface %d (%dx%d)\n", realSurfIndex, extW, extH);
        if (occluded)
            free(occluded);
        if (occluded_data)
            free(occluded_data);
        if (color)
            free(color);
        if (color_data)
            free(color_data);
        if (sampleHit)
            free(sampleHit);
        if (sampleHit_data)
            free(sampleHit_data);
        free(localLights);
        free(tw);
        return;
    }

    memset(color_data, 0, extW * extH * sizeof(vec3_t));
    memset(sampleHit_data, 0, extW * extH * sizeof(byte));
    if (deluxe_data)
    {
        memset(deluxe_data, 0, extW * extH * sizeof(vec3_t));
        memset(lmenergy_data, 0, extW * extH * sizeof(vec3_t));
        memset(normalArray_data, 0, extW * extH * sizeof(vec3_t));
    }

    for (i = 0; i < extW; i++)
    {
        occluded[i] = occluded_data + i * extH;
        color[i] = color_data + i * extH;
        sampleHit[i] = sampleHit_data + i * extH;
        if (deluxe)
        {
            deluxe[i] = deluxe_data + i * extH;
            lmenergy[i] = lmenergy_data + i * extH;
            normalArray[i] = normalArray_data + i * extH;
        }
    }

    // Resolve per-surface supersampling and pattern selection
    float ssRadius = localSurfaces[realSurfIndex].superSampleRadius;
    qboolean doSS = (ssRadius > 0.0f);
    const float (*pattern)[2] = ssPattern8;
    int actualSamples = 1;
    float jitterRadius = 0.0f;

    if (doSS)
    {
        int ssize = game->defaultSampleSize;
        if (si && si->lightmapSampleSize)
        {
            ssize = si->lightmapSampleSize;
        }
        if (ssize <= 0)
        {
            ssize = 4;
        }

        if (ssRadius > 0.5f * (float)ssize)
        {
            pattern = ssPattern16;
            actualSamples = SS_PATTERN16_COUNT;
        }
        else
        {
            pattern = ssPattern8;
            actualSamples = SS_PATTERN8_COUNT;
        }

        jitterRadius = ssRadius;
    }

    // Ticket wavefront (Phase 3b): per-surface batch. Capacity is
    // TICKET_BATCH, grown to fit one whole group (sun + all local lights)
    // when needed, so a group is never split across flushes.
    {
        int batchCapacity = TICKET_BATCH;
        if (batchCapacity < numLocalLights + 1)
        {
            batchCapacity = numLocalLights + 1;
            #pragma omp atomic
            c_groupOverflowGrow++;
        }
        TicketBatch_Init(&batch, batchCapacity);
    }

    // determine which samples are occluded
    memset(occluded_data, 0, extW * extH * sizeof(byte));
    for (i = 0; i < sampleWidth; i++)
    {
        for (j = 0; j < sampleHeight; j++)
        {
            vec3_t accumColor;
            vec3_t accumDir, accumEnergy, accumNormal;
            int hitCount;

            VectorClear(accumColor);
            VectorClear(accumDir);
            VectorClear(accumEnergy);
            VectorClear(accumNormal);
            hitCount = 0;
            int ss, k;

            // Ticket wavefront guard: the texel-end flush guarantees an
            // empty batch at every texel start. If this ever fires, a
            // group leaked across texels — order no longer guaranteed.
            if (batch.count != 0)
            {
                #pragma omp atomic
                c_ticketStrandWarnings++;
            }

            // Map current sample to global lightmap index for PrecacheTexelGeometry lookup
            int py = ds->lightmapOffset[0][1] * scale + j - currentGutter;
            int px = ds->lightmapOffset[0][0] * scale + i - currentGutter;

            if (px < 0 || px >= LIGHTMAP_WIDTH * scale || py < 0 || py >= LIGHTMAP_HEIGHT * scale)
            {
                sampleHit[i][j] = qfalse;
                occluded[i][j] = qtrue;
                continue;
            }

            int p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT * scale + py) * LIGHTMAP_WIDTH * scale + px;

            for (ss = 0; ss < actualSamples; ss++)
            {
                float jdx = 0.0f, jdy = 0.0f;
                if (jitterRadius > 0.0f && ss > 0)
                {
                    int pidx = ss % actualSamples;
                    jdx = pattern[pidx][0] * jitterRadius;
                    jdy = pattern[pidx][1] * jitterRadius;
                }

                // Add 0.5f to move from the texel edge to the exact center
                float u = (float)(i - currentGutter) + jdx + 0.5f;
                float v = (float)(j - currentGutter) + jdy + 0.5f;
                float step = 1.0f / (float)scale;

                int global_scale = upscale ? 2 : 1;
                if (ss == 0 && scale == global_scale)
                {
                    int native_px = px / scale;
                    int native_py = py / scale;
                    int native_p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + native_py) * LIGHTMAP_WIDTH + native_px;

                    if (unreachableMask && BITMAP_TEST(unreachableMask, native_p))
                        continue;

                    if (texelNormals[p][0] == 0.0f && texelNormals[p][1] == 0.0f && texelNormals[p][2] == 0.0f)
                        continue;

                    for (k = 0; k < 3; k++)
                    {
                        base[k] = texelOrigins[p][k];
                        normal[k] = texelNormals[p][k];
                    }
                }
                else
                {
                    int native_px = px / scale;
                    int native_py = py / scale;
                    int native_p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + native_py) * LIGHTMAP_WIDTH + native_px;

                    if (ss == 0 && unreachableMask && BITMAP_TEST(unreachableMask, native_p))
                        continue;

                    vec3_t temp_origin;
                    vec3_t centroid;
                    qboolean hasCentroid = qfalse;

                    if (ds->surfaceType == MST_TRIANGLE_SOUP)
                    {
                        float st[2];
                        st[0] = (float)ds->lightmapOffset[0][0] + u * step;
                        st[1] = (float)ds->lightmapOffset[0][1] + v * step;

                        if (!TriSoupSamplePoint(ds, st, temp_origin, normal, centroid))
                            continue;
                        hasCentroid = qtrue;
                    }
                    else if (ds->surfaceType == MST_PATCH)
                    {
                        float target_s = (float)ds->lightmapOffset[0][0] + u * step;
                        float target_t = (float)ds->lightmapOffset[0][1] + v * step;
                        float st[2] = {target_s, target_t};

                        if (!PatchSamplePoint(mesh, st, temp_origin, normal, centroid))
                            continue;
                        hasCentroid = qtrue;
                    }
                    else
                    {
                        if (ds->surfaceType == MST_PLANAR)
                        {
                            float st[2];
                            st[0] = (float)ds->lightmapOffset[0][0] + u * step;
                            st[1] = (float)ds->lightmapOffset[0][1] + v * step;
                            if (!PlanarSamplePointInside(ds, st, centroid))
                                continue;
                            hasCentroid = qtrue;
                        }

                        for (k = 0; k < 3; k++)
                        {
                            temp_origin[k] = lightmapOrigin[k] +
                                             u * lightmapVecs[0][k] +
                                             v * lightmapVecs[1][k];
                        }
                    }

                    vec3_t nudgeOffset;
                    VectorClear(nudgeOffset);

                    if (hasCentroid)
                    {
                        vec3_t toCentroid;
                        VectorSubtract(centroid, temp_origin, toCentroid);
                        float dist = VectorLength(toCentroid);
                        if (dist > 0.001f) {
                            VectorScale(toCentroid, 1.0f / dist, toCentroid);
                            float nudgeDist = SAMPLE_NUDGE;
                            if (nudgeDist > dist * 0.5f) {
                                nudgeDist = dist * 0.5f;
                            }
                            VectorScale(toCentroid, nudgeDist, nudgeOffset);
                        }
                    }

                    for (k = 0; k < 3; k++)
                    {
                        base[k] = (double)temp_origin[k] +
                                  (double)normal[k] * SAMPLE_NUDGE +
                                  (double)nudgeOffset[k];
                    }

                    for (k = 0; k < 3; k++)
                    {
                        base[k] += localSurfaces[realSurfIndex].entityOrigin[k];
                    }
                }

                // convert to vec3_t for raycast
                for (k = 0; k < 3; k++)
                {
                    origin[k] = (float)base[k];
                }

                // If this is a supersample or differently scaled surface, PrecacheTexelGeometryThread didn't cull it.
                // We MUST enforce the culling here, otherwise it receives direct sunlight in the void.
                if (!(ss == 0 && scale == global_scale))
                {
                    float texelSize = localSurfaces[realSurfIndex].sampleSize;
                    if (texelSize < 1.0f) texelSize = (float)game->defaultSampleSize;
                    texelSize /= scale;
                    float margin = texelSize * 1.5f;
                    
                    if (localSurfaces[realSurfIndex].upscale > 1) {
                        margin *= 2.0f;
                    }

                    if (BoxInSolid(origin, margin, qtrue))
                        continue;
                }

                // Ticket wavefront (Phase 3b): one group per supersample.
                // Stage 1 generate (below), Stage 2 flush (trace), Stage 3
                // consume+fold (TicketConsumeTickets). The old immediate
                // LightingAtSample call block is fully replaced; the fold
                // below replicates today's exact VectorAdds.
                tw->ignoreSurface = realSurfIndex;
                #pragma omp atomic
                c_ticketGroups++;

                {
                    int before = batch.count;

                    if (!nodirect)
                    {
                        // Reserve: a group is at most sun + all local lights;
                        // flush buffered tickets first if this group would
                        // not fit, so a group is never split (flushes land
                        // on group boundaries only).
                        if (batch.count + (numLocalLights + 1) > batch.capacity)
                        {
                            int n = TicketBatch_FlushTraces(&batch, tw);
                            TicketConsumeTickets(batch.tickets, n,
                                                 accumColor, accumDir, accumEnergy, accumNormal,
                                                 deluxe ? qtrue : qfalse, si->deluxeMinAngle);
                        }

                        // sun member — LightingAtSample's guard
                        // (testOcclusion || forceSunLight) is constant-true
                        // at this call site (literals qtrue/qfalse/qtrue at
                        // the old calls); applyColorFilter = qtrue as before.
                        {
                            lightTicket_t t;
                            memset(&t, 0, sizeof(t));
                            if (TicketPrepare_SunPlane(origin, normal, &t, qtrue, tw))
                            {
                                lightTicket_t *slot = TicketBatch_Append(&batch);
                                if (slot)
                                {
                                    t.texelIdx = i * extH + j;
                                    t.ssIdx = ss;
                                    *slot = t;
                                }
                                else
                                {
                                    #pragma omp atomic
                                    c_ticketAppendWarnings++;
                                }
                            }
                        }

                        // local light members, in localLights[] order
                        {
                            int li;
                            for (li = 0; li < numLocalLights; li++)
                            {
                                lightTicket_t t;
                                memset(&t, 0, sizeof(t));
                                if (TicketPrepare_Light(localLights[li], origin, normal, &t))
                                {
                                    lightTicket_t *slot = TicketBatch_Append(&batch);
                                    if (slot)
                                    {
                                        t.texelIdx = i * extH + j;
                                        t.ssIdx = ss;
                                        *slot = t;
                                    }
                                    else
                                    {
                                        #pragma omp atomic
                                        c_ticketAppendWarnings++;
                                    }
                                }
                            }
                        }
                    }

                    if (batch.count > before)
                    {
                        // group has tickets: left buffered; consumed at the
                        // next reserve flush, the next empty group, or the
                        // texel-end flush below
                    }
                    else
                    {
                        // EMPTY group (all culls failed, or nodirect): flush
                        // buffered tickets FIRST (order!), then fold inline
                        // exactly as today — including the zero adds, which
                        // are bit-safe (accumulated color is non-negative).
                        if (batch.count > 0)
                        {
                            int n = TicketBatch_FlushTraces(&batch, tw);
                            TicketConsumeTickets(batch.tickets, n,
                                                 accumColor, accumDir, accumEnergy, accumNormal,
                                                 deluxe ? qtrue : qfalse, si->deluxeMinAngle);
                        }
                        if (!nodirect)
                        {
                            vec3_t zeroVec;
                            VectorClear(zeroVec);
                            if (deluxe)
                            {
                                VectorAdd(accumColor, zeroVec, accumColor);
                                VectorAdd(accumDir, normal, accumDir);      // subDir inits to normal
                                VectorAdd(accumEnergy, zeroVec, accumEnergy);
                                VectorAdd(accumNormal, normal, accumNormal);
                            }
                            else
                            {
                                VectorAdd(accumColor, zeroVec, accumColor);
                            }
                        }
                        else
                        {
                            if (deluxe) VectorAdd(accumNormal, normal, accumNormal);
                        }
                    }
                }

                hitCount++;
            }

            // Ticket wavefront (Phase 3b): texel-end flush — the last group's
            // tickets must be consumed (and folded into the accumulators)
            // BEFORE the averaging fold reads them below. Guarantees an empty
            // batch at the start of every texel.
            if (batch.count > 0)
            {
                int n = TicketBatch_FlushTraces(&batch, tw);
                TicketConsumeTickets(batch.tickets, n,
                                     accumColor, accumDir, accumEnergy, accumNormal,
                                     deluxe ? qtrue : qfalse, si->deluxeMinAngle);
            }

            if (hitCount > 0)
            {
                sampleHit[i][j] = qtrue;
                occluded[i][j] = qfalse;
                float invHits = 1.0f / (float)hitCount;
                for (k = 0; k < 3; k++)
                    color[i][j][k] = accumColor[k] * invHits;
                if (deluxe)
                {
                    vec3_t avgDir;
                    VectorScale(accumDir, invHits, avgDir);
                    VectorNormalize(avgDir, deluxe[i][j]);
                    VectorScale(accumEnergy, invHits, lmenergy[i][j]);
                    vec3_t avgNrm;
                    VectorScale(accumNormal, invHits, avgNrm);
                    VectorNormalize(avgNrm, normalArray[i][j]);
                }
            }
            else
            {
                sampleHit[i][j] = qfalse;
                occluded[i][j] = qtrue;
            }
        }
    }

    if (isDilated && use_upscale)
    {
        for (i = 0; i < ds->lightmapWidth; i++)
        {
            for (j = 0; j < ds->lightmapHeight; j++)
            {
                float maxIntensity = -1.0f;
                int bestX = -1, bestY = -1;

                int baseI = i * scale + currentGutter;
                int baseJ = j * scale + currentGutter;

                for (int sx = 0; sx < scale; sx++)
                {
                    for (int sy = 0; sy < scale; sy++)
                    {
                        int i2 = baseI + sx;
                        int j2 = baseJ + sy;
                        if (!sampleHit[i2][j2])
                        {
                            continue;
                        }
                        float intensity = color[i2][j2][0] + color[i2][j2][1] + color[i2][j2][2];
                        if (intensity > maxIntensity)
                        {
                            maxIntensity = intensity;
                            bestX = i2;
                            bestY = j2;
                        }
                    }
                }

                if (bestX >= 0)
                {
                    VectorCopy(color[bestX][bestY], color[i][j]);
                    if (deluxe)
                    {
                        VectorCopy(deluxe[bestX][bestY], deluxe[i][j]);
                        VectorCopy(lmenergy[bestX][bestY], lmenergy[i][j]);
                        VectorCopy(normalArray[bestX][bestY], normalArray[i][j]);
                    }
                    sampleHit[i][j] = qtrue;
                }
                else
                {
                    VectorClear(color[i][j]);
                    if (deluxe)
                    {
                        VectorClear(deluxe[i][j]);
                        VectorClear(lmenergy[i][j]);
                        VectorClear(normalArray[i][j]);
                    }
                    sampleHit[i][j] = qfalse;
                }
            }
        }
    }
    else if (isDilated && !use_upscale)
    {
        for (i = 0; i < ds->lightmapWidth; i++)
        {
            for (j = 0; j < ds->lightmapHeight; j++)
            {
                VectorCopy(color[i + currentGutter][j + currentGutter], color[i][j]);
                sampleHit[i][j] = sampleHit[i + currentGutter][j + currentGutter];
                if (deluxe)
                {
                    VectorCopy(deluxe[i + currentGutter][j + currentGutter], deluxe[i][j]);
                    VectorCopy(lmenergy[i + currentGutter][j + currentGutter], lmenergy[i][j]);
                    VectorCopy(normalArray[i + currentGutter][j + currentGutter], normalArray[i][j]);
                }
            }
        }
    }

    if (lightmapBorder)
    {
        for (i = 0; i < ds->lightmapWidth; i++)
        {
            color[i][0][0] = 255;
            color[i][0][1] = 0;
            color[i][0][2] = 0;
            color[i][ds->lightmapHeight - 1][0] = 255;
            color[i][ds->lightmapHeight - 1][1] = 0;
            color[i][ds->lightmapHeight - 1][2] = 0;
        }
        for (i = 0; i < ds->lightmapHeight; i++)
        {
            color[0][i][0] = 255;
            color[0][i][1] = 0;
            color[0][i][2] = 0;
            color[ds->lightmapWidth - 1][i][0] = 255;
            color[ds->lightmapWidth - 1][i][1] = 0;
            color[ds->lightmapWidth - 1][i][2] = 0;
        }
    }

    // Copy result back to global buffers
    for (i = 0; i < ds->lightmapWidth; i++)
    {
        for (j = 0; j < ds->lightmapHeight; j++)
        {
            k = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + j) *
                    LIGHTMAP_WIDTH +
                ds->lightmapOffset[0][0] + i;

            if (lightFloats)
            {
                if (k >= 0 && k < numLightBytes / 3)
                {
                    lightFloats[k * 3 + 0] += color[i][j][0];
                    lightFloats[k * 3 + 1] += color[i][j][1];
                    lightFloats[k * 3 + 2] += color[i][j][2];

                    if (deluxeFloats)
                    {
                        if (lightSurfaceIndex)
                        {
                            lightSurfaceIndex[k] = realSurfIndex;
                        }

                        // Write direction and energy
                        if (deluxe)
                        {
                            VectorCopy(deluxe[i][j], &deluxeFloats[k * 3]);
                        }
                        if (energyFloats && lmenergy)
                        {
                            VectorCopy(lmenergy[i][j], &energyFloats[k * 3]);
                        }
                        if (normalFloats && normalArray)
                        {
                            VectorCopy(normalArray[i][j], &normalFloats[k * 3]);
                        }
                    }
                }
            }

            if (lightAlphaMask)
            {
                if (k >= 0 && k < numLightBytes / 3)
                {
                    if (sampleHit[i][j])
                    {
                        lightAlphaMask[k] = ds->surfaceType;
                    }
                }
            }

            if (unreachableMask)
            {
                if (k >= 0 && k < numLightBytes / 3)
                {
                    if (!sampleHit[i][j])
                    {
                        #pragma omp atomic
                        unreachableMask[k >> 3] |= (1 << (k & 7));
                    }
                    else
                    {
                        #pragma omp atomic
                        unreachableMask[k >> 3] &= ~(1 << (k & 7));
                    }
                }
            }
        }
    }

    free(sampleHit);
    free(sampleHit_data);
    free(tw);
    free(occluded);
    free(occluded_data);
    free(color);
    free(color_data);
    if (deluxe) free(deluxe);
    if (deluxe_data) free(deluxe_data);
    if (lmenergy) free(lmenergy);
    if (lmenergy_data) free(lmenergy_data);
    if (normalArray) free(normalArray);
    if (normalArray_data) free(normalArray_data);
    TicketBatch_Free(&batch);
    free(localLights);
    ThreadCompletedWeighted(surfWeight);
}

#define MAX_CONTRIBUTIONS 1024



float lightgridMaxDisplayIntensity = 0.0f;

#define LIGHTGRID_AMBIENT_MAX_LUMA 0.10f
#define LIGHTGRID_MAO_GATHER_FRACTION 0.40f
#define LIGHTGRID_MAO_MAX_SATURATION 0.25f
#define LIGHTGRID_MAO_CALIBRATION 17.0f // Raw MAO values are tiny; this calibration lifts them to grid-scale energy. The profile/worldspawn knob is a relative multiplier to make it more user intuitive

void TraceGrid(int num)
{
    int x, y, z;
    vec3_t origin;
    light_t *light;
    vec3_t color;
    int mod;
    vec3_t directedColor;
    vec3_t summedDir;
    contribution_t contributions[MAX_CONTRIBUTIONS];
    int numCon;
    int i;
    traceWork_t *tw;

    tw = Q_Alloc(sizeof(traceWork_t));
    if (!tw)
        Error("Failed to allocate traceWork_t");
    memset(tw, 0, sizeof(traceWork_t));
    tw->ignoreSurface = -1;
    tw->isLightgrid = qtrue;

    mod = num;
    z = mod / (gridBounds[0] * gridBounds[1]);
    mod -= z * (gridBounds[0] * gridBounds[1]);
    y = mod / gridBounds[0];
    mod -= y * gridBounds[0];
    x = mod;

    origin[0] = gridMins[0] + x * gridSize[0];
    origin[1] = gridMins[1] + y * gridSize[1];
    origin[2] = gridMins[2] + z * gridSize[2];

    if (gridInvalidMask[num] & GRID_FLAG_INVALID)
    {
        // Fully buried: the classification pass found no open point within
        // the nudge radius; the cell carries no light and stays zeroed.
        // CompressGrid writes the engine sentinel for it when writing FBSP
        // version 2 (see bsp_extended_capabilities.md).
        if (gridData32)
            memset(&gridData32[num], 0, sizeof(gridData32[num]));
        free(tw);
        return;
    }

    if (gridInvalidMask[num] & GRID_FLAG_SOLID)
    {
        // Lattice point sits inside solid geometry: sample at the open point
        // resolved once by the classification pass, so direct light, MAO
        // ambient and radiosity all describe the same location.
        VectorCopy(&gridResolvedOrigins[num * 3], origin);
    }

    float maxAddSize = 0.0f;
    float totalAddSize = 0.0f;
    vec3_t dominantDir = {0.0f, 0.0f, 0.0f};
    vec3_t averageDir = {0.0f, 0.0f, 0.0f};

    VectorSet(summedDir, 0.0f, 0.0f, -0.000001f);
    numCon = 0;
    if (!nodirect)
    {
        for (light = lights; light; light = light->next)
        {
            if (LightContributionToPoint(light, origin, NULL, &contributions[numCon], tw))
            {
                vec3_t tempColor;
                VectorScale(contributions[numCon].irradiance, contributions[numCon].angle, tempColor);
                if (contributions[numCon].gridAmbientScale > 0.0001f ||
                    contributions[numCon].gridDirectScale > 0.0001f)
                {
                    float addSize = VectorLength(tempColor);
                    totalAddSize += addSize;
                    VectorMA(averageDir, addSize, contributions[numCon].dir, averageDir);
                    if (addSize > maxAddSize)
                    {
                        maxAddSize = addSize;
                        VectorCopy(contributions[numCon].dir, dominantDir);
                    }
                }
                numCon++;
                if (numCon >= MAX_CONTRIBUTIONS)
                    break;
            }
        }

        if (SunToPoint(origin, tw, &contributions[numCon], qtrue))
        {
            contributions[numCon].angle = 1.0f; // SunToPoint doesn't set angle, default to 1
            vec3_t tempColor;
            VectorScale(contributions[numCon].irradiance, contributions[numCon].angle, tempColor);
            if (contributions[numCon].gridAmbientScale > 0.0001f ||
                contributions[numCon].gridDirectScale > 0.0001f)
            {
                float addSize = VectorLength(tempColor);
                totalAddSize += addSize;
                VectorMA(averageDir, addSize, contributions[numCon].dir, averageDir);
                if (addSize > maxAddSize)
                {
                    maxAddSize = addSize;
                    VectorCopy(contributions[numCon].dir, dominantDir);
                }
            }
            numCon++;
        }
    }

    if (totalAddSize > 0.00001f)
    {
        float confidence = maxAddSize / totalAddSize;
        VectorNormalize(averageDir, averageDir);
        
        for (i = 0; i < 3; i++)
        {
            summedDir[i] = averageDir[i] * (1.0f - confidence) + dominantDir[i] * confidence;
        }
    }

    VectorNormalize(summedDir, summedDir);
    VectorSet(color, 0.000001f, 0.000001f, 0.000001f);
    VectorSet(directedColor, 0.000001f, 0.000001f, 0.000001f);

    for (i = 0; i < numCon; i++)
    {
        float d;
        vec3_t tempColor;
        VectorScale(contributions[i].irradiance, contributions[i].angle, tempColor);
        
        d = CalculateShadingModel(DotProduct(contributions[i].dir, summedDir));

        vec3_t scaledDirect;
        VectorScale(tempColor, contributions[i].gridDirectScale, scaledDirect);

        if (contributions[i].directAbsMax > 0.0f)
        {
            vec3_t dContrib;
            VectorScale(scaledDirect, d, dContrib);
            float dLuma = dContrib[0] * 0.299f + dContrib[1] * 0.587f + dContrib[2] * 0.114f;
            if (dLuma > contributions[i].directAbsMax)
            {
                VectorScale(scaledDirect, contributions[i].directAbsMax / dLuma, scaledDirect);
            }
        }

        VectorMA(directedColor, d, scaledDirect, directedColor);

        if (contributions[i].gridAmbientScale > 0.0001f)
        {
            float distanceScale = 1.0f;
            if (contributions[i].light != NULL && contributions[i].light->reach > 0.001f)
            {
                vec3_t delta;
                VectorSubtract(contributions[i].light->origin, origin, delta);
                float dist = VectorLength(delta);

                // Distance ratio clamped to 1.0 (defensive guard against edge precision)
                float t = dist / contributions[i].light->reach;
                if (t > 1.0f)
                {
                    t = 1.0f;
                }

                // Goldilocks curve (sigmoid knee at 0.5)
                float knee = 0.5f;
                float r = t / knee;
                distanceScale = 1.0f / sqrtf(1.0f + r * r);
            }

            vec3_t ambContrib;
            float finalAmbientFraction = game->lightgridAmbientTransferFraction * contributions[i].gridAmbientScale * distanceScale;
            VectorScale(tempColor, finalAmbientFraction, ambContrib);

            float clampLuma;
            if (contributions[i].ambientAbsMax > 0.0f)
            {
                clampLuma = contributions[i].ambientAbsMax;
            }
            else
            {
                clampLuma = lightgridMaxDisplayIntensity * LIGHTGRID_AMBIENT_MAX_LUMA * contributions[i].ambientClampScale;
            }
            float luma = ambContrib[0] * 0.299f + ambContrib[1] * 0.587f + ambContrib[2] * 0.114f;
            
            if (luma > clampLuma)
            {
                float scale = clampLuma / luma;
                VectorScale(ambContrib, scale, ambContrib);
            }
            
            VectorAdd(color, ambContrib, color);
        }
    }

    if (gridData32)
    {
        VectorAdd(color, gridData32[num].ambient[0], gridData32[num].ambient[0]);
        VectorAdd(directedColor, gridData32[num].directed[0], gridData32[num].directed[0]);
        VectorNormalize(summedDir, summedDir);
        
        gridData32[num].styles[0] = 0;
        gridData32[num].styles[1] = 0xff;
        gridData32[num].styles[2] = 0xff;
        gridData32[num].styles[3] = 0xff;

        if (maoAmbient)
        {
            float dirRatio = 1.5f;
            float ambRatio = 1.5f;
            vec3_t moveDir;
            vec3_t moveAmb;
            
            VectorCopy(&maoAmbient[num * 3], moveAmb);
            VectorClear(moveDir);

            // Optimization: only attempt gather if this cell itself is lit (not solid/black).
            float selfLum = moveAmb[0] * 0.299f + moveAmb[1] * 0.587f + moveAmb[2] * 0.114f;
            
            extern float ambient_gatheradius;
            if (ambient_gatheradius > 0.001f && selfLum > 0.0001f)
            {
                vec3_t envAmb;
                if (GatherGridAmbientBleed(origin, envAmb, tw))
                {
                    // Blend local and gathered using our macro
                    float gatherMix = LIGHTGRID_MAO_GATHER_FRACTION;
                    float localMix = 1.0f - gatherMix;
                    
                    moveAmb[0] = moveAmb[0] * localMix + envAmb[0] * gatherMix;
                    moveAmb[1] = moveAmb[1] * localMix + envAmb[1] * gatherMix;
                    moveAmb[2] = moveAmb[2] * localMix + envAmb[2] * gatherMix;
                }
            }

            // Saturation cap (chroma/luma ratio clamp): saturation is a ratio, so the
            // ambient/directional split and ambientGridScale (all uniform
            // multiplies of this one color) can never change it. A very saturated
            // worldspawn ambient enters with a huge relative chroma (pure blue ~14),
            // so a fixed multiplier could not bound it. Clamping the ratio caps the
            // color regardless of input while preserving luma, leaving the calibrated
            // brightness untouched. Mild ambient colors pass through unmodified.
            {
                float lum = moveAmb[0] * 0.2126f + moveAmb[1] * 0.7152f + moveAmb[2] * 0.0722f;
                if (lum > 0.0001f)
                {
                    float mx = moveAmb[0], mn = moveAmb[0];
                    if (moveAmb[1] > mx) mx = moveAmb[1];
                    if (moveAmb[2] > mx) mx = moveAmb[2];
                    if (moveAmb[1] < mn) mn = moveAmb[1];
                    if (moveAmb[2] < mn) mn = moveAmb[2];
                    float ratio = (mx - mn) / lum;
                    if (ratio > LIGHTGRID_MAO_MAX_SATURATION)
                    {
                        float t = LIGHTGRID_MAO_MAX_SATURATION / ratio;
                        for (int i = 0; i < 3; i++)
                            moveAmb[i] = lum + t * (moveAmb[i] - lum);
                    }
                }
            }

            VectorScale(moveAmb, LIGHTGRID_MAO_CALIBRATION * game->ambientGridScale, moveAmb);

            VectorCopy(moveAmb, moveDir);
            VectorScale(moveAmb, ambRatio, moveAmb);
            VectorScale(moveDir, dirRatio, moveDir);

            // Absolute per-component luma cap (applied after scaling): prevents
            // wide-open, well-lit cells from contributing disproportionately to
            // the grid ambient/directed terms. Open, fully sky-lit cells
            // otherwise scale to MAO_CALIBRATION * ratio * (sky+ground) and
            // flood the grid. 0 disables the cap.
            if (game->lightgridMaoMaxLuma > 0.0f)
            {
                float ambLum = moveAmb[0] * 0.299f + moveAmb[1] * 0.587f + moveAmb[2] * 0.114f;
                if (ambLum > game->lightgridMaoMaxLuma)
                    VectorScale(moveAmb, game->lightgridMaoMaxLuma / ambLum, moveAmb);

                float dirLum = moveDir[0] * 0.299f + moveDir[1] * 0.587f + moveDir[2] * 0.114f;
                if (dirLum > game->lightgridMaoMaxLuma)
                    VectorScale(moveDir, game->lightgridMaoMaxLuma / dirLum, moveDir);
            }

            gridData32[num].ambient[0][0] += moveAmb[0];
            gridData32[num].ambient[0][1] += moveAmb[1];
            gridData32[num].ambient[0][2] += moveAmb[2];
            
            gridData32[num].directed[0][0] += moveDir[0];
            gridData32[num].directed[0][1] += moveDir[1];
            gridData32[num].directed[0][2] += moveDir[2];

            if (maoDir)
            {
                float moveMagnitude = VectorLength(moveDir);
                float totalWeight = totalAddSize + moveMagnitude;
                
                if (totalWeight > 0.0001f)
                {
                    vec3_t blendedDir;
                    blendedDir[0] = (summedDir[0] * totalAddSize + maoDir[num * 3 + 0] * moveMagnitude) / totalWeight;
                    blendedDir[1] = (summedDir[1] * totalAddSize + maoDir[num * 3 + 1] * moveMagnitude) / totalWeight;
                    blendedDir[2] = (summedDir[2] * totalAddSize + maoDir[num * 3 + 2] * moveMagnitude) / totalWeight;
                    
                    if (VectorNormalize(blendedDir, summedDir) == 0.0f)
                    {
                        // Fallback if vectors perfectly cancel each other out
                        VectorCopy(&maoDir[num * 3], summedDir);
                    }
                }
            }
        }
        
        VectorCopy(summedDir, gridData32[num].dir);
    }
    free(tw);
}

void LightWorld(void)
{
    double start, end;
    int i;

    // Reset ticket instrumentation for this run (Phase 0: counters stay zero
    // until the ticket-wavefront conversion fills them in a later phase).
    c_ticketsGenerated = c_ticketsSubmitted = c_ticketsBlocked = 0;
    c_ticketsContributed = c_ticketsRejected = c_ticketGroups = 0;
    c_ticketFlushes = c_groupOverflowGrow = 0;

    surfaceWorkOrder = Q_Alloc(numDrawSurfaces * sizeof(int));
    for (i = 0; i < numDrawSurfaces; i++)
    {
        surfaceWorkOrder[i] = i;
    }
    qsort(surfaceWorkOrder, numDrawSurfaces, sizeof(int), CompareSurfaces);

    if (!nogridlighting)
    {
        if (!directonly)
        {
            RunMAOPass();
        }

        _printf("--- TraceGrid ---\n");
        
            lightgridMaxDisplayIntensity = 255.0f * game->hdr8BitScale;

        start = I_FloatTime();
        RunThreadsOnIndividual(numGridPoints, qtrue, TraceGrid);
        end = I_FloatTime();
        _printf("%i x %i x %i = %i grid\n", gridBounds[0], gridBounds[1],
                gridBounds[2], numGridPoints);
        _printf("%5.0f seconds elapsed in TraceGrid\n", end - start);

    }

    _printf("--- TraceLights ---\n");
    start = I_FloatTime();
    RunThreadsOnWeighted(numDrawSurfaces, numTotalLuxels, qtrue, TraceLights);
    end = I_FloatTime();
    _printf("\n");
    _printf("%5.0f seconds elapsed in TraceLights\n", end - start);

    // Phase 0 instrumentation: ticket counters summary (print-only), with
    // the Phase 3b invariant checks (exact global atomic totals, so the
    // comparison is valid after all worker threads have completed).
    _printf("TraceLights tickets: generated=%d submitted=%d groups=%d flushes=%d blocked=%d contributed=%d rejected=%d\n",
            c_ticketsGenerated, c_ticketsSubmitted, c_ticketGroups, c_ticketFlushes,
            c_ticketsBlocked, c_ticketsContributed, c_ticketsRejected);
    if (c_ticketsSubmitted != c_ticketsGenerated ||
        c_ticketsBlocked + c_ticketsContributed + c_ticketsRejected != c_ticketsSubmitted)
    {
        _printf("WARNING: TraceLights ticket invariants violated "
                "(submitted=%d generated=%d blocked=%d contributed=%d rejected=%d)\n",
                c_ticketsSubmitted, c_ticketsGenerated,
                c_ticketsBlocked, c_ticketsContributed, c_ticketsRejected);
    }
    if (c_ticketStrandWarnings || c_ticketAppendWarnings || c_groupOverflowGrow)
    {
        _printf("TraceLights ticket guards: strand=%d appendNULL=%d overflowGrow=%d\n",
                c_ticketStrandWarnings, c_ticketAppendWarnings, c_groupOverflowGrow);
    }
}

void DilateDeluxeDirections(void)
{
    if (!deluxeFloats || !lightFloats)
        return;

    _printf("--- DilateDeluxeDirections ---\n");

    // Perform 2 passes of dilation to ensure directions spread deep into shadows
    for (int pass = 0; pass < 2; pass++)
    {
        #pragma omp parallel for schedule(dynamic)
        for (int s = 0; s < numDrawSurfaces; s++)
        {
            dsurface_t *ds = &drawSurfaces[s];
            if (ds->lightmapNum[0] < 0)
                continue;

            for (int i = 0; i < ds->lightmapWidth; i++)
            {
                for (int j = 0; j < ds->lightmapHeight; j++)
                {
                    int k = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + j) * LIGHTMAP_WIDTH +
                            ds->lightmapOffset[0][0] + i;
                    
                float intensity = 0.0f;
                if (energyFloats) {
                    intensity = energyFloats[k * 3 + 0] + energyFloats[k * 3 + 1] + energyFloats[k * 3 + 2];
                } else {
                    intensity = lightFloats[k * 3 + 0] + lightFloats[k * 3 + 1] + lightFloats[k * 3 + 2];
                }

                // Treat as black/target for dilation if intensity is practically zero
                if (intensity > 0.0001f)
                    continue;

                float bestIntensity = 0.0f;
                int bestK = -1;

                for (int di = -1; di <= 1; di++)
                {
                    for (int dj = -1; dj <= 1; dj++)
                    {
                        if (di == 0 && dj == 0) continue;
                        int ni = i + di;
                        int nj = j + dj;
                        if (ni < 0 || ni >= ds->lightmapWidth || nj < 0 || nj >= ds->lightmapHeight)
                            continue;

                        int nk = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + nj) * LIGHTMAP_WIDTH +
                                 ds->lightmapOffset[0][0] + ni;
                        
                        float nIntensity = 0.0f;
                        if (energyFloats) {
                            nIntensity = energyFloats[nk * 3 + 0] + energyFloats[nk * 3 + 1] + energyFloats[nk * 3 + 2];
                        } else {
                            nIntensity = lightFloats[nk * 3 + 0] + lightFloats[nk * 3 + 1] + lightFloats[nk * 3 + 2];
                        }

                        if (nIntensity > bestIntensity)
                        {
                            bestIntensity = nIntensity;
                            bestK = nk;
                        }
                    }
                }

                // Borrow if we found a neighbor with any energy
                if (bestK >= 0 && bestIntensity > 0.0001f)
                {
                    VectorCopy(&deluxeFloats[bestK * 3], &deluxeFloats[k * 3]);
                    if (normalFloats)
                        VectorCopy(&normalFloats[bestK * 3], &normalFloats[k * 3]);
                }
                }
            }
        }
    }
}
