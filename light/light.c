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
#define GUTTER 1

typedef struct {
  float plane[4];
  vec3_t origin;
  vec3_t vectors[2];
  shaderInfo_t *si;
} filter_t;

#define MAX_FILTERS 1024
filter_t filters[MAX_FILTERS];
int numFilters;

extern char source[1024];

qboolean notrace;
qboolean patchshadows;
qboolean dump;
qboolean extra;
qboolean extraWide;
qboolean lightmapBorder;

qboolean noSurfaces;
qboolean debugLightmaps;

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

FILE *dumpFile;

int c_visible, c_occluded;

// int			defaultLightSubdivide = 128;		// vary by
// surface size?
int defaultLightSubdivide = 999; // vary by surface size?

vec3_t ambientColor;

vec3_t surfaceOrigin[MAX_MAP_DRAW_SURFS];
int entitySurface[MAX_MAP_DRAW_SURFS];

// 7,9,11 normalized to avoid being nearly coplanar with common faces
// vec3_t		sunDirection = { 0.441835, 0.56807, 0.694313 };
// vec3_t		sunDirection = { 0.45, 0, 0.9 };
// vec3_t		sunDirection = { 0, 0, 1 };

// these are usually overrided by shader values
vec3_t sunDirection = {0.45, 0.3, 0.9};
vec3_t sunLight = {100, 100, 50};

typedef struct {
  dbrush_t *b;
  vec3_t bounds[2];
} skyBrush_t;

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

int numSkyBrushes;
skyBrush_t skyBrushes[MAX_MAP_BRUSHES];

/*

the corners of a patch mesh will always be exactly at lightmap samples.
The dimensions of the lightmap will be equal to the average length of the
control mesh in each dimension divided by 2. The lightmap sample points should
correspond to the chosen subdivision points.

*/

/*
===============================================================

SURFACE LOADING

===============================================================
*/

