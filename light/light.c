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
#include "../common/imagelib.h"
#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

#define EXTRASCALE 2
int numSuperSamples = 0;
#define GUTTER 1

// Super-sampling patterns.
// Points are in [-1, 1] range, scaled by jitterRadius at runtime.
// The pattern is chosen automatically based on smoothradius:
//   radius <= 1  ->  8-point Rotated Grid
//   radius >= 2  -> 16-point Halton(2,3) quasi-random sequence

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


qboolean notrace;
qboolean patchshadows = qtrue;
qboolean extra;
qboolean lightmapBorder;

qboolean debugLightmaps;
qboolean debugLightmapsAlpha;
qboolean oldTrace = qfalse;
qboolean bruteTrace = qfalse;
qboolean embree = qfalse;

// CLI Overrides
qboolean falloffOverridden = qfalse;
falloff_t overrideFalloff;
qboolean lightmapsRGBOverridden = qfalse;
qboolean deluxeMapOverridden = qfalse;

extern int samplesize; // sample size in units
int novertexlighting = 0;
int nogridlighting = 0;

// for run time tweaking of all area sources in the level
float areaScale = 0.25;

// for run time tweaking of all point sources in the level
float pointScale = 7500;

qboolean exactPointToPolygon = qtrue;

float formFactorValueScale = 3;

float linearScale = 1.0 / 8000;

light_t *lights;
int numPointLights;
int numAreaLights;
 
vec3_t gridMins;
vec3_t gridSize = {64, 64, 128};
int gridBounds[3];
// numGridPoints is defined in bspfile.c



int c_visible, c_occluded;

// int			defaultLightSubdivide = 128;		// vary by
// surface size?
int defaultLightSubdivide = 999; // vary by surface size?

vec3_t ambientColor;

vec3_t surfaceOrigin[MAX_MAP_DRAW_SURFS];
int entitySurface[MAX_MAP_DRAW_SURFS];

// 7,9,11 normalized to avoid being nearly coplanar with common faces
// vec3_t		sunDirection = { 0.441835, 0.56807, 0.694313 };
// vec3_t		sunDirection = { 0, 0, 1 };

// these are usually overrided by shader values
vec3_t sunDirection = {0.45, 0.3, 0.9};
vec3_t sunLight = {100, 100, 50};

