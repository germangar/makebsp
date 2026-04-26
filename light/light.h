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
#include <CL/cl.h>

extern RTCDevice g_device;
extern RTCScene g_scene;

extern cl_platform_id g_clPlatform;
extern cl_device_id g_clDevice;
extern cl_context g_clContext;
extern cl_command_queue g_clQueue;

extern qboolean useOpenCL;

void InitOpenCL(void);
void ShutdownOpenCL(void);
cl_program BuildOpenCLProgram(const char *filename, const char *options);

#define ALPHA_SURF_WORLD 1
#define ALPHA_TRISOUP 2

#define SAMPLE_NUDGE 1.0f
#define SELF_SHADOW_EPSILON 1.25f

/* These values have been manually calibrated. 
If the distance falloff calculation changes they would need to be recalibrated */
#define MIN_LIGHT_ADD 0.1f
#define MIN_RADIOSITY_EMITTER_ADD 0.0002f
#define MIN_RADIOSITY_EMITTER_GROUP_ADD MIN_RADIOSITY_EMITTER_ADD

#define UPSCALE_FACTOR 2
#define GUTTER 1
typedef enum { emit_point, emit_area, emit_spotlight, emit_sun } emittype_t;

extern tonemap_t tonemapMode;

typedef struct {
  vec3_t dir;
  vec3_t color;
} contribution_t;

#define MAX_LIGHT_EDGES 8

/*
================
CalculateLightReach

Calculates the distance at which a light's contribution falls below threshold.
If area > 0, it uses the area-light formula (Lambertian).
If area <= 0, it uses the point-light formula.
================
*/static inline float CalculateLightReach(float area, float intensity, float threshold, qboolean linearLight) {
  if (intensity <= 0 || threshold <= 0) {
    return 0.0f;
  }
  if (linearLight) {
    // Linear light math: add = intensity * 0.000125f - dist
    float reach = (intensity * 0.000125f) - threshold;
    return (reach > 0.0f) ? reach : 0.0f;
  }
  if (area > 0) {
    return (float)sqrt((area * intensity) / threshold);
  } else {
    return (float)sqrt(intensity / threshold);
  }
}

/*
================
CalculateRadiosityLightReach

Specialized version for Radiosity emitters that accounts for the 1/PI factor 
in the physical form factor formula used in radiosity.c.
================
*/
static inline float CalculateRadiosityLightReach(float area, float intensity, float threshold) {
  if (intensity <= 0 || threshold <= 0) {
    return 0.0f;
  }
  if (area > 0) {
    return (float)sqrt((area * intensity) / (M_PI * threshold));
  } else {
    return (float)sqrt(intensity / (M_PI * threshold));
  }
}


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
  float area;       // pre-calculated winding area (for seam fix)
  vec3_t emitColor; // full out-of-gamut value
  float reach;      // pre-calculated max distance
} light_t;

typedef struct {
  dbrush_t *b;
  vec3_t bounds[2];
} skyBrush_t;

extern vec3_t sunDirection, sunLight, ambientColor;
extern int numSkyBrushes;
extern skyBrush_t skyBrushes[];
extern vec3_t gridMins, gridSize;
extern int gridBounds[3], numGridPoints;

float CalculateFalloff(float dot);

extern float lightscale;
extern float ambient;
extern float maxlight;
extern float direct_scale;
extern float entity_scale;

extern qboolean debugLightmaps;
extern qboolean debugLightmapsAlpha;
extern qboolean bruteTrace;
extern qboolean embree;
extern qboolean rad_voxel;

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
qboolean PointInTrisoup(vec3_t origin, vec3_t normal);
struct MyRayQueryContext {
  struct RTCRayQueryContext context;
  traceWork_t *tw;
  int patchshadows;
};

//===============================================================
extern vec3_t surfaceOrigin[MAX_MAP_DRAW_SURFS];
extern int entitySurface[MAX_MAP_DRAW_SURFS];
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
extern qboolean upscale;
extern qboolean lightmapBorder;
extern int novertexlighting;
extern int nogridlighting;

extern qboolean falloffOverridden;
extern falloff_t overrideFalloff;
extern qboolean lightmapsRGBOverridden;
extern qboolean deluxeMapOverridden;

extern float lightmapSmoothRadius;
extern int lightmapSmoothPasses;
extern int lightmapAA;

typedef enum {
    SUPERSAMPLE_NONE = 0,
    SUPERSAMPLE_MODELS = 1,
    SUPERSAMPLE_ALL = 2
} ssMode_t;

extern ssMode_t superSampleMode;
void SmoothLightmaps(float radius);
void PostProcessLightmaps(void);

// Program flow
void LightMain(int radiosityPasses);
extern light_t *lights;
extern qboolean patchshadows;
extern int *surfaceWorkOrder;
extern int c_visible, c_occluded;

int CompareSurfaces(const void *a, const void *b);
void LightWorld(void);
void TraceLtm(int num);
void TraceGrid(int num);
void LightingAtSample(const vec3_t origin, const vec3_t normal, vec3_t color,
                      qboolean testOcclusion, qboolean forceSunLight,
                      qboolean applyColorFilter, traceWork_t *tw);
void VertexLighting(dsurface_t *ds, qboolean testOcclusion,
                    qboolean forceSunLight, float scale, traceWork_t *tw);
void CountLightmaps(void);
qboolean TriSoupSamplePoint(dsurface_t *ds, float st[2], vec3_t origin, vec3_t normal);
