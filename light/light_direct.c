#include "light.h"
#include "../common/imagelib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

int *surfaceWorkOrder;
extern int *texelSurfaceDebug;

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

    // Identify if this surface is planar
    int surfIndex = (int)(ds - drawSurfaces);
    qboolean isPlanar = localSurfaces[surfIndex].surfaceIsPlanar;

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
                if (isPlanar)
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
                else
                {
                    // Calculate raw barycentric coordinates (extrapolation)
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
    }

    // If we found no exact match but found a valid extrapolation, use it
    // For planar-derived trisoups (which xatlas splits with padding), limit extrapolation to 1 texel
    // to prevent pushing the origin deep into adjacent concave walls and causing false shadows.
    // Standard trisoups (e.g. terrain) use the full 2 texel (4.0f) extrapolation limit.
    float maxExtrapDistSq = isPlanar ? (float)GUTTER * GUTTER * 1.0f : (float)GUTTER * GUTTER * 4.0f;

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
================
*/
qboolean SunToPoint(const vec3_t origin, traceWork_t *tw, contribution_t *out,
                    qboolean applyColorFilter)
{
    trace_t trace;
    vec3_t end;

    if (!numSkyBrushes || !hasSun)
    {
        return qfalse;
    }

    VectorMA(origin, MAX_WORLD_COORD * 2, sunDirection, end);

    TraceLine(origin, end, &trace, qtrue, tw);

    // If the ray hit a solid occluder in Embree, it cannot be the sky!
    if (trace.passSolid)
    {
        return qfalse;
    }

    // We reached the void/sky without being blocked by a solid wall.
    // So we add sunlight.
    if (!applyColorFilter)
    {
        trace.filter[0] = trace.filter[1] = trace.filter[2] = 1.0f;
    }

    VectorCopy(sunDirection, out->dir);
    out->irradiance[0] = trace.filter[0] * sunLight[0];
    out->irradiance[1] = trace.filter[1] * sunLight[1];
    out->irradiance[2] = trace.filter[2] * sunLight[2];
    out->isGlow = qfalse;

    return qtrue;
}