typedef struct {
  dbrush_t *b;
  vec3_t bounds[2];
} skyBrush_t;

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
void FindSkyBrushes(void) {
  int i, j;
  dbrush_t *b;
  skyBrush_t *sb;
  shaderInfo_t *si;
  dbrushside_t *s;

  // find the brushes
  for (i = 0; i < numbrushes; i++) {
    b = &dbrushes[i];
    for (j = 0; j < b->numSides; j++) {
      s = &dbrushsides[b->firstSide + j];
      if (dshaders[s->shaderNum].surfaceFlags & SURF_SKY) {
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
  for (i = 0; i < numDrawSurfaces; i++) {
    si = ShaderInfoForShader(dshaders[drawSurfaces[i].shaderNum].shader);
    if (si->surfaceFlags & SURF_SKY) {
      VectorCopy(si->sunLight, sunLight);
      VectorCopy(si->sunDirection, sunDirection);
      break;
    }
  }
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
                        float areaSubdivide, qboolean backsplash) {
  float area, value, intensity;
  light_t *dl, *dl2;
  vec3_t mins, maxs;
  int axis;
  winding_t *front, *back;
  vec3_t planeNormal;
  float planeDist;

  if (!w) {
    return;
  }

  WindingBounds(w, mins, maxs);

  // check for subdivision
  for (axis = 0; axis < 3; axis++) {
    if (maxs[axis] - mins[axis] > areaSubdivide) {
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
  if (area <= 0 || area > 20000000) {
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
  VectorCopy(normal, dl->normal);
  dl->dist = DotProduct(dl->origin, normal);

  value = ls->value;
  intensity = value * area * areaScale;
  VectorAdd(dl->origin, dl->normal, dl->origin);

  VectorCopy(ls->color, dl->color);

  dl->photons = intensity;

  // emitColor is irrespective of the area
  VectorScale(ls->color, value * formFactorValueScale * areaScale,
              dl->emitColor);

  dl->si = ls;

  if (ls->contents & CONTENTS_FOG) {
    dl->twosided = qtrue;
  }

  // optionally create a point backsplash light
  if (backsplash && ls->backsplashFraction > 0) {
    dl2 = malloc(sizeof(*dl));
    memset(dl2, 0, sizeof(*dl2));
    dl2->next = lights;
    lights = dl2;
    dl2->type = emit_point;

    VectorMA(dl->origin, ls->backsplashDistance, normal, dl2->origin);

    VectorCopy(ls->color, dl2->color);

    dl2->photons = dl->photons * ls->backsplashFraction;
    dl2->si = ls;
  }
}

/*
===============
CreateSurfaceLights

This creates area lights
===============
*/
void CreateSurfaceLights(void) {
  int i, j, side;
  dsurface_t *ds;
  shaderInfo_t *ls;
  winding_t *w;
  cFacet_t *f;
  light_t *dl;
  vec3_t origin;
  drawVert_t *dv;
  int c_lightSurfaces;
  float lightSubdivide;
  vec3_t normal;

  qprintf("--- CreateSurfaceLights ---\n");
  c_lightSurfaces = 0;

  for (i = 0; i < numDrawSurfaces; i++) {
    // see if this surface is light emiting
    ds = &drawSurfaces[i];

    ls = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
    if (ls->value == 0) {
      continue;
    }

    // determine how much we need to chop up the surface
    if (ls->lightSubdivide) {
      lightSubdivide = ls->lightSubdivide;
    } else {
      lightSubdivide = defaultLightSubdivide;
    }

    c_lightSurfaces++;

    // an autosprite shader will become
    // a point light instead of an area light
    if (ls->autosprite) {
      // autosprite geometry should only have four vertexes
      if (surfaceTest[i]) {
        // curve or misc_model
        f = surfaceTest[i]->facets;
        if (surfaceTest[i]->numFacets != 1 || f->numBoundaries != 4) {
          _printf("WARNING: surface at (%i %i %i) has autosprite shader but "
                  "isn't a quad\n",
                  (int)f->points[0][0], (int)f->points[0][1],
                  (int)f->points[0][2]);
        }
        VectorAdd(f->points[0], f->points[1], origin);
        VectorAdd(f->points[2], origin, origin);
        VectorAdd(f->points[3], origin, origin);
        VectorScale(origin, 0.25, origin);
      } else {
        // normal polygon
        dv = &drawVerts[ds->firstVert];
        if (ds->numVerts != 4) {
          _printf("WARNING: surface at (%i %i %i) has autosprite shader but %i "
                  "verts\n",
                  (int)dv->xyz[0], (int)dv->xyz[1], (int)dv->xyz[2]);
          continue;
        }

        VectorAdd(dv[0].xyz, dv[1].xyz, origin);
        VectorAdd(dv[2].xyz, origin, origin);
        VectorAdd(dv[3].xyz, origin, origin);
        VectorScale(origin, 0.25, origin);
      }

      numPointLights++;
      dl = malloc(sizeof(*dl));
      memset(dl, 0, sizeof(*dl));
      dl->next = lights;
      lights = dl;

      VectorCopy(origin, dl->origin);
      VectorCopy(ls->color, dl->color);
      dl->photons = ls->value * pointScale;
      dl->type = emit_point;
      continue;
    }

    // possibly create for both sides of the polygon
    for (side = 0; side <= ls->twoSided; side++) {
      // create area lights
      if (surfaceTest[i]) {
        // curve or misc_model
        for (j = 0; j < surfaceTest[i]->numFacets; j++) {
          f = surfaceTest[i]->facets + j;
          w = AllocWinding(f->numBoundaries);
          w->numpoints = f->numBoundaries;
          memcpy(w->points, f->points, f->numBoundaries * 12);

          VectorCopy(f->surface, normal);
          if (side) {
            winding_t *t;

            t = w;
            w = ReverseWinding(t);
            FreeWinding(t);
            VectorSubtract(vec3_origin, normal, normal);
          }
          SubdivideAreaLight(ls, w, normal, lightSubdivide, qtrue);
        }
      } else {
        // normal polygon

        w = AllocWinding(ds->numVerts);
        w->numpoints = ds->numVerts;
        for (j = 0; j < ds->numVerts; j++) {
          VectorCopy(drawVerts[ds->firstVert + j].xyz, w->points[j]);
        }
        VectorCopy(ds->lightmapVecs[2], normal);
        if (side) {
          winding_t *t;

          t = w;
          w = ReverseWinding(t);
          FreeWinding(t);
          VectorSubtract(vec3_origin, normal, normal);
        }
        SubdivideAreaLight(ls, w, normal, lightSubdivide, qtrue);
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
entity_t *FindTargetEntity(const char *target) {
  int i;
  const char *n;

  for (i = 0; i < num_entities; i++) {
    n = ValueForKey(&entities[i], "targetname");
    if (!strcmp(n, target)) {
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
void CreateEntityLights(void) {
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
  for (i = 0; i < num_entities; i++) {
    e = &entities[i];
    name = ValueForKey(e, "classname");
    if (strncmp(name, "light", 5))
      continue;

    numPointLights++;
    dl = malloc(sizeof(*dl));
    memset(dl, 0, sizeof(*dl));
    dl->next = lights;
    lights = dl;

    spawnflags = FloatForKey(e, "spawnflags");
    if (spawnflags & 1) {
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
    if (_color && _color[0]) {
      sscanf(_color, "%f %f %f", &dl->color[0], &dl->color[1], &dl->color[2]);
      ColorNormalize(dl->color, dl->color);
    } else
      dl->color[0] = dl->color[1] = dl->color[2] = 1.0;

    intensity = intensity * pointScale;
    dl->photons = intensity;

    dl->type = emit_point;

    // lights with a target will be spotlights
    target = ValueForKey(e, "target");

    if (target[0]) {
      float radius;
      float dist;

      e2 = FindTargetEntity(target);
      if (!e2) {
        _printf("WARNING: light at (%i %i %i) has missing target\n",
                (int)dl->origin[0], (int)dl->origin[1], (int)dl->origin[2]);
      } else {
        GetVectorForKey(e2, "origin", dest);
        VectorSubtract(dest, dl->origin, dl->normal);
        dist = VectorNormalize(dl->normal, dl->normal);
        radius = FloatForKey(e, "radius");
        if (!radius) {
          radius = 64;
        }
        if (!dist) {
          dist = 64;
        }
        dl->radiusByDist = (radius + 16) / dist;
        dl->type = emit_spotlight;
      }
    }
  }
}

/*
================
SetEntityOrigins

Find the offset values for inline models
================
*/
void SetEntityOrigins(void) {
  int i, j;
  entity_t *e;
  vec3_t origin;
  const char *key;
  int modelnum;
  dmodel_t *dm;

  for (i = 0; i < num_entities; i++) {
    e = &entities[i];
    key = ValueForKey(e, "model");
    if (key[0] != '*') {
      continue;
    }
    modelnum = atoi(key + 1);
    dm = &dmodels[modelnum];

    // set entity surface to true for all surfaces for this model
    for (j = 0; j < dm->numSurfaces; j++) {
      entitySurface[dm->firstSurface + j] = qtrue;
    }

    key = ValueForKey(e, "origin");
    if (!key[0]) {
      continue;
    }
    GetVectorForKey(e, "origin", origin);

    // set origin for all surfaces for this model
    for (j = 0; j < dm->numSurfaces; j++) {
      VectorCopy(origin, surfaceOrigin[dm->firstSurface + j]);
    }
  }
}



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


static qboolean PointInTriangle(float px, float py, float v0[2], float v1[2],
                                float v2[2]) {
  float d1, d2, d3;
  qboolean has_neg, has_pos;

  d1 = (px - v1[0]) * (v0[1] - v1[1]) - (v0[0] - v1[0]) * (py - v1[1]);
  d2 = (px - v2[0]) * (v1[1] - v2[1]) - (v1[0] - v2[0]) * (py - v2[1]);
  d3 = (px - v0[0]) * (v2[1] - v0[1]) - (v2[0] - v0[0]) * (py - v0[1]);

  has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
  has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);

  return !(has_neg && has_pos);
}

/*
=================
DistanceSqToSegment

Returns the squared distance from a point to a line segment in 2D.
Also returns the parametric 't' value of the closest point [0, 1].
=================
*/
static float DistanceSqToSegment(float px, float py, float v0[2], float v1[2],
                                 float *t) {
  float dx = v1[0] - v0[0];
  float dy = v1[1] - v0[1];
  float l2 = dx * dx + dy * dy;
  if (l2 == 0.0f) {
    if (t)
      *t = 0.0f;
    return (px - v0[0]) * (px - v0[0]) + (py - v0[1]) * (py - v0[1]);
  }
  float tt = ((px - v0[0]) * dx + (py - v0[1]) * dy) / l2;
  if (t)
    *t = tt;
  if (tt < 0.0f)
    return (px - v0[0]) * (px - v0[0]) + (py - v0[1]) * (py - v0[1]);
  if (tt > 1.0f)
    return (px - v1[0]) * (px - v1[0]) + (py - v1[1]) * (py - v1[1]);
  float projx = v0[0] + tt * dx;
  float projy = v0[1] + tt * dy;
  return (px - projx) * (px - projx) + (py - projy) * (py - projy);
}

/*
=================
TriSoupSamplePoint

Finds the position and normal for a lightmap sample point (st in pixel space)
on a triangle soup surface using barycentric interpolation.
=================
*/
static qboolean TriSoupSamplePoint(dsurface_t *ds, float st[2], vec3_t origin,
                                   vec3_t normal) {
  int j, k;
  float st0[2], st1[2], st2[2];
  float area, w0, w1, w2;

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

      // Check if within dilation radius
      if (edgeBest >= 0 && dMin < (float)GUTTER * GUTTER) {
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
          origin[k] = w0 * v0->xyz[k] + w1 * v1->xyz[k] + w2 * v2->xyz[k];
          normal[k] =
              w0 * v0->normal[k] + w1 * v1->normal[k] + w2 * v2->normal[k];
        }
        VectorNormalize(normal, normal);
        return qtrue;
      }
    }
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

  // area light with exact PTPFF
  if (exactPointToPolygon && light->type == emit_area) {
    float factor;
    float d;
    vec3_t n;

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

    factor = PointToPolygonFormFactor(origin, n, light->w);
    if (factor <= 0) {
      if (light->twosided) {
        factor = -factor;
      } else {
        return qfalse;
      }
    }
    angle = CalculateFalloff(factor);
    if (angle <= 0) {
      return qfalse;
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
  } else if (light->type == emit_area) {
    // legacy/approximate area light logic
    VectorSubtract(light->origin, origin, dir);
    dist = VectorNormalize(dir, out->dir);
    if (dist < 16) dist = 16;
    
    if (normal) {
      angle = CalculateFalloff(DotProduct(normal, out->dir));
      if (angle <= 0) return qfalse;
    }
    
    // light surface orientation check
    float emitAngle = -DotProduct(light->normal, out->dir);
    if (emitAngle <= 0) return qfalse;
    angle *= emitAngle;

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
  int i, j;
  drawVert_t *dv;
  vec3_t sample, normal;
  float max;

  VectorCopy(ds->lightmapVecs[2], normal);

  // generate vertex lighting
  for (i = 0; i < ds->numVerts; i++) {
    dv = &drawVerts[ds->firstVert + i];

    if (ds->patchWidth) {
      LightingAtSample(dv->xyz, dv->normal, sample, testOcclusion,
                       forceSunLight, qfalse, tw);
    } else if (ds->surfaceType == MST_TRIANGLE_SOUP) {
      LightingAtSample(dv->xyz, dv->normal, sample, testOcclusion,
                       forceSunLight, qfalse, tw);
    } else {
      LightingAtSample(dv->xyz, normal, sample, testOcclusion, forceSunLight,
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

  ds = &drawSurfaces[num];
  si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

  // vertex-lit triangle model if no lightmap allocated
  if (ds->surfaceType == MST_TRIANGLE_SOUP && ds->lightmapNum[0] == -1) {
    VertexLighting(ds, !si->noVertexShadows, si->forceSunLight, 1.0, tw);
    free(tw);
    return;
  }

  if (ds->lightmapNum[0] == -1) {
    free(tw);
    return; // doesn't need lighting at all
  }

  if (!novertexlighting) {
    // calculate the vertex lighting for gouraud shade mode
    VertexLighting(ds, si->vertexShadows, si->forceSunLight, si->vertexScale,
                   tw);
  }

  if (ds->lightmapNum[0] < 0) {
    free(tw);
    return; // doesn't need lightmap lighting
  }

  si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
  int superSample = extra || (ds->surfaceType == MST_TRIANGLE_SOUP);
  int use_upscale = extra;
  ssize = samplesize;
  if (si->lightmapSampleSize)
    ssize = si->lightmapSampleSize;

  tw->patchshadows = patchshadows;
  tw->forceFrontOnly = qtrue;

  int scale = use_upscale ? 2 : 1;
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
      Error("Mesh lightmap miscount");
    }

    if (superSample) {
      mesh_t *mp;

      // chop it up for more light samples (leaking memory...)
      mp = mesh; // CopyMesh( mesh );
      mp = LinearSubdivideMesh(mp);
      mp = TransposeMesh(mp);
      mp = LinearSubdivideMesh(mp);
      mp = TransposeMesh(mp);

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
        VectorScale(ds->lightmapVecs[0], 0.5, lightmapVecs[0]);
        VectorScale(ds->lightmapVecs[1], 0.5, lightmapVecs[1]);
        VectorMA(lightmapOrigin, -0.5, lightmapVecs[0], lightmapOrigin);
        VectorMA(lightmapOrigin, -0.5, lightmapVecs[1], lightmapOrigin);
      }

      // Dilation: shift origin for gutter
      VectorMA(lightmapOrigin, -currentGutter, lightmapVecs[0], lightmapOrigin);
      VectorMA(lightmapOrigin, -currentGutter, lightmapVecs[1], lightmapOrigin);
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

      // --- Super-sampling ---
      // Mode: 0 = OFF, 1 = Models Only, 2 = Everything
      // Pattern: radius <= 1 -> 8 samples, radius >= 2 -> 16 samples
      qboolean doSS = qfalse;
      if (numSuperSamples == 2) {
          doSS = qtrue;
      } else if (numSuperSamples == 1 && ds->surfaceType == MST_TRIANGLE_SOUP) {
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

          if (!PointInSolid(origin)) {
            break;
          }
        }

        // if none of the nudges worked, this sub-sample is occluded
        if (position == numPositions) {
          continue;
        }

        // Trace this sub-sample
        vec3_t subColor = {0, 0, 0};
        tw->ignoreSurface = num;
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

      // For non-trisoups with numSuperSamples == 1, preserve original sampleHit behavior
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
      }
    }
  }

  // average together the values if we are extra sampling
  if (superSample && use_upscale) {
    for (i = 0; i < ds->lightmapWidth; i++) {
      for (j = 0; j < ds->lightmapHeight; j++) {
        vec3_t value;
        float coverage;

        int i2 = i * scale + currentGutter;
        int j2 = j * scale + currentGutter;

        VectorClear(value);
        coverage = 0;

        if (sampleHit[i2][j2]) {
          VectorAdd(value, color[i2][j2], value);
          coverage++;
        }
        if (sampleHit[i2][j2 + 1]) {
          VectorAdd(value, color[i2][j2 + 1], value);
          coverage++;
        }
        if (sampleHit[i2 + 1][j2]) {
          VectorAdd(value, color[i2 + 1][j2], value);
          coverage++;
        }
        if (sampleHit[i2 + 1][j2 + 1]) {
          VectorAdd(value, color[i2 + 1][j2 + 1], value);
          coverage++;
        }

        if (coverage > 0.0f) {
          color[i][j][0] = value[0] / coverage;
          color[i][j][1] = value[1] / coverage;
          color[i][j][2] = value[2] / coverage;
        } else {
          VectorClear(color[i][j]);
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
        lightFloats[k * 3 + 0] += color[i][j][0];
        lightFloats[k * 3 + 1] += color[i][j][1];
        lightFloats[k * 3 + 2] += color[i][j][2];
      }

      if (lightAlphaMask) {
        // If this pixel ended up with any coverage/hits, mark it as processed
        // For superSampled, dilation ensures coverage > 0 if there was any hit nearby.
        // For standard, we just mark the pixel if it's within the surface's rectangle
        // since Q3Map traces all texels in the allocated lightmap for planar/patch types.
        if (ds->surfaceType == MST_TRIANGLE_SOUP) {
          // For models, we must be careful to only mark pixels that actually contain geometry
          if (superSample) {
            // Check if this pixel had any coverage during dilation
            // This is actually already handled by the 'coverage' check in the superSample block
            // but we can just check if the final color is not the clear color if we prefer.
            // However, a more robust way is to check the Hit status.
            // For now, if we are in this loop, we are within the lightmap bounds.
            if (color[i][j][0] != 0 || color[i][j][1] != 0 || color[i][j][2] != 0) {
                 lightAlphaMask[k] = ALPHA_NO_SMOOTH;
            }
          } else {
            if (sampleHit[i][j]) {
              lightAlphaMask[k] = ALPHA_NO_SMOOTH;
            }
          }
        } else {
          // For standard patches and faces, the entire allocated rectangle is valid
          lightAlphaMask[k] = ALPHA_SMOOTH;
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
}

//=============================================================================

int gridBounds[3];

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
  float addSize;

  tw = malloc(sizeof(traceWork_t));
  if (!tw)
    Error("Failed to allocate traceWork_t");
  memset(tw, 0, sizeof(traceWork_t));

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

  if (!nogridlighting) {
    if (embree) {
      _printf("--- TraceGrid (embree) ---\n");
    } else if (oldTrace) {
      _printf("--- TraceGrid (legacy) ---\n");
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
  } else if (oldTrace) {
    _printf("--- TraceLtm (legacy) ---\n");
  } else {
    _printf("--- TraceLtm (surface) ---\n");
  }
  start = I_FloatTime();
  RunThreadsOnIndividual(numDrawSurfaces, qtrue, TraceLtm);
  end = I_FloatTime();
  _printf("%5i visible samples\n", c_visible);
  _printf("%5i occluded samples\n", c_occluded);
  _printf("%5.0f seconds elapsed in TraceLtm\n", end - start);
}



/*
==========================
VisualizeLightmapAllocation
Note: This function doesn't really belong to tracing
nor geometry initialization, but uses TriSoupSamplePoint
==========================
*/
void VisualizeLightmapAllocation(void) {
  int i, x, y, p, numPages;
  char filename[1024];
  dsurface_t *ds;

  int j, k;
  byte color[3];
  int rasterizedCount = 0;

  _printf("--- VisualizeLightmapAllocation ---\n");
  _printf("numLightBytes: %d, Page Size: %dx%d\n", numLightBytes, LIGHTMAP_WIDTH, LIGHTMAP_HEIGHT);

  // Clear the entire lightmap buffer with a dark grey color
  memset(lightBytes, 24, numLightBytes);

  for (i = 0; i < numDrawSurfaces; i++) {
    ds = &drawSurfaces[i];
    if (ds->lightmapNum[0] < 0)
      continue;

    rasterizedCount++;

    int superSample = extra || (ds->surfaceType == MST_TRIANGLE_SOUP);
    int use_upscale = extra;
    int scale = use_upscale ? 2 : 1;
    int currentGutter = superSample ? (GUTTER * scale) : 0;

    // generate a unique color for this surface based on index
    color[0] = (i * 123) % 200 + 55;
    color[1] = (i * 456) % 200 + 55;
    color[2] = (i * 789) % 200 + 55;

    if (debugLightmapsAlpha) {
      if (ds->surfaceType == MST_TRIANGLE_SOUP) {
        // Use the exact same logic as TraceLtm for model lightmaps
        int extW = ds->lightmapWidth * scale + currentGutter * 2;
        int extH = ds->lightmapHeight * scale + currentGutter * 2;

        for (y = 0; y < extH; y++) {
          for (x = 0; x < extW; x++) {
            float st[2];
            vec3_t temp_origin, normal;
            float fi = (float)(x - currentGutter);
            float fj = (float)(y - currentGutter);
            float step = 1.0f / (float)scale;
            float offset = 0.5f * step;

            st[0] = (float)ds->lightmapOffset[0][0] + fi * step + offset;
            st[1] = (float)ds->lightmapOffset[0][1] + fj * step + offset;

            if (TriSoupSamplePoint(ds, st, temp_origin, normal)) {
              int px = (int)floor(st[0]);
              int py = (int)floor(st[1]);

              if (px >= 0 && px < LIGHTMAP_WIDTH && py >= 0 && py < LIGHTMAP_HEIGHT) {
                p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + py) * LIGHTMAP_WIDTH + px;
                k = p * 3;
                lightBytes[k] = color[0];
                lightBytes[k + 1] = color[1];
                lightBytes[k + 2] = color[2];
              }
            }
          }
        }
      } else {
        // For patches and planar surfaces, we also honor the dilated bounds
        int extW = ds->lightmapWidth * scale + currentGutter * 2;
        int extH = ds->lightmapHeight * scale + currentGutter * 2;

        for (y = 0; y < extH; y++) {
          for (x = 0; x < extW; x++) {
            int px = ds->lightmapOffset[0][0] + (x - currentGutter) / scale;
            int py = ds->lightmapOffset[0][1] + (y - currentGutter) / scale;

            if (px >= 0 && px < LIGHTMAP_WIDTH && py >= 0 && py < LIGHTMAP_HEIGHT) {
              p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + py) * LIGHTMAP_WIDTH + px;
              k = p * 3;
              lightBytes[k] = color[0];
              lightBytes[k + 1] = color[1];
              lightBytes[k + 2] = color[2];
            }
          }
        }
      }
    } else {
      // FAST path: original debuglightmaps logic
      if (ds->surfaceType == MST_TRIANGLE_SOUP) {
        // Rasterize triangles into the lightmap
        for (j = 0; j < ds->numIndexes; j += 3) {
          int i0 = drawIndexes[ds->firstIndex + j];
          int i1 = drawIndexes[ds->firstIndex + j + 1];
          int i2 = drawIndexes[ds->firstIndex + j + 2];

          drawVert_t *v0 = &drawVerts[ds->firstVert + i0];
          drawVert_t *v1 = &drawVerts[ds->firstVert + i1];
          drawVert_t *v2 = &drawVerts[ds->firstVert + i2];

          // UVs in lightmap space
          float st0[2], st1[2], st2[2];
          st0[0] = v0->lightmap[0][0] * LIGHTMAP_WIDTH;
          st0[1] = v0->lightmap[0][1] * LIGHTMAP_HEIGHT;
          st1[0] = v1->lightmap[0][0] * LIGHTMAP_WIDTH;
          st1[1] = v1->lightmap[0][1] * LIGHTMAP_HEIGHT;
          st2[0] = v2->lightmap[0][0] * LIGHTMAP_WIDTH;
          st2[1] = v2->lightmap[0][1] * LIGHTMAP_HEIGHT;

          // Bounding box of the triangle in pixels
          float fMinX = st0[0];
          if (st1[0] < fMinX) fMinX = st1[0];
          if (st2[0] < fMinX) fMinX = st2[0];
          float fMaxX = st0[0];
          if (st1[0] > fMaxX) fMaxX = st1[0];
          if (st2[0] > fMaxX) fMaxX = st2[0];
          float fMinY = st0[1];
          if (st1[1] < fMinY) fMinY = st1[1];
          if (st2[1] < fMinY) fMinY = st2[1];
          float fMaxY = st0[1];
          if (st1[1] > fMaxY) fMaxY = st1[1];
          if (st2[1] > fMaxY) fMaxY = st2[1];

          int minX = (int)floor(fMinX);
          int maxX = (int)ceil(fMaxX);
          int minY = (int)floor(fMinY);
          int maxY = (int)ceil(fMaxY);

          // Clamp to lightmap block bounds
          if (minX < ds->lightmapOffset[0][0]) minX = ds->lightmapOffset[0][0];
          if (maxX >= ds->lightmapOffset[0][0] + ds->lightmapWidth) maxX = ds->lightmapOffset[0][0] + ds->lightmapWidth - 1;
          if (minY < ds->lightmapOffset[0][1]) minY = ds->lightmapOffset[0][1];
          if (maxY >= ds->lightmapOffset[0][1] + ds->lightmapHeight) maxY = ds->lightmapOffset[0][1] + ds->lightmapHeight - 1;

          for (y = minY; y <= maxY; y++) {
            for (x = minX; x <= maxX; x++) {
              if (PointInTriangle((float)x + 0.5f, (float)y + 0.5f, st0, st1, st2)) {
                p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + y) * LIGHTMAP_WIDTH + x;
                k = p * 3;
                lightBytes[k] = color[0];
                lightBytes[k + 1] = color[1];
                lightBytes[k + 2] = color[2];
              }
            }
          }
        }
      } else {
        // Standard rectangular filling for planar/patch surfaces
        for (y = 0; y < ds->lightmapHeight; y++) {
          for (x = 0; x < ds->lightmapWidth; x++) {
            p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT +
                 ds->lightmapOffset[0][1] + y) *
                    LIGHTMAP_WIDTH +
                (ds->lightmapOffset[0][0] + x);
            k = p * 3;
            lightBytes[k] = color[0];
            lightBytes[k + 1] = color[1];
            lightBytes[k + 2] = color[2];
          }
        }
      }
    }
  }

  // export pages to BMP
  _printf("%5i surfaces rasterized into debug lightmaps\n", rasterizedCount);

  numPages = numLightBytes / (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3);
  for (i = 0; i < numPages; i++) {
    sprintf(filename, "lm_%04i.bmp", i);
    _printf("Writing %s...\n", filename);
    SaveBMP(filename, lightBytes + (i * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3),
            LIGHTMAP_WIDTH, LIGHTMAP_HEIGHT, 3);
  }
}

/*
========
LightMain

========
*/
void LightMain(void) {
  float f;
  
  _printf("--- LightMain ---\n");

  // find the optional world ambient
  GetVectorForKey(&entities[0], "_color", ambientColor);
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
    for (i = 0; i < numDrawSurfaces; i++) {
      ds = &drawSurfaces[i];
      for (j = 0; j < 4; j++) {
        if (ds->lightmapNum[j] > count) {
          count = ds->lightmapNum[j];
        }
      }
      if (ds->lightmapNum[0] >= 0) {
        numSamples += ds->lightmapWidth * ds->lightmapHeight;
      }
    }

    count++;
    numLightBytes = count * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3;
    if (numLightBytes > MAX_MAP_LIGHTING) {
      Error("MAX_MAP_LIGHTING exceeded");
    }

    _printf("%5i drawSurfaces\n", numDrawSurfaces);
    _printf("%5i lightmaps\n", count);
    _printf("%5i lightmap samples\n", numSamples);
  }

  if (debugLightmaps) {
    VisualizeLightmapAllocation();
    return;
  }

  // Initialize the grid
  {
    int i;
    vec3_t maxs;

    const char *value = ValueForKey(&entities[0], "gridsize");
    if (strlen(value)) {
        sscanf(value, "%f %f %f", &gridSize[0], &gridSize[1], &gridSize[2]);
        _printf("grid size = {%1.1f, %1.1f, %1.1f}\n", gridSize[0], gridSize[1],
                gridSize[2]);
    }

    for (i = 0; i < 3; i++) {
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
  _printf("%i point lights\n", numPointLights);
  _printf("%i area lights\n", numAreaLights);

  InitTrace();
  LightWorld();
}
