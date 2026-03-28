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

#include "../common/bspfile.h"
#include "../common/lightdata.h"
#include "../common/cmdlib.h"
#include "../common/mathlib.h"
#include "../common/polylib.h"
#include "../common/scriplib.h"
#include "../common/threads.h"
#include "../shared/globals.h"
#include "../shared/mesh.h"
#include "../shared/shaders.h"
#include <embree4/rtcore.h>

extern RTCDevice g_device;
extern RTCScene g_scene;

#define SAMPLE_NUDGE 1.0f
#define SELF_SHADOW_EPSILON 1.25f
#define MIN_LIGHT_ADD 0.1f
#define MIN_EMBREE_LIGHT_ADD 0.01f

typedef enum { emit_point, emit_area, emit_spotlight, emit_sun } emittype_t;

#define MAX_LIGHT_EDGES 8
typedef struct light_s {
  struct light_s *next;
  emittype_t type;
  struct shaderInfo_s *si;

  vec3_t origin;
  vec3_t normal; // for surfaces, spotlights, and suns
  float dist;    // plane location along normal

  qboolean linearLight;
  int photons;
  int style;
  vec3_t color;
  float radiusByDist; // for spotlights

  qboolean twosided; // fog lights both sides

  winding_t *w;
  vec3_t emitColor; // full out-of-gamut value
} light_t;

float CalculateFalloff(float dot);

extern float lightscale;
extern float ambient;
extern float maxlight;
extern float direct_scale;
extern float entity_scale;

extern qboolean debugLightmaps;
extern qboolean debugLightmapsAlpha;
extern qboolean oldTrace;
extern qboolean bruteTrace;
extern qboolean embree;

//===============================================================

// light_trace.c

// a facet is a subdivided element of a patch aproximation or model
typedef struct cFacet_s {
  float surface[4];
  int numBoundaries;      // either 3 or 4, anything less is degenerate
  float boundaries[4][4]; // positive is outside the bounds

  vec3_t points[4]; // needed for area light subdivision

  float textureMatrix[2][4]; // compute texture coordinates at point of impact
                             // for translucency
  int surfaceNum;
} cFacet_t;

typedef struct {
  vec3_t mins, maxs;
  vec3_t origin;
  float radius;

  qboolean patch;

  int numFacets;
  cFacet_t *facets;

  shaderInfo_t *shader; // for translucency
  int surfaceNum;
} surfaceTest_t;

extern surfaceTest_t *surfaceTest[MAX_MAP_DRAW_SURFS];

typedef struct {
  qboolean passSolid;
  vec3_t filter; // starts out 1.0, 1.0, 1.0, may be reduced if
                 // transparent surfaces are crossed

  vec3_t hit;        // the impact point of a completely opaque surface
  float hitFraction; // 0 = at start, 1.0 = at end
} trace_t;

typedef struct {
  vec3_t start, end;
  int numOpenLeafs;
  int openLeafNumbers[MAX_MAP_LEAFS];
  trace_t *trace;
  int patchshadows;
  qboolean forceFrontOnly;
  int ignoreSurface;
} traceWork_t;

void InitTrace(void);
void InitTracingGeometry(void);
qboolean Trace_SampleFilter(struct shaderInfo_s *si, float s, float t, vec3_t filter);

void TraceLine(const vec3_t start, const vec3_t stop, trace_t *trace,
               qboolean testAll, traceWork_t *tw);
qboolean PointInSolid(vec3_t start);

//===============================================================

//===============================================================

typedef struct {
  int textureNum;
  int x, y, width, height;

  // for patches
  qboolean patch;
  mesh32_t mesh;

  // for faces
  vec3_t origin;
  vec3_t vecs[3];
} lightmap_t;

extern float areaScale;
extern float pointScale;
extern qboolean notrace;
extern qboolean extra;
extern qboolean lightmapBorder;
extern int novertexlighting;
extern int nogridlighting;

extern qboolean falloffOverridden;
extern falloff_t overrideFalloff;
extern qboolean lightmapsRGBOverridden;
extern qboolean deluxeMapOverridden;

// Program flow
void LightMain(void);
void VisualizeLightmapAllocation(void);
void CountLightmaps(void);
