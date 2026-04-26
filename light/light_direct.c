#include "light.h"
#include "../common/imagelib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

// 8-point Rotated Grid (tilted ~26.6 degrees)
static const float ssPattern8[][2] = {
  { 0.000f,  0.000f},   // center
  {-0.354f, -0.854f},
  { 0.354f, -0.354f},
  { 0.854f,  0.146f},
  { 0.354f,  0.646f},
  {-0.146f,  0.354f},
  {-0.646f, -0.146f},
  {-0.854f,  0.354f},
};
#define SS_PATTERN8_COUNT 8

// 16-point Halton(2,3) quasi-random sequence
static const float ssPattern16[][2] = {
  { 0.000f,  0.000f},   // center
  { 0.000f, -0.333f},
  {-0.500f,  0.333f},
  { 0.500f, -0.778f},
  {-0.750f, -0.111f},
  { 0.250f,  0.556f},
  {-0.250f, -0.556f},
  { 0.750f,  0.111f},
  {-0.875f,  0.778f},
  { 0.125f, -0.926f},
  {-0.375f, -0.259f},
  { 0.625f,  0.407f},
  {-0.625f, -0.704f},
  { 0.375f, -0.037f},
  {-0.125f,  0.630f},
  { 0.875f, -0.481f},
};
#define SS_PATTERN16_COUNT 16

/*
===============================================================

LIGHT TRACING EXECUTION

===============================================================
*/

float CalculateFalloff(float dot) {
  float val = (dot > 1.0f) ? 1.0f : dot;
  if (g_game->falloff == FALLOFF_HALFLAMBERT) {
    val = val * 0.5f + 0.5f;
    return val * val;
  } else if (g_game->falloff == FALLOFF_WRAPPED) {
    // 0.5 wrap rescaled to 0-1
    val = (val + 0.5f) / 1.5f;
    return (val < 0.0f) ? 0.0f : val;
  } else if (g_game->falloff == FALLOFF_UNREAL) {
    // Unreal angular part is standard Lambert
    return (val < 0.0f) ? 0.0f : val;
  } else if (g_game->falloff == FALLOFF_QUADRATIC) {
    if (val < 0.0f)
      return 0.0f;
    val = 1.0f - val;
    return 1.0f - (val * val);
  } else if (g_game->falloff == FALLOFF_DOUBLEQUADRATIC) {
    if (val < 0.0f)
      return 0.0f;
    val = 1.0f - val;
    return 1.0f - (val * val * val);
  }
  return (val < 0.0f) ? 0.0f : val;
}



/*
=================
TriSoupSamplePoint

Finds the position and normal for a lightmap sample point (st in pixel space)
on a triangle soup surface using barycentric interpolation.
=================
*/
qboolean TriSoupSamplePoint(dsurface_t *ds, float st[2], vec3_t origin,
                                   vec3_t normal) {
  int j, k;
  float st0[2], st1[2], st2[2];
  float area, w0, w1, w2;
  float bestExtrapolatedDistSq = 999999.0f;
  vec3_t bestExtrapOrigin;
  vec3_t bestExtrapNormal;

  for (j = 0; j < ds->numIndexes; j += 3) {
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
        st[1] < mins[1] - GUTTER || st[1] > maxs[1] + GUTTER) {
      continue;
    }

    if (PointInTriangle(st[0], st[1], st0, st1, st2)) {
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

      for (k = 0; k < 3; k++) {
        origin[k] = w0 * v0->xyz[k] + w1 * v1->xyz[k] + w2 * v2->xyz[k];
        normal[k] =
            w0 * v0->normal[k] + w1 * v1->normal[k] + w2 * v2->normal[k];
      }
      VectorNormalize(normal, normal);
      return qtrue;
    }

    // Dilation: if not inside, check if we are within the gutter distance
    // For TriSoup, we always allow this if we have a gutter
    {
      float dSq, dMin = 999999.0f;
      float t;
      int edgeBest = -1;

      dSq = DistanceSqToSegment(st[0], st[1], st0, st1, &t);
      if (dSq < dMin) {
        dMin = dSq;
        edgeBest = 0;
      }
      dSq = DistanceSqToSegment(st[0], st[1], st1, st2, &t);
      if (dSq < dMin) {
        dMin = dSq;
        edgeBest = 1;
      }
      dSq = DistanceSqToSegment(st[0], st[1], st2, st0, &t);
      if (dSq < dMin) {
        dMin = dSq;
        edgeBest = 2;
      }

      // Check if within dilation radius AND better than previous extrapolation
      if (edgeBest >= 0 && dMin < (float)GUTTER * GUTTER && dMin < bestExtrapolatedDistSq) {
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

        for (k = 0; k < 3; k++) {
          bestExtrapOrigin[k] = w0 * v0->xyz[k] + w1 * v1->xyz[k] + w2 * v2->xyz[k];
          bestExtrapNormal[k] =
              w0 * v0->normal[k] + w1 * v1->normal[k] + w2 * v2->normal[k];
        }
        VectorNormalize(bestExtrapNormal, bestExtrapNormal);
        bestExtrapolatedDistSq = dMin;
      }
    }
  }

  // If we found no exact match but found a valid extrapolation, use it
  if (bestExtrapolatedDistSq < 999999.0f) {
    VectorCopy(bestExtrapOrigin, origin);
    VectorCopy(bestExtrapNormal, normal);
    return qtrue;
  }

  return qfalse;
}