#define MAX_FACE_POINTS 128

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
CountLightmaps
===============
*/
void CountLightmaps(void) {
  int count;
  int i, j;
  dsurface_t *ds;
  int numSamples = 0;

  _printf("--- CountLightmaps ---\n");
  count = -1;
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
==========================
VisualizeLightmapAllocation
==========================
*/
void VisualizeLightmapAllocation(void) {
  int i, x, y, p, numPages;
  char filename[1024];
  dsurface_t *ds;
  vec3_t v0, v1, v2;
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

    if (ds->surfaceType == MST_TRIANGLE_SOUP) {
    }

    // generate a unique color for this surface based on index
    // avoid too dark or too bright colors
    color[0] = (i * 123) % 200 + 55;
    color[1] = (i * 456) % 200 + 55;
    color[2] = (i * 789) % 200 + 55;

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
        if (st1[0] < fMinX)
          fMinX = st1[0];
        if (st2[0] < fMinX)
          fMinX = st2[0];
        float fMaxX = st0[0];
        if (st1[0] > fMaxX)
          fMaxX = st1[0];
        if (st2[0] > fMaxX)
          fMaxX = st2[0];
        float fMinY = st0[1];
        if (st1[1] < fMinY)
          fMinY = st1[1];
        if (st2[1] < fMinY)
          fMinY = st2[1];
        float fMaxY = st0[1];
        if (st1[1] > fMaxY)
          fMaxY = st1[1];
        if (st2[1] > fMaxY)
          fMaxY = st2[1];

        int minX = (int)floor(fMinX);
        int maxX = (int)ceil(fMaxX);
        int minY = (int)floor(fMinY);
        int maxY = (int)ceil(fMaxY);

        // Clamp to lightmap block bounds
        if (minX < ds->lightmapOffset[0][0])
          minX = ds->lightmapOffset[0][0];
        if (maxX >= ds->lightmapOffset[0][0] + ds->lightmapWidth)
          maxX = ds->lightmapOffset[0][0] + ds->lightmapWidth - 1;
        if (minY < ds->lightmapOffset[0][1])
          minY = ds->lightmapOffset[0][1];
        if (maxY >= ds->lightmapOffset[0][1] + ds->lightmapHeight)
          maxY = ds->lightmapOffset[0][1] + ds->lightmapHeight - 1;

        for (y = minY; y <= maxY; y++) {
          for (x = minX; x <= maxX; x++) {
            if (PointInTriangle((float)x + 0.5f, (float)y + 0.5f, st0, st1,
                                st2)) {
              p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + y) * LIGHTMAP_WIDTH +
                  x;
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
          memcpy(w->p, f->points, f->numBoundaries * 12);

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
          VectorCopy(drawVerts[ds->firstVert + j].xyz, w->p[j]);
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
=================================================================

  LIGHT SETUP

=================================================================
*/

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

//=================================================================

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
=================================================================


=================================================================
*/

#define MAX_POINTS_ON_WINDINGS 64

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
    VectorSubtract(w->p[i], point, dirs[i]);
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
                total, w->p[i][0], w->p[i][1], w->p[i][2], point[0], point[1],
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
FilterTrace

Returns 0 to 1.0 filter fractions for the given trace
================
*/
void FilterTrace(const vec3_t start, const vec3_t end, vec3_t filter) {
  float d1, d2;
  filter_t *f;
  int filterNum;
  vec3_t point;
  float frac;
  int i;
  float s, t;
  int u, v;
  int x, y;
  byte *pixel;
  float radius;
  float len;
  vec3_t total;

  filter[0] = 1.0;
  filter[1] = 1.0;
  filter[2] = 1.0;

  for (filterNum = 0; filterNum < numFilters; filterNum++) {
    f = &filters[filterNum];

    // see if the plane is crossed
    d1 = DotProduct(start, f->plane) - f->plane[3];
    d2 = DotProduct(end, f->plane) - f->plane[3];

    if ((d1 < 0) == (d2 < 0)) {
      continue;
    }

    // calculate the crossing point
    frac = d1 / (d1 - d2);

    for (i = 0; i < 3; i++) {
      point[i] = start[i] + frac * (end[i] - start[i]);
    }

    VectorSubtract(point, f->origin, point);

    s = DotProduct(point, f->vectors[0]);
    t = 1.0 - DotProduct(point, f->vectors[1]);
    if (s < 0 || s >= 1.0 || t < 0 || t >= 1.0) {
      continue;
    }

    // decide the filter size
    radius = 10 * frac;
    len = VectorLength(f->vectors[0]);
    if (!len) {
      continue;
    }
    radius = radius * len * f->si->width;

    // look up the filter, taking multiple samples
    VectorClear(total);
    for (u = -1; u <= 1; u++) {
      for (v = -1; v <= 1; v++) {
        x = s * f->si->width + u * radius;
        if (x < 0) {
          x = 0;
        }
        if (x >= f->si->width) {
          x = f->si->width - 1;
        }
        y = t * f->si->height + v * radius;
        if (y < 0) {
          y = 0;
        }
        if (y >= f->si->height) {
          y = f->si->height - 1;
        }

        pixel = f->si->pixels + (y * f->si->width + x) * 4;
        total[0] += pixel[0];
        total[1] += pixel[1];
        total[2] += pixel[2];
      }
    }

    filter[0] *= total[0] / (255.0 * 9);
    filter[1] *= total[1] / (255.0 * 9);
    filter[2] *= total[2] / (255.0 * 9);
  }
}

/*
================
SunToPoint

Returns an amount of light to add at the point
================
*/
int c_sunHit, c_sunMiss;
void SunToPoint(const vec3_t origin, traceWork_t *tw, vec3_t addLight) {
  int i;
  trace_t trace;
  skyBrush_t *b;
  vec3_t end;

  if (!numSkyBrushes) {
    VectorClear(addLight);
    return;
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
    addLight[0] = trace.filter[0] * sunLight[0];
    addLight[1] = trace.filter[1] * sunLight[1];
    addLight[2] = trace.filter[2] * sunLight[2];

    return;
  }

  if (numthreads == 1) {
    c_sunMiss++;
  }

  VectorClear(addLight);
}

/*
================
SunToPlane
================
*/
void SunToPlane(const vec3_t origin, const vec3_t normal, vec3_t color,
                traceWork_t *tw) {
  float angle;
  vec3_t sunColor;

  if (!numSkyBrushes) {
    return;
  }

  // if the sun is behind the surface
  if (tw->forceFrontOnly) {
    if (DotProduct(normal, sunDirection) < -0.125f) {
      return; // facing away
    }
  } else if (g_game->falloff != FALLOFF_HALFLAMBERT && g_game->falloff != FALLOFF_WRAPPED) {
    if (DotProduct(normal, sunDirection) <= 0) {
      return; // facing away
    }
  }

  angle = CalculateFalloff(DotProduct(normal, sunDirection));
  if (angle <= 0) {
    return; // facing away
  }

  SunToPoint(origin, tw, sunColor);
  VectorMA(color, angle, sunColor, color);
}

/*
================
LightingAtSample
================
*/
void LightingAtSample(vec3_t origin, vec3_t normal, vec3_t color,
                      qboolean testOcclusion, qboolean forceSunLight,
                      traceWork_t *tw) {
  light_t *light;
  trace_t trace;
  float angle;
  float add = 0;
  float dist;
  vec3_t dir;

  VectorCopy(ambientColor, color);

  // trace to all the lights
  for (light = lights; light; light = light->next) {

    // if the light is behind the surface
    if (tw->forceFrontOnly) {
      if (DotProduct(light->origin, normal) - DotProduct(normal, origin) < -0.125f)
        continue;
    } else if (g_game->falloff != FALLOFF_HALFLAMBERT && g_game->falloff != FALLOFF_WRAPPED) {
      if (DotProduct(light->origin, normal) - DotProduct(normal, origin) < 0)
        continue;
    }
    // testing exact PTPFF
    if (exactPointToPolygon && light->type == emit_area) {
      float factor;
      float d;
      vec3_t pushedOrigin;

      // see if the point is behind the light
      d = DotProduct(origin, light->normal) - light->dist;
      if (!light->twosided) {
        if (d < -1) {
          continue; // point is behind light
        }
      }

      // test occlusion and find light filters
      // clip the line, tracing from the surface towards the light
      if (!notrace && testOcclusion) {
        TraceLine(origin, light->origin, &trace, qfalse, tw);

        // other light rays must not hit anything
        if (trace.passSolid) {
          continue;
        }
      } else {
        trace.filter[0] = 1.0;
        trace.filter[1] = 1.0;
        trace.filter[2] = 1.0;
      }

      // nudge the point so that it is clearly forward of the light
      // so that surfaces meeting a light emiter don't get black edges
      if (d > -8 && d < 8) {
        VectorMA(origin, (8 - d), light->normal, pushedOrigin);
      } else {
        VectorCopy(origin, pushedOrigin);
      }

      // calculate the contribution
      {
        float formFactor = PointToPolygonFormFactor(pushedOrigin, normal, light->w);
        if (formFactor < 0 && light->twosided) {
          formFactor = -formFactor;
        }
        factor = CalculateFalloff(formFactor);
        if (factor <= 0) {
          continue;
        }
      }
      color[0] += factor * light->emitColor[0] * trace.filter[0];
      color[1] += factor * light->emitColor[1] * trace.filter[1];
      color[2] += factor * light->emitColor[2] * trace.filter[2];

      continue;
    }

    // calculate the amount of light at this sample
    if (light->type == emit_point) {
      VectorSubtract(light->origin, origin, dir);
      dist = VectorNormalize(dir, dir);
      // clamp the distance to prevent super hot spots
      if (dist < 16) {
        dist = 16;
      }
      angle = CalculateFalloff(DotProduct(normal, dir));
      if (g_game->falloff == FALLOFF_UNREAL) {
        // Unreal Windowed Inverse Square
        // R is the distance where light would naturally fall below 1.0 (clamped)
        float R = sqrt(light->photons);
        if (dist > R || R < 0.001f) {
           add = 0;
        } else {
           float ratio = dist / R;
           float ratio2 = ratio * ratio;
           float window = 1.0f - ratio2 * ratio2;
           if (window < 0) window = 0;
           // + 1.0f on bottom to prevent hotspots
           add = (light->photons / (dist * dist + 1.0f)) * (window * window) * angle;
        }
      } else if (light->linearLight) {
        add = angle * light->photons * linearScale - dist;
        if (add < 0) {
          add = 0;
        }
      } else {
        add = light->photons / (dist * dist) * angle;
      }
    } else if (light->type == emit_spotlight) {
      float distByNormal;
      vec3_t pointAtDist;
      float radiusAtDist;
      float sampleRadius;
      vec3_t distToSample;
      float coneScale;

      VectorSubtract(light->origin, origin, dir);

      distByNormal = -DotProduct(dir, light->normal);
      if (distByNormal < 0) {
        continue;
      }
      VectorMA(light->origin, distByNormal, light->normal, pointAtDist);
      radiusAtDist = light->radiusByDist * distByNormal;

      VectorSubtract(origin, pointAtDist, distToSample);
      sampleRadius = VectorLength(distToSample);

      if (sampleRadius >= radiusAtDist) {
        continue; // outside the cone
      }
      if (sampleRadius <= radiusAtDist - 32) {
        coneScale = 1.0; // fully inside
      } else {
        coneScale = (radiusAtDist - sampleRadius) / 32.0;
      }

      dist = VectorNormalize(dir, dir);
      // clamp the distance to prevent super hot spots
      if (dist < 16) {
        dist = 16;
      }
      angle = CalculateFalloff(DotProduct(normal, dir));
      add = light->photons / (dist * dist) * angle * coneScale;

    } else if (light->type == emit_area) {
      VectorSubtract(light->origin, origin, dir);
      dist = VectorNormalize(dir, dir);
      // clamp the distance to prevent super hot spots
      if (dist < 16) {
        dist = 16;
      }
      angle = CalculateFalloff(DotProduct(normal, dir));
      if (angle <= 0) {
        continue;
      }
      angle *= -DotProduct(light->normal, dir);
      if (angle <= 0) {
        continue;
      }

      if (light->linearLight) {
        add = angle * light->photons * linearScale - dist;
        if (add < 0) {
          add = 0;
        }
      } else {
        add = light->photons / (dist * dist) * angle;
      }
    }

    if (add <= 1.0) {
      continue;
    }

    // clip the line, tracing from the surface towards the light
    if (!notrace && testOcclusion) {
      TraceLine(origin, light->origin, &trace, qfalse, tw);

      // other light rays must not hit anything
      if (trace.passSolid) {
        continue;
      }
    } else {
      trace.filter[0] = 1;
      trace.filter[1] = 1;
      trace.filter[2] = 1;
    }

    // add the result
    color[0] += add * light->color[0] * trace.filter[0];
    color[1] += add * light->color[1] * trace.filter[1];
    color[2] += add * light->color[2] * trace.filter[2];
  }

  //
  // trace directly to the sun
  //
  if (testOcclusion || forceSunLight) {
    SunToPlane(origin, normal, color, tw);
  }
}

/*
=============
PrintOccluded

For debugging
=============
*/
void PrintOccluded(byte **occluded, int width, int height) {
  int i, j;

  _printf("\n");

  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      _printf("%i", (int)occluded[j][i]);
    }
    _printf("\n");
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
                       forceSunLight, tw);
    } else if (ds->surfaceType == MST_TRIANGLE_SOUP) {
      LightingAtSample(dv->xyz, dv->normal, sample, testOcclusion,
                       forceSunLight, tw);
    } else {
      LightingAtSample(dv->xyz, normal, sample, testOcclusion, forceSunLight,
                       tw);
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

    // save the sample
    for (j = 0; j < 3; j++) {
      if (sample[j] > 255) {
        sample[j] = 255;
      }
      dv->color[0][j] = sample[j];
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
==============
ColorToBytes
==============
*/
void ColorToBytes(const float *color, byte *colorBytes) {
  float max;
  vec3_t sample;
  int i;

  VectorCopy(color, sample);

  if (g_game->lightmapsRGB) {
    for (i = 0; i < 3; i++) {
      float l = sample[i] / 255.0f;
      if (l <= 0.0031308f)
        l *= 12.92f;
      else
        l = 1.055f * pow(l, 1.0f / 2.4f) - 0.055f;
      sample[i] = l * 255.0f;
    }
  }

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
  colorBytes[0] = (byte)sample[0];
  colorBytes[1] = (byte)sample[1];
  colorBytes[2] = (byte)sample[2];
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

  if (si->patchShadows)
    tw->patchshadows = qtrue;
  else
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

      if (ds->surfaceType == MST_TRIANGLE_SOUP) {
        float st[2];
        vec3_t temp_origin;
        
        // Calculate the target (s,t) coordinate in the lightmap
        // Account for the Gutter shift and the optional 0.5x supersampling
        float fi = (float)(i - currentGutter);
        float fj = (float)(j - currentGutter);
        float step = 1.0f / (float)scale;
        float offset = 0.5f * step;
        
        st[0] = (float)ds->lightmapOffset[0][0] + fi * step + offset;
        st[1] = (float)ds->lightmapOffset[0][1] + fj * step + offset;

        if (!TriSoupSamplePoint(ds, st, temp_origin, normal)) {
          sampleHit[i][j] = qfalse;
          continue; // totally outside any dilated triangle
        }
        sampleHit[i][j] = qtrue;
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

        MakeNormalVectors(normal, lightmapVecs[0], lightmapVecs[1]);
      } else {
        numPositions = 9;
        // Dilation: offset the planar calculation
        int pi = i - currentGutter;
        int pj = j - currentGutter;
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

      if (ds->surfaceType != MST_TRIANGLE_SOUP) {
        sampleHit[i][j] = qtrue;
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
        if (!PointInSolid(origin)) {
          break;
        }
      }

      // if none of the nudges worked, this sample is occluded
      if (position == numPositions) {
        occluded[i][j] = qtrue;
        if (numthreads == 1) {
          c_occluded++;
        }
        continue;
      }

      if (numthreads == 1) {
        c_visible++;
      }
      occluded[i][j] = qfalse;
      LightingAtSample(origin, normal, color[i][j], qtrue, qfalse, tw);
    }
  }

  if (dump) {
    PrintOccluded(occluded, sampleWidth, sampleHeight);
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

        if (extraWide) {
          // wider than box filter
          // We can now use the gutter data even at edges!
          int ii, jj;
          for (ii = -2; ii <= 3; ii++) {
            for (jj = -2; jj <= 3; jj++) {
              if (ii >= 0 && ii <= 1 && jj >= 0 && jj <= 1)
                continue; // Already added
              int ni = i2 + ii;
              int nj = j2 + jj;
              if (ni >= 0 && ni < sampleWidth && nj >= 0 && nj < sampleHeight) {
                if (sampleHit[ni][nj]) {
                  VectorAdd(value, color[ni][nj], value);
                  coverage++;
                }
              }
            }
          }
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

      ColorToBytes(color[i][j], lightBytes + k * 3);
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

vec3_t gridMins;
vec3_t gridSize = {64, 64, 128};
int gridBounds[3];

/*
========================
LightContributionToPoint
========================
*/
qboolean LightContributionToPoint(const light_t *light, const vec3_t origin,
                                  vec3_t color, traceWork_t *tw) {
  trace_t trace;
  float add;

  add = 0;

  VectorClear(color);

  // testing exact PTPFF
  if (exactPointToPolygon && light->type == emit_area) {
    float factor;
    float d;
    vec3_t normal;

    // see if the point is behind the light
    d = DotProduct(origin, light->normal) - light->dist;
    if (!light->twosided) {
      if (d < 1) {
        return qfalse; // point is behind light
      }
    }

    // test occlusion
    // clip the line, tracing from the surface towards the light
    TraceLine(origin, light->origin, &trace, qfalse, tw);
    if (trace.passSolid) {
      return qfalse;
    }

    // calculate the contribution
    VectorSubtract(light->origin, origin, normal);
    if (VectorNormalize(normal, normal) == 0) {
      return qfalse;
    }
    factor = PointToPolygonFormFactor(origin, normal, light->w);
    if (factor <= 0) {
      if (light->twosided) {
        factor = -factor;
      } else {
        return qfalse;
      }
    }
    VectorScale(light->emitColor, factor, color);
    return qtrue;
  }

  // calculate the amount of light at this sample
  if (light->type == emit_point || light->type == emit_spotlight) {
    vec3_t dir;
    float dist;

    VectorSubtract(light->origin, origin, dir);
    dist = VectorLength(dir);
    // clamp the distance to prevent super hot spots
    if (dist < 16) {
      dist = 16;
    }
    if (light->linearLight) {
      add = light->photons * linearScale - dist;
      if (add < 0) {
        add = 0;
      }
    } else {
      add = light->photons / (dist * dist);
    }
  } else {
    return qfalse;
  }

  if (add <= 1.0) {
    return qfalse;
  }

  // clip the line, tracing from the surface towards the light
  TraceLine(origin, light->origin, &trace, qfalse, tw);

  // other light rays must not hit anything
  if (trace.passSolid) {
    return qfalse;
  }

  // add the result
  color[0] = add * light->color[0];
  color[1] = add * light->color[1];
  color[2] = add * light->color[2];

  return qtrue;
}

typedef struct {
  vec3_t dir;
  vec3_t color;
} contribution_t;

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
      memset(&gridData[num], 0, sizeof(gridData[num]));
      free(tw);
      return;
    }
  }

  VectorClear(summedDir);

  // trace to all the lights

  // find the major light direction, and divide the
  // total light between that along the direction and
  // the remaining in the ambient
  numCon = 0;
  for (light = lights; light; light = light->next) {
    vec3_t add;
    vec3_t dir;
    float addSize;

    if (!LightContributionToPoint(light, origin, add, tw)) {
      continue;
    }

    VectorSubtract(light->origin, origin, dir);
    VectorNormalize(dir, dir);

    VectorCopy(add, contributions[numCon].color);
    VectorCopy(dir, contributions[numCon].dir);
    numCon++;

    addSize = VectorLength(add);
    VectorMA(summedDir, addSize, dir, summedDir);

    if (numCon == MAX_CONTRIBUTIONS - 1) {
      break;
    }
  }

  //
  // trace directly to the sun
  //
  SunToPoint(origin, tw, color);
  addSize = VectorLength(color);
  if (addSize > 0) {
    VectorCopy(color, contributions[numCon].color);
    VectorCopy(sunDirection, contributions[numCon].dir);
    VectorMA(summedDir, addSize, sunDirection, summedDir);
    numCon++;
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
  memset(&gridData[num], 0, sizeof(gridData[num]));
  ColorToBytes(color, gridData[num].ambient[0]);
  ColorToBytes(directedColor, gridData[num].directed[0]);

  VectorNormalize(summedDir, summedDir);
  NormalToLatLong(summedDir, gridData[num].latLong);
  free(tw);
}

/*
=============
SetupGrid
=============
*/
void SetupGrid(void) {
  int i;
  vec3_t maxs;

  for (i = 0; i < 3; i++) {
    gridMins[i] = gridSize[i] * ceil(dmodels[0].mins[i] / gridSize[i]);
    maxs[i] = gridSize[i] * floor(dmodels[0].maxs[i] / gridSize[i]);
    gridBounds[i] = (maxs[i] - gridMins[i]) / gridSize[i] + 1;
  }

  numGridPoints = gridBounds[0] * gridBounds[1] * gridBounds[2];
  if (numGridPoints * sizeof(bspGridPoint_t) >= MAX_MAP_LIGHTGRID)
    Error("MAX_MAP_LIGHTGRID");
  qprintf("%5i gridPoints\n", numGridPoints);
}

//=============================================================================

/*
=============
RemoveLightsInSolid
=============
*/
void RemoveLightsInSolid(void) {
  light_t *light, *prev;
  int numsolid = 0;

  prev = NULL;
  for (light = lights; light;) {
    if (PointInSolid(light->origin)) {
      if (prev)
        prev->next = light->next;
      else
        lights = light->next;
      if (light->w)
        FreeWinding(light->w);
      free(light);
      numsolid++;
      if (prev)
        light = prev->next;
      else
        light = lights;
    } else {
      prev = light;
      light = light->next;
    }
  }
  _printf(" %7i lights in solid\n", numsolid);
}

/*
=============
LightWorld
=============
*/
void LightWorld(void) {
  float f;

  // determine the number of grid points
  SetupGrid();

  // find the optional world ambient
  GetVectorForKey(&entities[0], "_color", ambientColor);
  f = FloatForKey(&entities[0], "ambient");
  VectorScale(ambientColor, f, ambientColor);

  // create lights out of patches and lights
  _printf("--- CreateLights ---\n");
  CreateEntityLights();
  _printf("%i point lights\n", numPointLights);
  _printf("%i area lights\n", numAreaLights);

  if (!nogridlighting) {
    _printf("--- TraceGrid ---\n");
    RunThreadsOnIndividual(numGridPoints, qtrue, TraceGrid);
    _printf("%i x %i x %i = %i grid\n", gridBounds[0], gridBounds[1],
            gridBounds[2], numGridPoints);
  }

  _printf("--- TraceLtm ---\n");
  RunThreadsOnIndividual(numDrawSurfaces, qtrue, TraceLtm);
  _printf("%5i visible samples\n", c_visible);
  _printf("%5i occluded samples\n", c_occluded);
}

/*
========
CreateFilters

EXPERIMENTAL, UNUSED

Look for transparent light filter surfaces.

This will only work for flat 3*3 patches that exactly hold one copy of the
texture.
========
*/
#define PLANAR_PATCH_EPSILON 0.1
void CreateFilters(void) {
  int i;
  filter_t *f;
  dsurface_t *ds;
  drawVert_t *v1, *v2, *v3;
  vec3_t d1, d2;
  int vertNum;

  numFilters = 0;

  return;

  for (i = 0; i < numDrawSurfaces; i++) {
    ds = &drawSurfaces[i];
    if (!ds->patchWidth) {
      continue;
    }
    ShaderInfoForShader(dshaders[ds->shaderNum].shader);
    /*
                    if ( !(si->surfaceFlags & SURF_LIGHTFILTER) ) {
                            continue;
                    }
    */

    // we have a filter patch
    v1 = &drawVerts[ds->firstVert];

    if (ds->patchWidth != 3 || ds->patchHeight != 3) {
      _printf("WARNING: patch at %i %i %i has SURF_LIGHTFILTER but isn't a 3 "
              "by 3\n",
              v1->xyz[0], v1->xyz[1], v1->xyz[2]);
      continue;
    }

    if (numFilters == MAX_FILTERS) {
      Error("MAX_FILTERS");
    }
    f = &filters[numFilters];
    numFilters++;

    v2 = &drawVerts[ds->firstVert + 2];
    v3 = &drawVerts[ds->firstVert + 6];

    VectorSubtract(v2->xyz, v1->xyz, d1);
    VectorSubtract(v3->xyz, v1->xyz, d2);
    VectorNormalize(d1, d1);
    VectorNormalize(d2, d2);
    CrossProduct(d1, d2, f->plane);
    f->plane[3] = DotProduct(v1->xyz, f->plane);

    // make sure all the control points are on the plane
    for (vertNum = 0; vertNum < ds->numVerts; vertNum++) {
      float d;

      d = DotProduct(drawVerts[ds->firstVert + vertNum].xyz, f->plane) -
          f->plane[3];
      if (fabs(d) > PLANAR_PATCH_EPSILON) {
        break;
      }
    }
    if (vertNum != ds->numVerts) {
      numFilters--;
      _printf(
          "WARNING: patch at %i %i %i has SURF_LIGHTFILTER but isn't flat\n",
          v1->xyz[0], v1->xyz[1], v1->xyz[2]);
      continue;
    }
  }

  f = &filters[0];
  numFilters = 1;

  f->plane[0] = 1;
  f->plane[1] = 0;
  f->plane[2] = 0;
  f->plane[3] = 448;

  f->origin[0] = 448;
  f->origin[1] = 192;
  f->origin[2] = 0;

  f->vectors[0][0] = 0;
  f->vectors[0][1] = -1.0 / 128;
  f->vectors[0][2] = 0;

  f->vectors[1][0] = 0;
  f->vectors[1][1] = 0;
  f->vectors[1][2] = 1.0 / 128;

  f->si = ShaderInfoForShader("textures/hell/blocks11ct");
}

/*
=============
VertexLightingThread
=============
*/
void VertexLightingThread(int num) {
  dsurface_t *ds;
  traceWork_t *tw;
  shaderInfo_t *si;

  tw = malloc(sizeof(traceWork_t));
  if (!tw)
    Error("Failed to allocate traceWork_t");
  memset(tw, 0, sizeof(traceWork_t));

  ds = &drawSurfaces[num];

  // vertex-lit triangle model
  if (ds->surfaceType == MST_TRIANGLE_SOUP) {
    return;
  }

  if (novertexlighting)
    return;

  if (ds->lightmapNum[0] == -1) {
    return; // doesn't need lighting at all
  }

  si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

  // calculate the vertex lighting for gouraud shade mode
  VertexLighting(ds, si->vertexShadows, si->forceSunLight, si->vertexScale, tw);
  free(tw);
}

/*
=============
TriSoupLightingThread
=============
*/
void TriSoupLightingThread(int num) {
  dsurface_t *ds;
  traceWork_t *tw;
  shaderInfo_t *si;

  tw = malloc(sizeof(traceWork_t));
  if (!tw)
    Error("Failed to allocate traceWork_t");
  memset(tw, 0, sizeof(traceWork_t));

  ds = &drawSurfaces[num];
  si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);

  // vertex-lit triangle model
  if (ds->surfaceType == MST_TRIANGLE_SOUP) {
    VertexLighting(ds, !si->noVertexShadows, si->forceSunLight, 1.0, tw);
  }
  free(tw);
}

/*
=============
GridAndVertexLighting
=============
*/
void GridAndVertexLighting(void) {
  SetupGrid();

  FindSkyBrushes();
  CreateFilters();
  InitTrace();
  CreateEntityLights();
  CreateSurfaceLights();

  if (!nogridlighting) {
    _printf("--- TraceGrid ---\n");
    RunThreadsOnIndividual(numGridPoints, qtrue, TraceGrid);
  }

  if (!novertexlighting) {
    _printf("--- Vertex Lighting ---\n");
    RunThreadsOnIndividual(numDrawSurfaces, qtrue, VertexLightingThread);
  }

  _printf("--- Model Lighting ---\n");
  RunThreadsOnIndividual(numDrawSurfaces, qtrue, TriSoupLightingThread);
}

/*
========
LightMain

========
*/
int LightMain(int argc, char **argv) {
  int i;
  double start, end;
  const char *value;

  _printf("----- Lighting ----\n");

  verbose = qfalse;
  extra = qfalse;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-tempname")) {
      i++;
    } else if (!strcmp(argv[i], "-v")) {
      verbose = qtrue;
    } else if (!strcmp(argv[i], "-threads")) {
      numthreads = atoi(argv[i + 1]);
      i++;
    } else if (!strcmp(argv[i], "-area")) {
      areaScale *= atof(argv[i + 1]);
      _printf("area light scaling at %f\n", areaScale);
      i++;
    } else if (!strcmp(argv[i], "-point")) {
      pointScale *= atof(argv[i + 1]);
      _printf("point light scaling at %f\n", pointScale);
      i++;
    } else if (!strcmp(argv[i], "-notrace")) {
      notrace = qtrue;
      _printf("No occlusion tracing\n");
    } else if (!strcmp(argv[i], "-patchshadows")) {
      patchshadows = qtrue;
      _printf("Patch shadow casting enabled\n");
    } else if (!strcmp(argv[i], "-extra")) {
      extra = qtrue;
      _printf("Extra detail tracing\n");
    } else if (!strcmp(argv[i], "-extrawide")) {
      extra = qtrue;
      extraWide = qtrue;
      _printf("Extra wide detail tracing\n");
    } else if (!strcmp(argv[i], "-samplesize")) {
      samplesize = atoi(argv[i + 1]);
      if (samplesize < 1)
        samplesize = 1;
      i++;
      _printf("lightmap sample size is %dx%d units\n", samplesize, samplesize);
    } else if (!strcmp(argv[i], "-novertex")) {
      novertexlighting = qtrue;
      _printf("no vertex lighting = true\n");
    } else if (!strcmp(argv[i], "-nogrid")) {
      nogridlighting = qtrue;
      _printf("no grid lighting = true\n");
    } else if (!strcmp(argv[i], "-border")) {
      lightmapBorder = qtrue;
      _printf("Adding debug border to lightmaps\n");
    } else if (!strcmp(argv[i], "-nosurf")) {
      noSurfaces = qtrue;
      _printf("Not tracing against surfaces\n");
    } else if (!strcmp(argv[i], "-dump")) {
      dump = qtrue;
      _printf("Dumping occlusion maps\n");
    } else if (!strcmp(argv[i], "-debug_lightmaps")) {
      debugLightmaps = qtrue;
      _printf("Lightmap debug visualization enabled\n");
    } else if (!strcmp(argv[i], "-game")) {
      char *arg = argv[++i];
      int j;
      for (j = 0; games[j].arg; j++) {
        if (!strcmp(games[j].arg, arg)) {
          g_game = &games[j];
          break;
        }
      }
      strcpy(gamedir, arg);
    } else if (!strcmp(argv[i], "-sRGB")) {
      g_game->lightmapsRGB = qtrue;
      lightmapsRGBOverridden = qtrue;
      _printf("sRGB lightmaps enabled\n");
    } else if (!strcmp(argv[i], "-falloff")) {
      char *arg = argv[++i];
          if (!strcmp(arg, "halflambert")) {
            g_game->falloff = FALLOFF_HALFLAMBERT;
            _printf("Half-Lambert attenuation enabled\n");
          } else if (!strcmp(arg, "lambert")) {
            g_game->falloff = FALLOFF_LAMBERT;
            _printf("Lambert attenuation enabled\n");
          } else if (!strcmp(arg, "quadratic")) {
            g_game->falloff = FALLOFF_QUADRATIC;
            _printf("Quadratic attenuation enabled\n");
          } else if (!strcmp(arg, "doublequadratic")) {
            g_game->falloff = FALLOFF_DOUBLEQUADRATIC;
            _printf("Double Quadratic attenuation enabled\n");
          } else if (!strcmp(arg, "unreal")) {
            g_game->falloff = FALLOFF_UNREAL;
            _printf("Unreal Windowed Inverse Square attenuation enabled\n");
          } else if (!strcmp(arg, "wrapped")) {
            g_game->falloff = FALLOFF_WRAPPED;
            _printf("Wrapped Lambert (0.5) attenuation enabled\n");
          } else {
            Error("Unknown falloff type: %s", arg);
          }
          falloffOverridden = qtrue;
          overrideFalloff = g_game->falloff;
    } else if (!strcmp(argv[i], "-deluxe")) {
      g_game->deluxeMap = qtrue;
      deluxeMapOverridden = qtrue;
      _printf("Deluxemaps enabled\n");
    } else {
      break;
    }
  }

  ThreadSetDefault();

  if (i != argc - 1) {
    _printf("usage: q3map -light [-<switch> [-<switch> ...]] <mapname>\n"
            "\n"
            "Switches:\n"
            "   v              = verbose output\n"
            "   threads <X>    = set number of threads to X\n"
            "   area <V>       = set the area light scale to V\n"
            "   point <W>      = set the point light scale to W\n"
            "   notrace        = don't cast any shadows\n"
            "   extra          = enable super sampling for anti-aliasing\n"
            "   extrawide      = same as extra but smoothen more\n"
            "   nogrid         = don't calculate light grid for dynamic model "
            "lighting\n"
            "   novertex       = don't calculate vertex lighting\n"
            "   samplesize <N> = set the lightmap pixel size to NxN units\n"
            "   falloff <type>  = set the falloff model (lambert, halflambert,\n"
            "                     quadratic, doublequadratic)\n"
            "   debug_lightmaps = visualize lightmap allocation and export BMPs\n");
    exit(0);
  }

  start = I_FloatTime();

  SetQdirFromPath(argv[i]);

#ifdef _WIN32
  InitPakFile(gamedir, NULL);
#endif

  strcpy(source, ExpandArg(argv[i]));
  StripExtension(source);
  DefaultExtension(source, ".bsp");

  LoadShaderInfo();

  _printf("reading %s\n", source);

  LoadBSPFile(source);
  _printf("Active game: %s (BSP format: %s)\n", g_game->arg, g_game->bspIdent);

  // Re-apply CLI overrides that might have been clobbered by LoadBSPFile profile detection
  if (falloffOverridden) {
    g_game->falloff = overrideFalloff;
    _printf("Restoring CLI override: Falloff mode\n");
  }
  if (lightmapsRGBOverridden) {
    g_game->lightmapsRGB = qtrue;
    _printf("Restoring CLI override: sRGB lightmaps\n");
  }
  if (deluxeMapOverridden) {
    g_game->deluxeMap = qtrue;
    _printf("Restoring CLI override: Deluxemaps\n");
  }

  if (samplesize == 0) {
    samplesize = g_game->defaultSampleSize;
    _printf("Defaulting lightmap sample size to %dx%d units\n", samplesize,
            samplesize);
  }

  FindSkyBrushes();

  ParseEntities();

  value = ValueForKey(&entities[0], "gridsize");
  if (strlen(value)) {
    sscanf(value, "%f %f %f", &gridSize[0], &gridSize[1], &gridSize[2]);
    _printf("grid size = {%1.1f, %1.1f, %1.1f}\n", gridSize[0], gridSize[1],
            gridSize[2]);
  }

  CreateFilters();

  InitTrace();

  SetEntityOrigins();

  CountLightmaps();

  if (debugLightmaps) {
    VisualizeLightmapAllocation();
    _printf("writing %s\n", source);
    WriteBSPFile(source);
    end = I_FloatTime();
    _printf("%5.0f seconds elapsed\n", end - start);
    return 0;
  }

  CreateSurfaceLights();

  LightWorld();

  _printf("writing %s\n", source);
  WriteBSPFile(source);

  end = I_FloatTime();
  _printf("%5.0f seconds elapsed\n", end - start);

  return 0;
}