/*
================
SunToPlane
Returns an amount of light to add at the texel (surface)
================
*/
qboolean SunToPlane(const vec3_t origin, const vec3_t normal,
                    contribution_t *out, qboolean applyColorFilter,
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

    if (SunToPoint(origin, tw, out, applyColorFilter))
    {
        out->angle = angle;
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
========================
*/
qboolean LightContributionToPoint(const light_t *light, const vec3_t origin,
                                  const vec3_t normal, contribution_t *out,
                                  traceWork_t *tw)
{
    trace_t trace;
    float add = 0;
    vec3_t dir;
    float dist;
    float angle = 1.0f;

    // area light (Exact Point-To-Polygon Form Factor)
    if (light->type == emit_area)
    {
        float factor;
        float d;
        vec3_t n;

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
        VectorCopy(n, out->dir);

        // test occlusion
        TraceLine(origin, light->origin, &trace, qfalse, tw);
        if (trace.passSolid)
        {
            return qfalse;
        }

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

        float formFactorBase;
        float outAngle;
        qboolean outIsGlow = qfalse;

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

        if (light->attnSoftnessRange > 0.0f)
        {
            float fadeStartDist = light->reach - light->attnSoftnessRange;
            if (dist > fadeStartDist)
            {
                float fadeScale = (light->reach - dist) / light->attnSoftnessRange;
                formFactorBase *= fadeScale;
            }
        }

        out->irradiance[0] = light->emitColor[0] * formFactorBase * trace.filter[0];
        out->irradiance[1] = light->emitColor[1] * formFactorBase * trace.filter[1];
        out->irradiance[2] = light->emitColor[2] * formFactorBase * trace.filter[2];
        out->angle = outAngle;
        out->isGlow = outIsGlow;
        return qtrue;
    }

    // point or spotlight logic
    if (light->type == emit_point || light->type == emit_spotlight)
    {
        VectorSubtract(light->origin, origin, dir);
        dist = VectorNormalize(dir, out->dir);
        if (dist > light->reach)
        {
            return qfalse;
        }
        if (dist < 16)
        {
            dist = 16;
        }

        float surfaceAngle = 1.0f;
        float coneScale = 1.0f;

        // surface falloff
        if (normal)
        {
            surfaceAngle = CalculateShadingModel(DotProduct(normal, out->dir));
            if (surfaceAngle <= 0)
            {
                return qfalse;
            }
        }

        add = CalculateAttenuation(light, dist, light->attenuationModel, DEFAULT_ATTN_OFFSET);
        
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

            distByNormal = -DotProduct(out->dir, light->normal) * dist;
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

        out->angle = surfaceAngle;
        out->isGlow = qfalse;
    }
    else
    {
        return qfalse;
    }

    if (add <= MIN_LIGHT_ADD)
    {
        return qfalse;
    }

    // occlusion check
    TraceLine(origin, light->origin, &trace, qfalse, tw);
    if (trace.passSolid)
    {
        return qfalse;
    }

    out->irradiance[0] = add * light->color[0] * trace.filter[0];
    out->irradiance[1] = add * light->color[1] * trace.filter[1];
    out->irradiance[2] = add * light->color[2] * trace.filter[2];

    return qtrue;
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
                if (localSurfaces[i].upscale > 1) {
                    margin *= 2.0f;
                }
                
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

                vec3_t subColor, subDir, subEnergy;
                tw->ignoreSurface = realSurfIndex;
                if (!nodirect)
                {
                    if (deluxe)
                    {
                        LightingAtSample(origin, normal, subColor, subDir, subEnergy,
                                         qtrue, qfalse, qtrue, localLights, numLocalLights, tw, si->deluxeMinAngle);
                        VectorAdd(accumColor, subColor, accumColor);
                        VectorAdd(accumDir, subDir, accumDir);
                        VectorAdd(accumEnergy, subEnergy, accumEnergy);
                        VectorAdd(accumNormal, normal, accumNormal);
                    }
                    else
                    {
                        LightingAtSample(origin, normal, subColor, NULL, NULL,
                                         qtrue, qfalse, qtrue, localLights, numLocalLights, tw, si->deluxeMinAngle);
                        VectorAdd(accumColor, subColor, accumColor);
                    }
                }
                else
                {
                    if (deluxe) VectorAdd(accumNormal, normal, accumNormal);
                }
                
                hitCount++;
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
    free(localLights);
    ThreadCompletedWeighted(surfWeight);
}

#define MAX_CONTRIBUTIONS 1024



float lightgridMaxDisplayIntensity = 0.0f;

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

    mod = num;
    z = mod / (gridBounds[0] * gridBounds[1]);
    mod -= z * (gridBounds[0] * gridBounds[1]);
    y = mod / gridBounds[0];
    mod -= y * gridBounds[0];
    x = mod;

    origin[0] = gridMins[0] + x * gridSize[0];
    origin[1] = gridMins[1] + y * gridSize[1];
    origin[2] = gridMins[2] + z * gridSize[2];

    if (PointInSolid(origin))
    {
        vec3_t baseOrigin;
        int step;
        VectorCopy(origin, baseOrigin);
        for (step = 9; step <= 18; step += 9)
        {
            for (i = 0; i < 8; i++)
            {
                VectorCopy(baseOrigin, origin);
                if (i & 1)
                    origin[0] += step;
                else
                    origin[0] -= step;
                if (i & 2)
                    origin[1] += step;
                else
                    origin[1] -= step;
                if (i & 4)
                    origin[2] += step;
                else
                    origin[2] -= step;
                if (!PointInSolid(origin))
                    break;
            }
            if (i != 8)
                break;
        }
        if (step > 18)
        {
            if (gridData32)
                memset(&gridData32[num], 0, sizeof(gridData32[num]));
            free(tw);
            return;
        }
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
                float addSize = VectorLength(tempColor);
                totalAddSize += addSize;
                VectorMA(averageDir, addSize, contributions[numCon].dir, averageDir);
                if (addSize > maxAddSize)
                {
                    maxAddSize = addSize;
                    VectorCopy(contributions[numCon].dir, dominantDir);
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
            float addSize = VectorLength(tempColor);
            totalAddSize += addSize;
            VectorMA(averageDir, addSize, contributions[numCon].dir, averageDir);
            if (addSize > maxAddSize)
            {
                maxAddSize = addSize;
                VectorCopy(contributions[numCon].dir, dominantDir);
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
        
        if (lightgridDirectBias != 1.0f)
        {
            float length = VectorLength(tempColor);
            if (length > 0.001f && length < lightgridMaxDisplayIntensity)
            {
                float new_length = pow(length / lightgridMaxDisplayIntensity, 1.0f / lightgridDirectBias) * lightgridMaxDisplayIntensity;
                float scale = new_length / length;
                VectorScale(tempColor, scale, tempColor);
            }
        }
        
        d = CalculateShadingModel(DotProduct(contributions[i].dir, summedDir));
        VectorMA(directedColor, d, tempColor, directedColor);
        
        vec3_t ambContrib;
        VectorScale(tempColor, 0.25f, ambContrib);
        
        float clampLuma = lightgridMaxDisplayIntensity * 0.10f;
        float luma = ambContrib[0] * 0.299f + ambContrib[1] * 0.587f + ambContrib[2] * 0.114f;
        
        if (luma > clampLuma)
        {
            float scale = clampLuma / luma;
            VectorScale(ambContrib, scale, ambContrib);
        }
        
        VectorAdd(color, ambContrib, color);
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
            float dirRatio = 0.45f;
            float ambRatio = 0.75f;
            vec3_t moveDir;
            vec3_t moveAmb;
            
            VectorScale(&maoAmbient[num * 3], dirRatio, moveDir);
            VectorScale(&maoAmbient[num * 3], ambRatio, moveAmb);
            
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

static void GatherGridNeighbors(int x, int y, int z, vec3_t origin, vec3_t outAmb, vec3_t outDir, vec3_t outDirVec)
{
    int nx, ny, nz;
    int ni;
    int count = 0;
    trace_t trace;
    traceWork_t tw;
    
    memset(&tw, 0, sizeof(tw));
    tw.ignoreSurface = -1;
    tw.forceFrontOnly = qfalse;

    outAmb[0] = outAmb[1] = outAmb[2] = 0;
    outDir[0] = outDir[1] = outDir[2] = 0;
    VectorClear(outDirVec);

    for (nz = z - 1; nz <= z + 1; nz++)
    {
        if (nz < 0 || nz >= gridBounds[2]) continue;
        for (ny = y - 1; ny <= y + 1; ny++)
        {
            if (ny < 0 || ny >= gridBounds[1]) continue;
            for (nx = x - 1; nx <= x + 1; nx++)
            {
                if (nx < 0 || nx >= gridBounds[0]) continue;
                
                ni = (nz * gridBounds[1] + ny) * gridBounds[0] + nx;
                
                if (nx != x || ny != y || nz != z)
                {
                    vec3_t n_origin;
                    n_origin[0] = gridMins[0] + nx * gridSize[0];
                    n_origin[1] = gridMins[1] + ny * gridSize[1];
                    n_origin[2] = gridMins[2] + nz * gridSize[2];
                    
                    TraceLine(origin, n_origin, &trace, qtrue, &tw);
                    if (trace.hitFraction < 0.999f)
                        continue; // blocked by solid
                }

                outAmb[0] += gridData32[ni].ambient[0][0];
                outAmb[1] += gridData32[ni].ambient[0][1];
                outAmb[2] += gridData32[ni].ambient[0][2];
                outDir[0] += gridData32[ni].directed[0][0];
                outDir[1] += gridData32[ni].directed[0][1];
                outDir[2] += gridData32[ni].directed[0][2];
                outDirVec[0] += gridData32[ni].dir[0];
                outDirVec[1] += gridData32[ni].dir[1];
                outDirVec[2] += gridData32[ni].dir[2];
                count++;
            }
        }
    }

    if (count > 0)
    {
        float inv = 1.0f / count;
        outAmb[0] *= inv; outAmb[1] *= inv; outAmb[2] *= inv;
        outDir[0] *= inv; outDir[1] *= inv; outDir[2] *= inv;
        VectorNormalize(outDirVec, outDirVec);
    }
}



void LightWorld(void)
{
    double start, end;
    int i;

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
        
        {
            const char *existingIntensity = ValueForKey(&entities[0], "_lightingIntensity");
            float customIntensity = existingIntensity[0] ? atof(existingIntensity) : 0.0f;
            lightgridMaxDisplayIntensity = 255.0f * (customIntensity > 1.0f ? customIntensity : game->hdr8BitScale);
        }

        start = I_FloatTime();
        RunThreadsOnIndividual(numGridPoints, qtrue, TraceGrid);
        end = I_FloatTime();
        _printf("%i x %i x %i = %i grid\n", gridBounds[0], gridBounds[1],
                gridBounds[2], numGridPoints);
        _printf("%5.0f seconds elapsed in TraceGrid\n", end - start);

        if (lightgridAmbientBias != 1.0f)
        {
            float maxIntensity = 255.0f * game->hdr8BitScale;
            _printf("Applying lightgrid ambient bias (gamma %f, maxIntensity %f)...\n", lightgridAmbientBias, maxIntensity);
            
            for (i = 0; i < numGridPoints; i++)
            {
                float length = VectorLength(gridData32[i].ambient[0]);
                if (length > 0.001f && length < maxIntensity)
                {
                    float new_length = pow(length / maxIntensity, 1.0f / lightgridAmbientBias) * maxIntensity;
                    float scale = new_length / length;
                    VectorScale(gridData32[i].ambient[0], scale, gridData32[i].ambient[0]);
                }
            }
        }

        if (lightgridMinLight > 0.0f)
        {
            float targetMinLight = (lightgridMinLight / 100.0f) * lightgridMaxDisplayIntensity;
            _printf("Applying lightgrid min light (target %f)...\n", targetMinLight);

            vec3_t local_ambient_hue;
            VectorCopy(ambientColor, local_ambient_hue);
            float localHueLum = local_ambient_hue[0] * 0.299f + local_ambient_hue[1] * 0.587f + local_ambient_hue[2] * 0.114f;
            if (localHueLum > 0.001f) {
                VectorScale(local_ambient_hue, 1.0f / localHueLum, local_ambient_hue);
            } else {
                VectorSet(local_ambient_hue, 1.0f, 1.0f, 1.0f);
            }
            
            for (i = 0; i < numGridPoints; i++) 
            {
                float lumAmb = gridData32[i].ambient[0][0] * 0.299f + gridData32[i].ambient[0][1] * 0.587f + gridData32[i].ambient[0][2] * 0.114f;
                float lumDir = gridData32[i].directed[0][0] * 0.299f + gridData32[i].directed[0][1] * 0.587f + gridData32[i].directed[0][2] * 0.114f;
                float currentIntensity = lumAmb + lumDir;
                
                if (currentIntensity < targetMinLight) 
                {
                    float missing = targetMinLight - currentIntensity;
                    
                    int z = i / (gridBounds[0] * gridBounds[1]);
                    int mod = i - z * (gridBounds[0] * gridBounds[1]);
                    int y = mod / gridBounds[0];
                    int x = mod - y * gridBounds[0];

                    vec3_t origin;
                    origin[0] = gridMins[0] + x * gridSize[0];
                    origin[1] = gridMins[1] + y * gridSize[1];
                    origin[2] = gridMins[2] + z * gridSize[2];

                    vec3_t avgAmb, avgDir, avgDirVec;
                    GatherGridNeighbors(x, y, z, origin, avgAmb, avgDir, avgDirVec);

                    float n_lumAmb = avgAmb[0] * 0.299f + avgAmb[1] * 0.587f + avgAmb[2] * 0.114f;
                    float n_lumDir = avgDir[0] * 0.299f + avgDir[1] * 0.587f + avgDir[2] * 0.114f;
                    float L_total = n_lumDir + n_lumAmb;

                    float R_dir = (L_total > 0.001f) ? (n_lumDir / L_total) : 0.0f;
                    float R_amb = (L_total > 0.001f) ? (n_lumAmb / L_total) : 1.0f;

                    vec3_t neighbor_dir_hue;
                    VectorCopy(avgDir, neighbor_dir_hue);
                    if (n_lumDir > 0.001f) {
                        VectorScale(neighbor_dir_hue, 1.0f / n_lumDir, neighbor_dir_hue);
                    } else {
                        VectorCopy(local_ambient_hue, neighbor_dir_hue);
                    }

                    float final_ambient_amount = missing * R_amb;
                    float real_directed_amount = missing * R_dir;
                    float mao_directed_amount  = 0.0f;

                    gridData32[i].ambient[0][0] += local_ambient_hue[0] * final_ambient_amount;
                    gridData32[i].ambient[0][1] += local_ambient_hue[1] * final_ambient_amount;
                    gridData32[i].ambient[0][2] += local_ambient_hue[2] * final_ambient_amount;

                    vec3_t combined_dir_color;
                    combined_dir_color[0] = (neighbor_dir_hue[0] * real_directed_amount) + (local_ambient_hue[0] * mao_directed_amount);
                    combined_dir_color[1] = (neighbor_dir_hue[1] * real_directed_amount) + (local_ambient_hue[1] * mao_directed_amount);
                    combined_dir_color[2] = (neighbor_dir_hue[2] * real_directed_amount) + (local_ambient_hue[2] * mao_directed_amount);

                    gridData32[i].directed[0][0] += combined_dir_color[0];
                    gridData32[i].directed[0][1] += combined_dir_color[1];
                    gridData32[i].directed[0][2] += combined_dir_color[2];

                    vec3_t m_dir;
                    if (maoDir) {
                        m_dir[0] = maoDir[i * 3 + 0];
                        m_dir[1] = maoDir[i * 3 + 1];
                        m_dir[2] = maoDir[i * 3 + 2];
                        if (VectorLength(m_dir) < 0.001f) VectorSet(m_dir, 0.0f, 0.0f, -1.0f);
                    } else {
                        VectorSet(m_dir, 0.0f, 0.0f, -1.0f);
                    }

                    vec3_t combined_dir;
                    VectorScale(avgDirVec, real_directed_amount, combined_dir);
                    VectorMA(combined_dir, mao_directed_amount, m_dir, combined_dir);
                    if (VectorNormalize(combined_dir, combined_dir) == 0.0f) {
                        VectorCopy(m_dir, combined_dir);
                    }

                    VectorCopy(combined_dir, gridData32[i].dir);
                }
            }
        }

        if (lightgridMaxAmbient > 0.0f)
        {
            float maxAmbientAllowed = (lightgridMaxAmbient / 100.0f) * lightgridMaxDisplayIntensity;
            _printf("Applying lightgrid max ambient (max allowed %f)...\n", maxAmbientAllowed);
            
            for (i = 0; i < numGridPoints; i++) 
            {
                float lumAmb = gridData32[i].ambient[0][0] * 0.299f + gridData32[i].ambient[0][1] * 0.587f + gridData32[i].ambient[0][2] * 0.114f;
                
                if (lumAmb > maxAmbientAllowed)
                {
                    float scale = maxAmbientAllowed / lumAmb;
                    VectorScale(gridData32[i].ambient[0], scale, gridData32[i].ambient[0]);
                }
            }
        }
    }

    _printf("--- TraceLights ---\n");
    start = I_FloatTime();
    RunThreadsOnWeighted(numDrawSurfaces, numTotalLuxels, qtrue, TraceLights);
    end = I_FloatTime();
    _printf("\n");
    _printf("%5.0f seconds elapsed in TraceLights\n", end - start);
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