/*
================
PointToPolygonFormFactor
================
*/
float PointToPolygonFormFactor(const vec3_t point, const vec3_t normal,
                               const winding_t *w) {
  vec3_t triVector, triNormal;
  int i, j;
  vec3_t dirs[MAX_POINTS_ON_WINDING];
  float total;
  float dot, angle, facing;

  for (i = 0; i < w->numpoints; i++) {
    VectorSubtract(w->points[i], point, dirs[i]);
    VectorNormalize(dirs[i], dirs[i]);
  }

  // duplicate first vertex to avoid mod operation
  VectorCopy(dirs[0], dirs[i]);

  total = 0;
  for (i = 0; i < w->numpoints; i++) {
    j = i + 1;
    dot = DotProduct(dirs[i], dirs[j]);

    // roundoff can cause slight creep, which gives an IND from acos
    if (dot > 1.0) {
      dot = 1.0;
    } else if (dot < -1.0) {
      dot = -1.0;
    }

    angle = acos(dot);
    CrossProduct(dirs[i], dirs[j], triVector);
    if (VectorNormalize(triVector, triNormal) < 0.0001) {
      continue;
    }
    facing = DotProduct(normal, triNormal);
    total += facing * angle;

    if (total > 6.3 || total < -6.3) {
      static qboolean printed;

      if (!printed) {
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
int c_sunHit, c_sunMiss;
qboolean SunToPoint(const vec3_t origin, traceWork_t *tw, contribution_t *out,
                    qboolean applyColorFilter) {
  int i;
  trace_t trace;
  skyBrush_t *b;
  vec3_t end;

  if (!numSkyBrushes) {
    return qfalse;
  }

  VectorMA(origin, MAX_WORLD_COORD * 2, sunDirection, end);

  TraceLine(origin, end, &trace, qtrue, tw);

  // see if trace.hit is inside a sky brush
  for (i = 0; i < numSkyBrushes; i++) {
    b = &skyBrushes[i];

    // this assumes that sky brushes are axial...
    if (trace.hit[0] < b->bounds[0][0] || trace.hit[0] > b->bounds[1][0] ||
        trace.hit[1] < b->bounds[0][1] || trace.hit[1] > b->bounds[1][1] ||
        trace.hit[2] < b->bounds[0][2] || trace.hit[2] > b->bounds[1][2]) {
      continue;
    }

    // trace again to get intermediate filters
    TraceLine(origin, trace.hit, &trace, qtrue, tw);

    // we hit the sky, so add sunlight
    if (numthreads == 1) {
      c_sunHit++;
    }
    if (!applyColorFilter) {
      trace.filter[0] = trace.filter[1] = trace.filter[2] = 1.0f;
    }

    VectorCopy(sunDirection, out->dir);
    out->color[0] = trace.filter[0] * sunLight[0];
    out->color[1] = trace.filter[1] * sunLight[1];
    out->color[2] = trace.filter[2] * sunLight[2];

    return qtrue;
  }

  if (numthreads == 1) {
    c_sunMiss++;
  }

  return qfalse;
}

/*
================
SunToPlane
Returns an amount of light to add at the texel (surface)
================
*/
qboolean SunToPlane(const vec3_t origin, const vec3_t normal,
                      contribution_t *out, qboolean applyColorFilter,
                      traceWork_t *tw) {
  float angle;

  if (!numSkyBrushes) {
    return qfalse;
  }

  // if the sun is behind the surface
  if (tw->forceFrontOnly) {
    if (DotProduct(normal, sunDirection) < -0.125f) {
      return qfalse; // facing away
    }
  } else if (g_game->falloff != FALLOFF_HALFLAMBERT &&
             g_game->falloff != FALLOFF_WRAPPED) {
    if (DotProduct(normal, sunDirection) <= 0) {
      return qfalse; // facing away
    }
  }

  angle = CalculateFalloff(DotProduct(normal, sunDirection));
  if (angle <= 0) {
    return qfalse; // facing away
  }

  if (SunToPoint(origin, tw, out, applyColorFilter)) {
    VectorScale(out->color, angle, out->color);
    return qtrue;
  }

  return qfalse;
}

/*
================
LightingAtSample
================
*/
/*
========================
LightContributionToPoint
========================
*/
qboolean LightContributionToPoint(const light_t *light, const vec3_t origin,
                                  const vec3_t normal, contribution_t *out,
                                  traceWork_t *tw) {
  trace_t trace;
  float add = 0;
  vec3_t dir;
  float dist;
  float angle = 1.0f;

  // area light (Exact Point-To-Polygon Form Factor)
  if (light->type == emit_area) {
    float factor;
    float d;
    vec3_t n;

    // instant reach check
    VectorSubtract(light->origin, origin, n);
    if (VectorLength(n) > light->reach) {
        return qfalse;
    }

    // see if the point is behind the light
    d = DotProduct(origin, light->normal) - light->dist;
    if (!light->twosided) {
      if (d < 1) {
        return qfalse;
      }
    }

    // test occlusion
    TraceLine(origin, light->origin, &trace, qfalse, tw);
    if (trace.passSolid) {
      return qfalse;
    }

    // calculate the contribution


    VectorSubtract(light->origin, origin, n);
    if (VectorNormalize(n, n) == 0) {
      return qfalse;
    }
    VectorCopy(n, out->dir);

    // Liquid surfaces act as omnidirectional glowing volumes, not flat Lambertian emitters.
    // They emit "plain light" in all directions without cosine falloff.
    if (light->si && (light->si->contents & (CONTENTS_LAVA | CONTENTS_SLIME | 
                                             CONTENTS_WATER | CONTENTS_FOG))) {
        vec3_t toLight;
        VectorSubtract(light->origin, origin, toLight);
        float distToLightSq = DotProduct(toLight, toLight);
        factor = light->area / (distToLightSq + light->area);
    } else {
        // Standard Lambertian area light
        factor = PointToPolygonFormFactor(origin, n, light->w);
        if (factor <= 0) {
            if (light->twosided) {
                factor = -factor;
            } else {
                return qfalse;
            }
        }
    }

    angle = CalculateFalloff(factor);
    if (angle <= 0) {
      return qfalse;
    }

    if (normal) {
      float receiveAngle = CalculateFalloff(DotProduct(normal, out->dir));
      if (receiveAngle <= 0) {
        // NOTICE: If we allow this light to go through it must not contribute to light direction (deluxemaps). It's a "glow"
        
        // for some light sources allow a small fraction of contribution on backfaces creating a "glow effect"
        float glowFactor = 0.0f;
        if (light->si) {
            if (light->si->surfaceLightGlow >= 0.0f) {
                glowFactor = light->si->surfaceLightGlow;
            } else {
                // defaults if not explicitly configured
                if (light->si->contents & CONTENTS_LAVA) {
                    glowFactor = 0.25f;
                } else if (light->si->contents & CONTENTS_SLIME) {
                    glowFactor = 0.10f;
                }
            }
        }
        
        if (glowFactor > 0.0f) {
            angle *= glowFactor;
        } else {
          return qfalse;
        }
      }
      //angle *= receiveAngle;
    }

    out->color[0] = light->emitColor[0] * angle * trace.filter[0];
    out->color[1] = light->emitColor[1] * angle * trace.filter[1];
    out->color[2] = light->emitColor[2] * angle * trace.filter[2];
    return qtrue;
  }

  // point or spotlight logic
  if (light->type == emit_point || light->type == emit_spotlight) {
    VectorSubtract(light->origin, origin, dir);
    dist = VectorNormalize(dir, out->dir);
    if (dist > light->reach) {
      return qfalse;
    }
    if (dist < 16) {
      dist = 16;
    }
    
    // surface falloff
    if (normal) {
      angle = CalculateFalloff(DotProduct(normal, out->dir));
      if (angle <= 0) {
        return qfalse;
      }
    }

    if (light->type == emit_spotlight) {
      float distByNormal;
      float sampleRadius;
      vec3_t pointAtDist;
      vec3_t distToSample;
      float radiusAtDist;

      distByNormal = -DotProduct(out->dir, light->normal) * dist;
      if (distByNormal < 0) {
        return qfalse;
      }
      VectorMA(light->origin, distByNormal * (1.0f / dist), out->dir, pointAtDist);
      radiusAtDist = light->radiusByDist * distByNormal;
      VectorSubtract(origin, pointAtDist, distToSample);
      sampleRadius = VectorLength(distToSample);

      if (sampleRadius >= radiusAtDist) {
        return qfalse;
      }
      if (sampleRadius > radiusAtDist - 32) {
        angle *= (radiusAtDist - sampleRadius) / 32.0;
      }
    }

    if (light->linearLight) {
      add = angle * light->photons * 0.000125f - dist;
      if (add < 0) return qfalse;
    } else {
      add = (light->photons / (dist * dist)) * angle;
    }
  } else {
    return qfalse;
  }

  if (add <= MIN_LIGHT_ADD) {
    return qfalse;
  }

  // occlusion check
  TraceLine(origin, light->origin, &trace, qfalse, tw);
  if (trace.passSolid) {
    return qfalse;
  }

  out->color[0] = add * light->color[0] * trace.filter[0];
  out->color[1] = add * light->color[1] * trace.filter[1];
  out->color[2] = add * light->color[2] * trace.filter[2];

  return qtrue;
}

/*
========================
LightingAtSample
========================
*/
void LightingAtSample(const vec3_t origin, const vec3_t normal,
                      vec3_t color, qboolean testOcclusion,
                      qboolean forceSunLight, qboolean applyColorFilter,
                      traceWork_t *tw) {
  light_t *light;
  contribution_t cont;

  VectorCopy(ambientColor, color);

  for (light = lights; light; light = light->next) {
    if (LightContributionToPoint(light, origin, normal, &cont, tw)) {
      VectorAdd(color, cont.color, color);
    }
  }

  // trace directly to the sun
  if (testOcclusion || forceSunLight) {
    if (SunToPlane(origin, normal, &cont, applyColorFilter, tw)) {
      VectorAdd(color, cont.color, color);
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
                    qboolean forceSunLight, float scale, traceWork_t *tw) {
  int i;
  drawVert_t *dv;
  vec3_t sample, normal;
  float max;

  VectorCopy(ds->lightmapVecs[2], normal);

  // generate vertex lighting
  for (i = 0; i < ds->numVerts; i++) {
    dv = &drawVerts[ds->firstVert + i];

    vec3_t v_origin;
    if (ds->patchWidth || ds->surfaceType == MST_TRIANGLE_SOUP) {
      VectorMA(dv->xyz, SAMPLE_NUDGE, dv->normal, v_origin);
      LightingAtSample(v_origin, dv->normal, sample, testOcclusion,
                       forceSunLight, qfalse, tw);
    } else {
      VectorMA(dv->xyz, SAMPLE_NUDGE, normal, v_origin);
      LightingAtSample(v_origin, normal, sample, testOcclusion, forceSunLight,
                       qfalse, tw);
    }

    if (scale >= 0)
      VectorScale(sample, scale, sample);
    // clamp with color normalization
    max = sample[0];
    if (sample[1] > max) {
      max = sample[1];
    }
    if (sample[2] > max) {
      max = sample[2];
    }
    if (max > 255) {
      VectorScale(sample, 255 / max, sample);
    }

    // save the high-precision result only
    if (internalDrawVerts) {
      internalDrawVerts[ds->firstVert + i].color[0][0] += sample[0];
      internalDrawVerts[ds->firstVert + i].color[0][1] += sample[1];
      internalDrawVerts[ds->firstVert + i].color[0][2] += sample[2];
    }

    // Don't bother writing alpha since it will already be set to 255,
    // plus we don't want to write over alpha generated by SetTerrainTextures
    // dv->color[3] = 255;
  }
}

/*
=================
LinearSubdivideMesh

For extra lighting, just midpoint one of the axis.
The edges are clamped at the original edges.
=================
*/
mesh_t *LinearSubdivideMesh(mesh_t *in) {
  int i, j;
  mesh_t *out;
  drawVert_t *v1, *v2, *vout;

  out = malloc(sizeof(*out));

  out->width = in->width * 2;
  out->height = in->height;
  out->verts = malloc(out->width * out->height * sizeof(*out->verts));
  for (j = 0; j < in->height; j++) {
    out->verts[j * out->width + 0] = in->verts[j * in->width + 0];
    out->verts[j * out->width + out->width - 1] =
        in->verts[j * in->width + in->width - 1];
    for (i = 1; i < out->width - 1; i += 2) {
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
TraceLtm
=============
*/
void TraceLtm(int num) {
  dsurface_t *ds;
  int realSurfIndex;
  int i, j, k;
  int x, y;
  int position, numPositions;
  double base[3], origin_d[3];
  vec3_t origin, normal;
  traceWork_t *tw;
  tw = malloc(sizeof(traceWork_t));
  if (!tw)
    Error("Failed to allocate TraceLtm memory (traceWork_t)");
  memset(tw, 0, sizeof(traceWork_t));
  tw->ignoreSurface = -1;

  byte **occluded = NULL;
  byte *occluded_data = NULL;
  vec3_t **color = NULL;
  vec3_t *color_data = NULL;
  vec3_t average;
  int count;
  mesh_t srcMesh, *mesh = NULL, *subdivided = NULL;
  shaderInfo_t *si;
  static float nudge[2][9] = {{0, -1, 0, 1, -1, 1, -1, 0, 1},
                               {0, -1, -1, -1, 0, 0, 1, 1, 1}};
  int sampleWidth, sampleHeight, ssize;
  int extW, extH;
  vec3_t lightmapOrigin, lightmapVecs[2];
  int widthtable[MAX_EXPANDED_AXIS], heighttable[MAX_EXPANDED_AXIS];

  realSurfIndex = surfaceWorkOrder[num];
  ds = &drawSurfaces[realSurfIndex];
  si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

  int surfWeight = (ds->lightmapNum[0] >= 0) ? (ds->lightmapWidth * ds->lightmapHeight) : 1;


  // vertex-lit triangle model if no lightmap allocated
  if (ds->surfaceType == MST_TRIANGLE_SOUP && ds->lightmapNum[0] == -1) {
    tw->ignoreSurface = realSurfIndex;
    VertexLighting(ds, !si->noVertexShadows, si->forceSunLight, 1.0, tw);
    free(tw);
    ThreadCompletedWeighted(surfWeight);
    return;
  }

  if (ds->lightmapNum[0] == -1) {
    free(tw);
    ThreadCompletedWeighted(surfWeight);
    return; // doesn't need lighting at all
  }

  if (!novertexlighting) {
    // calculate the vertex lighting for gouraud shade mode
    tw->ignoreSurface = realSurfIndex;
    VertexLighting(ds, si->vertexShadows, si->forceSunLight, si->vertexScale,
                   tw);
  }

  if (ds->lightmapNum[0] < 0) {
    free(tw);
    ThreadCompletedWeighted(surfWeight);
    return; // doesn't need lightmap lighting
  }

  si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
  int superSample = upscale || (ds->surfaceType == MST_TRIANGLE_SOUP);
  int use_upscale = upscale;
  ssize = samplesize;
  if (si->lightmapSampleSize)
    ssize = si->lightmapSampleSize;

  tw->patchshadows = patchshadows;
  tw->forceFrontOnly = qtrue;

  int scale = use_upscale ? UPSCALE_FACTOR : 1;
  int currentGutter = superSample ? (GUTTER * scale) : 0;

  if (ds->surfaceType == MST_PATCH) {
    srcMesh.width = ds->patchWidth;
    srcMesh.height = ds->patchHeight;
    srcMesh.verts = drawVerts + ds->firstVert;
    mesh = SubdivideMesh(srcMesh, 8, 999);
    PutMeshOnCurve(*mesh);
    MakeMeshNormals(*mesh);

    subdivided = RemoveLinearMeshColumnsRows(mesh);
    FreeMesh(mesh);

    mesh = SubdivideMeshQuads(subdivided, ssize, LIGHTMAP_WIDTH, widthtable,
                               heighttable);
    if (mesh->width != ds->lightmapWidth ||
        mesh->height != ds->lightmapHeight) {
      Error("Mesh lightmap miscount (%dx%d != %dx%d)\n"
            "This usually happens when 'q3map' and 'light' use different -samplesize values.\n"
            "Make sure both tools use the same -samplesize flag.",
            mesh->width, mesh->height, ds->lightmapWidth, ds->lightmapHeight);
    }

    if (superSample) {
      mesh_t *mp;
      int steps = 0;
      int tempScale = scale;

      // chop it up for more light samples (leaking memory...)
      mp = mesh;

      // Calculate log2(scale) for subdivision steps
      while (tempScale > 1) {
        tempScale >>= 1;
        steps++;
      }

      for (int s = 0; s < steps; s++) {
        mp = LinearSubdivideMesh(mp);
        mp = TransposeMesh(mp);
        mp = LinearSubdivideMesh(mp);
        mp = TransposeMesh(mp);
      }

      mesh = mp;
    }
    sampleWidth = mesh->width + currentGutter * 2;
    sampleHeight = mesh->height + currentGutter * 2;
  } else {
    VectorCopy(ds->lightmapVecs[2], normal);

    if (!superSample) {
      VectorCopy(ds->lightmapOrigin, lightmapOrigin);
      VectorCopy(ds->lightmapVecs[0], lightmapVecs[0]);
      VectorCopy(ds->lightmapVecs[1], lightmapVecs[1]);
      sampleWidth = ds->lightmapWidth;
      sampleHeight = ds->lightmapHeight;
    } else {
      // sample at a closer spacing for antialiasing
      VectorCopy(ds->lightmapOrigin, lightmapOrigin);
      if (use_upscale) {
        float invScale = 1.0f / (float)scale;
        VectorScale(ds->lightmapVecs[0], invScale, lightmapVecs[0]);
        VectorScale(ds->lightmapVecs[1], invScale, lightmapVecs[1]);
        VectorMA(lightmapOrigin, -(1.0f - invScale) * 0.5f, lightmapVecs[0], lightmapOrigin);
        VectorMA(lightmapOrigin, -(1.0f - invScale) * 0.5f, lightmapVecs[1], lightmapOrigin);
      }

      // NOTE: do NOT shift origin by -currentGutter here. The trace loop uses
      // pi = i - currentGutter, which already compensates for the gutter offset.
      // Shifting origin here as well would double-displace all samples by 1 LR
      // texel, placing edge blocks outside the polygon and producing black borders.
      sampleWidth = ds->lightmapWidth * scale + currentGutter * 2;
      sampleHeight = ds->lightmapHeight * scale + currentGutter * 2;
    }
  }

  extW = sampleWidth;
  extH = sampleHeight;

  occluded = malloc(extW * sizeof(byte *));
  occluded_data = malloc(extW * extH * sizeof(byte));
  color = malloc(extW * sizeof(vec3_t *));
  color_data = malloc(extW * extH * sizeof(vec3_t));
  byte *sampleHit_data = malloc(extW * extH * sizeof(byte));
  byte **sampleHit = malloc(extW * sizeof(byte *));

  if (!occluded || !occluded_data || !color || !color_data || !sampleHit || !sampleHit_data) {
    _printf("WARNING: Failed to allocate TraceLtm memory for surface %d (%dx%d)\n", num, extW, extH);
    if (occluded) free(occluded);
    if (occluded_data) free(occluded_data);
    if (color) free(color);
    if (color_data) free(color_data);
    if (sampleHit) free(sampleHit);
    if (sampleHit_data) free(sampleHit_data);
    free(tw);
    return;
  }

  for (i = 0; i < extW; i++) {
    occluded[i] = occluded_data + i * extH;
    color[i] = color_data + i * extH;
    sampleHit[i] = sampleHit_data + i * extH;
  }

  memset(color_data, 0, extW * extH * sizeof(vec3_t));
  memset(sampleHit_data, 0, extW * extH * sizeof(byte));

  // determine which samples are occluded
  memset(occluded_data, 0, extW * extH * sizeof(byte));
  for (i = 0; i < sampleWidth; i++) {
    for (j = 0; j < sampleHeight; j++) {

      // sample at a closer spacing for anti-aliasing
      // Mode: 0 = OFF, 1 = Models Only, 2 = Everything
      // Pattern: radius <= 1 -> 8 samples, radius >= 2 -> 16 samples
      qboolean doSS = qfalse;
      if (superSampleMode == SUPERSAMPLE_ALL) {
          doSS = qtrue;
      } else if (superSampleMode == SUPERSAMPLE_MODELS && ds->surfaceType == MST_TRIANGLE_SOUP) {
          doSS = qtrue;
      }

      // Pick pattern based on smoothradius
      const float (*pattern)[2];
      int actualSamples;
      if (!doSS) {
          actualSamples = 1;
          pattern = ssPattern8; // unused, but avoids uninitialized warning
      } else if (lightmapSmoothRadius >= 2.0f) {
          actualSamples = SS_PATTERN16_COUNT;
          pattern = ssPattern16;
      } else {
          actualSamples = SS_PATTERN8_COUNT;
          pattern = ssPattern8;
      }
      
      float jitterRadius = doSS ? lightmapSmoothRadius : 0.0f;
      vec3_t accumColor = {0, 0, 0};
      int hitCount = 0;

      for (int ss = 0; ss < actualSamples; ss++) {
        // Generate jitter offset using the selected pattern
        float jdx = 0.0f, jdy = 0.0f;
        if (jitterRadius > 0.0f && ss > 0) {
          int pidx = ss % actualSamples;
          jdx = pattern[pidx][0] * jitterRadius;
          jdy = pattern[pidx][1] * jitterRadius;
        }

        if (ds->surfaceType == MST_TRIANGLE_SOUP) {
          float st[2];
          vec3_t temp_origin;
          
          // Calculate the target (s,t) coordinate in the lightmap
          // Account for the Gutter shift and the optional 0.5x supersampling
          float fi = (float)(i - currentGutter) + jdx;
          float fj = (float)(j - currentGutter) + jdy;
          float step = 1.0f / (float)scale;
          float offset = 0.5f * step;
          
          st[0] = (float)ds->lightmapOffset[0][0] + fi * step + offset;
          st[1] = (float)ds->lightmapOffset[0][1] + fj * step + offset;

          if (!TriSoupSamplePoint(ds, st, temp_origin, normal)) {
            continue; // jittered sample missed geometry, skip
          }
          numPositions = 9;
          for (k = 0; k < 3; k++) {
            origin_d[k] = (double)temp_origin[k];
            base[k] = origin_d[k] + (double)normal[k] * SAMPLE_NUDGE;
          }
          MakeNormalVectors(normal, lightmapVecs[0], lightmapVecs[1]);
        } else if (ds->patchWidth) {
          numPositions = 9;
          // Dilation: clamp to mesh bounds for the gutter
          int mi = i - currentGutter;
          int mj = j - currentGutter;
          if (mi < 0) mi = 0; 
          if (mi >= mesh->width) mi = mesh->width - 1;
          if (mj < 0) mj = 0;
          if (mj >= mesh->height) mj = mesh->height - 1;

          VectorCopy(mesh->verts[mj * mesh->width + mi].normal, normal);
          // push off of the curve a bit
          for (k = 0; k < 3; k++) {
            base[k] = (double)mesh->verts[mj * mesh->width + mi].xyz[k] +
                      (double)normal[k] * SAMPLE_NUDGE;
          }
          // Apply jitter in world space along the surface tangent plane
          if (jitterRadius > 0.0f && ss > 0) {
            MakeNormalVectors(normal, lightmapVecs[0], lightmapVecs[1]);
            for (k = 0; k < 3; k++) {
              base[k] += (double)jdx * ssize * lightmapVecs[0][k] +
                         (double)jdy * ssize * lightmapVecs[1][k];
            }
          }

          MakeNormalVectors(normal, lightmapVecs[0], lightmapVecs[1]);
        } else {
          numPositions = 9;
          // Dilation: offset the planar calculation
          float pi = (float)(i - currentGutter) + jdx;
          float pj = (float)(j - currentGutter) + jdy;
          for (k = 0; k < 3; k++) {
            base[k] = (double)lightmapOrigin[k] +
                      (double)normal[k] * SAMPLE_NUDGE +
                      (double)pi * lightmapVecs[0][k] +
                      (double)pj * lightmapVecs[1][k];
          }
        }
        for (k = 0; k < 3; k++) {
          base[k] += surfaceOrigin[num][k];
        }

        // we may need to slightly nudge the sample point
        // if directly on a wall
        for (position = 0; position < numPositions; position++) {
          // calculate lightmap sample position
          for (k = 0; k < 3; k++) {
            origin_d[k] = base[k] +
                          ((double)nudge[0][position] / 16.0) * lightmapVecs[0][k] +
                          ((double)nudge[1][position] / 16.0) * lightmapVecs[1][k];
            origin[k] = (float)origin_d[k];
          }

          if (notrace) {
            break;
          }

          // --- PointInSolid Bypass (q3map2 style) ---
          // We always use the nominal position (position 0) because our raytracer
          // uses a 1.25 unit jump (SELF_SHADOW_EPSILON) to escape from solid 
          // geometry at junctions.
          break;
        }

        // if none of the nudges worked, this sub-sample is occluded
        if (position == numPositions) {
          continue;
        }

        // Trace this sub-sample
        vec3_t subColor = {0, 0, 0};
        tw->ignoreSurface = realSurfIndex;
        LightingAtSample(origin, normal, subColor, qtrue, qfalse, qtrue, tw);
        VectorAdd(accumColor, subColor, accumColor);
        hitCount++;
      } // end super-sample loop

      // Resolve: set the texel color from accumulated sub-samples
      if (hitCount > 0) {
        if (ds->surfaceType == MST_TRIANGLE_SOUP) {
          sampleHit[i][j] = qtrue;
        }
        occluded[i][j] = qfalse;
        if (numthreads == 1) {
          c_visible++;
        }
        float invHits = 1.0f / (float)hitCount;
        color[i][j][0] = accumColor[0] * invHits;
        color[i][j][1] = accumColor[1] * invHits;
        color[i][j][2] = accumColor[2] * invHits;
      } else {
        // No sub-samples hit — mark as occluded/miss
        if (ds->surfaceType == MST_TRIANGLE_SOUP) {
          sampleHit[i][j] = qfalse;
        }
        occluded[i][j] = qtrue;
        if (numthreads == 1) {
          c_occluded++;
        }
      }

      // For non-trisoups with superSampleMode == SUPERSAMPLE_MODELS, preserve original sampleHit behavior
      if (ds->surfaceType != MST_TRIANGLE_SOUP && actualSamples == 1) {
        sampleHit[i][j] = qtrue;
      }
    }
  }


  // calculate average values for occluded samples
  for (i = 0; i < sampleWidth; i++) {
    for (j = 0; j < sampleHeight; j++) {
      if (!occluded[i][j]) {
        continue;
      }
      // scan all surrounding samples
      count = 0;
      VectorClear(average);
      for (x = -1; x <= 1; x++) {
        for (y = -1; y <= 1; y++) {
          if (i + x < 0 || i + x >= sampleWidth) {
            continue;
          }
          if (j + y < 0 || j + y >= sampleHeight) {
            continue;
          }
          if (occluded[i + x][j + y]) {
            continue;
          }
          count++;
          VectorAdd(color[i + x][j + y], average, average);
        }
      }
      if (count) {
        VectorScale(average, 1.0 / count, color[i][j]);
        sampleHit[i][j] = qtrue;
      }
    }
  }

  // downscale HD buffer to LR: pick the strongest (brightest) lit HD sample
  if (superSample && use_upscale) {
    for (i = 0; i < ds->lightmapWidth; i++) {
      for (j = 0; j < ds->lightmapHeight; j++) {
        float maxIntensity = -1.0f;
        int bestX = -1, bestY = -1;

        int baseI = i * scale + currentGutter;
        int baseJ = j * scale + currentGutter;

        for (int sx = 0; sx < scale; sx++) {
          for (int sy = 0; sy < scale; sy++) {
            int i2 = baseI + sx;
            int j2 = baseJ + sy;
            if (!sampleHit[i2][j2]) {
              continue;
            }
            float intensity = color[i2][j2][0] + color[i2][j2][1] + color[i2][j2][2];
            if (intensity > maxIntensity) {
              maxIntensity = intensity;
              bestX = i2;
              bestY = j2;
            }
          }
        }

        if (bestX >= 0) {
          VectorCopy(color[bestX][bestY], color[i][j]);
          sampleHit[i][j] = qtrue;
        } else {
          VectorClear(color[i][j]);
          sampleHit[i][j] = qfalse;
        }
      }
    }
  } else if (superSample && !use_upscale) {
    // 1:1 dilation test: copy directly from guttered buffer
    for (i = 0; i < ds->lightmapWidth; i++) {
      for (j = 0; j < ds->lightmapHeight; j++) {
        VectorCopy(color[i + currentGutter][j + currentGutter], color[i][j]);
      }
    }
  }

  // optionally create a debugging border around the lightmap
  if (lightmapBorder) {
    for (i = 0; i < ds->lightmapWidth; i++) {
      color[i][0][0] = 255;
      color[i][0][1] = 0;
      color[i][0][2] = 0;

      color[i][ds->lightmapHeight - 1][0] = 255;
      color[i][ds->lightmapHeight - 1][1] = 0;
      color[i][ds->lightmapHeight - 1][2] = 0;
    }
    for (i = 0; i < ds->lightmapHeight; i++) {
      color[0][i][0] = 255;
      color[0][i][1] = 0;
      color[0][i][2] = 0;

      color[ds->lightmapWidth - 1][i][0] = 255;
      color[ds->lightmapWidth - 1][i][1] = 0;
      color[ds->lightmapWidth - 1][i][2] = 0;
    }
  }

  // clamp the colors to bytes and store off
  for (i = 0; i < ds->lightmapWidth; i++) {
    for (j = 0; j < ds->lightmapHeight; j++) {
      k = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + j) *
              LIGHTMAP_WIDTH +
          ds->lightmapOffset[0][0] + i;

      if (lightFloats) {
        if (k >= 0 && k < numLightBytes / 3) {
            lightFloats[k * 3 + 0] += color[i][j][0];
            lightFloats[k * 3 + 1] += color[i][j][1];
            lightFloats[k * 3 + 2] += color[i][j][2];
        } else {
            // This should never happen if numLightBytes and lightmapNum are consistent
            static qboolean warned = qfalse;
            if (!warned) {
                _printf("\nWARNING: TraceLtm: lightmap index %d out of bounds (max %d) on surface %d\n", k, numLightBytes / 3, realSurfIndex);
                warned = qtrue;
            }
        }
      }

      if (lightAlphaMask) {
        if (k >= 0 && k < numLightBytes / 3) {
            if (ds->surfaceType == MST_TRIANGLE_SOUP) {
              if (sampleHit[i][j]) {
                lightAlphaMask[k] = ALPHA_TRISOUP;
              }
            } else {
              lightAlphaMask[k] = ALPHA_SURF_WORLD;
            }
        }
      }
    }
  }

  if (ds->surfaceType == MST_PATCH) {
    FreeMesh(mesh);
  }
  free(sampleHit);
  free(sampleHit_data);
  free(tw);
  free(occluded);
  free(occluded_data);
  free(color);
  free(color_data);

  ThreadCompletedWeighted(surfWeight);
}

//=============================================================================

/*
=============
TraceGrid

Grid samples are foe quickly determining the lighting
of dynamically placed entities in the world
=============
*/

#define MAX_CONTRIBUTIONS 1024

void TraceGrid(int num) {
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

  tw = malloc(sizeof(traceWork_t));
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

  if (PointInSolid(origin)) {
    vec3_t baseOrigin;
    int step;

    VectorCopy(origin, baseOrigin);

    // try to nudge the origin around to find a valid point
    for (step = 9; step <= 18; step += 9) {
      for (i = 0; i < 8; i++) {
        VectorCopy(baseOrigin, origin);
        if (i & 1) {
          origin[0] += step;
        } else {
          origin[0] -= step;
        }
        if (i & 2) {
          origin[1] += step;
        } else {
          origin[1] -= step;
        }
        if (i & 4) {
          origin[2] += step;
        } else {
          origin[2] -= step;
        }

        if (!PointInSolid(origin)) {
          break;
        }
      }
      if (i != 8) {
        break;
      }
    }
    if (step > 18) {
      // can't find a valid point at all
      if (gridData32) {
        memset(&gridData32[num], 0, sizeof(gridData32[num]));
      }
      free(tw);
      return;
    }
  }

  VectorClear(summedDir);

  // trace all lights
  numCon = 0;
  for (light = lights; light; light = light->next) {
    if (LightContributionToPoint(light, origin, NULL, &contributions[numCon], tw)) {
      float addSize = VectorLength(contributions[numCon].color);
      VectorMA(summedDir, addSize, contributions[numCon].dir, summedDir);
      numCon++;
      if (numCon >= MAX_CONTRIBUTIONS) {
        Error("TraceGrid: MAX_CONTRIBUTIONS (%i) exceeded at grid point (%f %f %f)",
              MAX_CONTRIBUTIONS, origin[0], origin[1], origin[2]);
      }
    }
  }

  // sun
  if (SunToPoint(origin, tw, &contributions[numCon], qtrue)) {
    float addSize = VectorLength(contributions[numCon].color);
    VectorMA(summedDir, addSize, contributions[numCon].dir, summedDir);
    numCon++;
    if (numCon >= MAX_CONTRIBUTIONS) {
      Error("TraceGrid: MAX_CONTRIBUTIONS (%i) exceeded with sun at grid point (%f %f %f)",
            MAX_CONTRIBUTIONS, origin[0], origin[1], origin[2]);
    }
  }

  // now that we have identified the primary light direction,
  // go back and seperate all the light into directed and ambient
  VectorNormalize(summedDir, summedDir);
  VectorCopy(ambientColor, color);
  VectorClear(directedColor);

  for (i = 0; i < numCon; i++) {
    float d;

    d = CalculateFalloff(DotProduct(contributions[i].dir, summedDir));

    VectorMA(directedColor, d, contributions[i].color, directedColor);

    // the ambient light will be at 1/4 the value of directed light
    d = 0.25 * (1.0 - d);
    VectorMA(color, d, contributions[i].color, color);
  }

  // now do some fudging to keep the ambient from being too low
  VectorMA(color, 0.25, directedColor, color);

  //
  // save the resulting value out
  //
  if (gridData32) {
    VectorAdd(color, gridData32[num].ambient[0], gridData32[num].ambient[0]);
    VectorAdd(directedColor, gridData32[num].directed[0], gridData32[num].directed[0]);
    VectorNormalize(summedDir, summedDir);
    NormalToLatLong(summedDir, gridData32[num].latLong);
    gridData32[num].styles[0] = 0;
    gridData32[num].styles[1] = 0xff;
    gridData32[num].styles[2] = 0xff;
    gridData32[num].styles[3] = 0xff;
  }

  free(tw);
}


//=============================================================================

/*
=============
LightWorld
=============
*/
void LightWorld(void) {
  double start, end;
  int i;
  long long totalLuxels = 0;

  surfaceWorkOrder = malloc(numDrawSurfaces * sizeof(int));
  for (i = 0; i < numDrawSurfaces; i++) {
    surfaceWorkOrder[i] = i;
    if (drawSurfaces[i].lightmapNum[0] >= 0) {
      totalLuxels += drawSurfaces[i].lightmapWidth * drawSurfaces[i].lightmapHeight;
    } else {
      totalLuxels += 1;
    }
  }
  qsort(surfaceWorkOrder, numDrawSurfaces, sizeof(int), CompareSurfaces);

  if (!nogridlighting) {
    if (embree) {
      _printf("--- TraceGrid (embree) ---\n");
    } else {
      _printf("--- TraceGrid (surface) ---\n");
    }
    start = I_FloatTime();
    RunThreadsOnIndividual(numGridPoints, qtrue, TraceGrid);
    end = I_FloatTime();
    _printf("%i x %i x %i = %i grid\n", gridBounds[0], gridBounds[1],
            gridBounds[2], numGridPoints);
    _printf("%5.0f seconds elapsed in TraceGrid\n", end - start);
  }

  if (embree) {
    _printf("--- TraceLtm (embree) ---\n");
  } else {
    _printf("--- TraceLtm (surface) ---\n");
  }
  start = I_FloatTime();
  RunThreadsOnWeighted(numDrawSurfaces, totalLuxels, qtrue, TraceLtm);
  end = I_FloatTime();
  _printf("%5i visible samples\n", c_visible);
  _printf("%5i occluded samples\n", c_occluded);
  _printf("%5.0f seconds elapsed in TraceLtm\n", end - start);

  free(surfaceWorkOrder);
}
