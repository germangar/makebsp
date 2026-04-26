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
#include "radiosity.h"
#include "../common/imagelib.h"
#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

ssMode_t superSampleMode = SUPERSAMPLE_NONE;

qboolean notrace;
qboolean patchshadows = qtrue;
qboolean upscale;
qboolean lightmapBorder;

qboolean debugLightmaps;
qboolean debugLightmapsAlpha;
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
float lightscale = 1.0;
float pointScale = 7500;

int *surfaceWorkOrder;
int CompareSurfaces(const void *a, const void *b) {
  int i1 = *(int *)a;
  int i2 = *(int *)b;
  dsurface_t *ds1 = &drawSurfaces[i1];
  dsurface_t *ds2 = &drawSurfaces[i2];
  int w1 = ds1->lightmapWidth * ds1->lightmapHeight;
  int w2 = ds2->lightmapWidth * ds2->lightmapHeight;

  // Bonus weight for patches and triangle soups as they take more CPU cycles per luxel
  if (ds1->surfaceType == MST_PATCH || ds1->surfaceType == MST_TRIANGLE_SOUP)
    w1 *= 2;
  if (ds2->surfaceType == MST_PATCH || ds2->surfaceType == MST_TRIANGLE_SOUP)
    w2 *= 2;

  if (w1 > w2)
    return -1;
  if (w1 < w2)
    return 1;
  return 0;
}

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
  // (NOTE: This is now handled by InjectSunEntity in the BSP stage
  // and CreateEntityLights in the Light stage via the entity system)
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
  dl->area = area;
  VectorCopy(normal, dl->normal);
  dl->dist = DotProduct(dl->origin, normal);

  value = ls->value;
  intensity = value * area * areaScale;
  VectorMA(dl->origin, 0.1f, dl->normal, dl->origin);

  VectorCopy(ls->color, dl->color);

  dl->photons = intensity;

  // emitColor is irrespective of the area
  float volumetricScale = 1.0f;
  if (ls->contents & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER | CONTENTS_FOG)) {
    volumetricScale = 0.25f;
  }

  VectorScale(ls->color, value * formFactorValueScale * areaScale * volumetricScale,
              dl->emitColor);

  dl->si = ls;

  if (ls->contents & (CONTENTS_FOG | CONTENTS_LAVA | CONTENTS_SLIME)) {
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
    dl2->reach = CalculateLightReach(0, dl2->photons, MIN_LIGHT_ADD, qfalse);
  }

  dl->reach = CalculateLightReach(area, value * areaScale, MIN_LIGHT_ADD, qfalse);
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
    int maxSide = ls->twoSided;
    // Liquids are volumetric and emit omnidirectionally from a single plane
    if (ls->contents & (CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_WATER | CONTENTS_FOG)) {
        maxSide = 0;
    }

    for (side = 0; side <= maxSide; side++) {
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

    // Check if this is a sun entity (injected or manual)
    if (ValueForKey(e, "_sun")[0]) {
      float intensity;
      const char *t;
      entity_t *tEnt;

      _printf("Processing sun entity: origin %s, target %s\n", 
              ValueForKey(e, "origin"), ValueForKey(e, "target"));

      // 1. Direction Calculation (Targeting > Vector Fallback)
      t = ValueForKey(e, "target");
      if (t && t[0] && (tEnt = FindTargetEntity(t))) {
        vec3_t sunOrigin, targetOrigin;
        GetVectorForKey(e, "origin", sunOrigin);
        GetVectorForKey(tEnt, "origin", targetOrigin);
        VectorSubtract(sunOrigin, targetOrigin, sunDirection);
      } else {
        // Fallback to high-precision dir from injector or default
        const char *sunDirKey = ValueForKey(e, "_sun_dir");
        if (sunDirKey && sunDirKey[0]) {
          GetVectorForKey(e, "_sun_dir", sunDirection);
        } else {
          VectorSet(sunDirection, -0.45, -0.3, 0.9); // Q3 default-ish (UP)
        }
      }
      VectorNormalize(sunDirection, sunDirection);

      // 2. Intensity and Color
      intensity = FloatForKey(e, "light");
      if (!intensity)
        intensity = FloatForKey(e, "_light");

      _color = ValueForKey(e, "_color");
      if (_color && _color[0]) {
        sscanf(_color, "%f %f %f", &sunLight[0], &sunLight[1], &sunLight[2]);
        
        // If the mapper provided a separate 'light' key, we treat _color as a normalized multiplier
        if (intensity > 0) {
          ColorNormalize(sunLight, sunLight);
          VectorScale(sunLight, intensity, sunLight);
        }
      } else {
        // Default white sun if no color provided
        if (intensity <= 0) intensity = 300.0f;
        VectorSet(sunLight, intensity, intensity, intensity);
      }

      _printf("Sun entity found: Direction (%f %f %f), Intensity (%f %f %f)\n",
              sunDirection[0], sunDirection[1], sunDirection[2],
              sunLight[0], sunLight[1], sunLight[2]);
      continue;
    }

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
    dl->reach = CalculateLightReach(0, dl->photons, MIN_LIGHT_ADD, dl->linearLight);
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
==========================
VisualizeLightmapAllocation
==========================
*/
void VisualizeLightmapAllocation(void) {
  int i, x, y, p, numPages;
  char filename[1024];
  dsurface_t *ds;

  int k;
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

    int superSample = upscale || (ds->surfaceType == MST_TRIANGLE_SOUP);
    int use_upscale = upscale;
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
      // FAST path: original debuglightmaps logic (rectangles)
      for (y = 0; y < ds->lightmapHeight; y++) {
        for (x = 0; x < ds->lightmapWidth; x++) {
          p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH +
              (ds->lightmapOffset[0][0] + x);
          k = p * 3;
          lightBytes[k] = color[0];
          lightBytes[k + 1] = color[1];
          lightBytes[k + 2] = color[2];
        }
      }
    }
  }

  _printf("    %d surfaces rasterized for visualization\n", rasterizedCount);

  numPages = 0;
  for (i = 0; i < numDrawSurfaces; i++) {
    if (drawSurfaces[i].lightmapNum[0] > numPages)
      numPages = drawSurfaces[i].lightmapNum[0];
  }
  numPages++; // 1-indexed count

  for (i = 0; i < numPages; i++) {
    sprintf(filename, "vis_lm_%d.bmp", i);
    _printf("    Writing %s...\n", filename);
    SaveBMP(filename, &lightBytes[i * LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3], LIGHTMAP_WIDTH, LIGHTMAP_HEIGHT, 3);
  }
}


/*
========
LightMain

========
*/
void LightMain(int radiosityPasses) {
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

  // Call radiosity passes
  LightRadiosity(radiosityPasses);

  PostProcessLightmaps();
}
