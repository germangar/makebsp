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
#include "lightdata.h"
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
extern qboolean openclEnabled;
extern qboolean g_fast;
extern float   ambient_color_scale;

extern qboolean    directonly;
extern qboolean    radiosityonly;
extern qboolean    ambientonly;
extern qboolean    upscale;
extern float       deluxeAmbientExaggerate;
extern float       deluxeRadiosityExaggerate;

/*
 * GpuPlanarSurface — CPU-side mirror of the struct in lm_common.cl.
 * Must stay layout-identical (64 bytes, all float/int).
 */
typedef struct
{
	float originX, originY, originZ;
	float vecs0X, vecs0Y, vecs0Z;
	float vecs1X, vecs1Y, vecs1Z;
	float invMagSq0, invMagSq1;
	int width, height;
	int lmNum;
	int lmOffX, lmOffY;
} GpuPlanarSurface_t;

/*
 * GpuLightmapState — persistent GPU buffers shared by all post-processing
 * filter kernels (AA, Smooth, and any future filters).
 * Lifetime: BuildPlanarSurfaceIndex() → GpuLightmapState_Upload() →
 *           [kernel dispatches] → GpuLightmapState_Download() →
 *           GpuLightmapState_Free() → FreePlanarSurfaceIndex()
 */
typedef struct
{
	/* Ping-pong atlas buffers (float RGB).                        */
	/* Input for each pass is atlasA when pingIsA==1, atlasB when 0 */
	cl_mem atlasA;
	cl_mem atlasB;

	/* Ping-pong deluxe direction buffers (float RGB, same layout) */
	cl_mem deluxeA;
	cl_mem deluxeB;

	/* Ping-pong surface normal buffers (float RGB, same layout)   */
	cl_mem normalA;
	cl_mem normalB;

	/* Alpha validity mask (uchar, 0 = invalid texel)              */
	cl_mem maskBuf;

	/* Planar surface metadata (GpuPlanarSurface[])                */
	cl_mem surfacesBuf;

	/* Partner adjacency in CSR layout                             */
	cl_mem partnerData;	   /* int[] flat partner indices          */
	cl_mem partnerOffsets; /* int[numPlanarSurfaces+1]            */

	/* Per-texel lookup tables (all indexed by flat atlas pixel)   */
	cl_mem validList;	   /* int[numValid] — valid texel indices */
	cl_mem radiiBuf;	   /* float[numPlanarSurfaces] */
	cl_mem pixelToSurface; /* int[totalPixels] -> surface index  */
	cl_mem pixelToX;	   /* int[totalPixels] -> local x        */
	cl_mem pixelToY;	   /* int[totalPixels] -> local y        */

	int numPlanarSurfaces;
	int numValid;
	int totalAtlasPixels;
	int pingIsA; /* 1 = atlasA is current output       */
	int upscale; /* 1 = native, 2 = 2x resolution, etc. */
} GpuLightmapState;

extern GpuLightmapState g_gpuLM;

void InitOpenCL(void);
void ShutdownOpenCL(void);
cl_program BuildOpenCLProgram(const char *filename, const char *options);
cl_program BuildOpenCLProgramWithCommon(const char *filename, const char *options);
void GpuLightmapState_Upload(void);
void GpuLightmapState_Download(void);
void GpuLightmapState_Free(void);

#define SAMPLE_NUDGE 0.0625f
#define SPOTLIGHT_SOFTNESS_RANGE 128.0f

/* These values have been manually calibrated.
If the distance falloff calculation changes they would need to be recalibrated */
#define MIN_LIGHT_ADD game->minLightAdd
#define MIN_RADIOSITY_EMITTER_ADD 0.0002f
#define MIN_RADIOSITY_EMITTER_GROUP_ADD MIN_RADIOSITY_EMITTER_ADD
#define MIN_DELUXE_ENERGY 0.001f

#define UPSCALE_FACTOR 2
#define GUTTER 1
typedef enum
{
	emit_point,
	emit_area,
	emit_spotlight,
	emit_sun
} emittype_t;

#define DEFAULT_ATTN_OFFSET 16.0f



/*
================
CalculateLightReach

Calculates the distance at which a light's contribution falls below threshold.
If area > 0, it uses the area-light formula (Lambertian).
If area <= 0, it uses the point-light formula.
================
*/
static inline float CalculateLightReach(float area, float intensity, float threshold, float offset, attenuationModel_t model)
{
	if (intensity <= 0 || threshold <= 0)
	{
		return 0.0f;
	}
	
    float reach = 0.0f;
    switch (model)
    {
        case ATTENUATION_UNREAL:
        case ATTENUATION_SMOOTHSTEP:
        case ATTENUATION_INVERSE_SQUARE:
        case ATTENUATION_INVERSE_SQUARE_PI:
            if (area > 0)
                reach = (float)sqrt((area * intensity) / threshold);
            else
                reach = (float)sqrt(intensity / threshold);
            break;
        case ATTENUATION_INVERSE:
            // reach is where energy falls to min_light_add: I/d = threshold -> d = I/threshold
            if (area > 0)
                reach = (area * intensity) / threshold;
            else
                reach = intensity / threshold;
            break;
        case ATTENUATION_LINEAR:
            // reach is where formula hits zero: I*0.000125 - d = 0 -> d = I*0.000125
            // subtract threshold as a small guard
            if (area > 0)
                reach = (area * intensity * 0.000125f) - threshold;
            else
                reach = (intensity * 0.000125f) - threshold;
            break;
    }
    
    reach -= offset;
    return (reach > 0.0f) ? reach : 0.0f;
}

/*
================
CalculateRadiosityLightReach

Specialized version for Radiosity emitters that accounts for the 1/PI factor
in the physical form factor formula used in radiosity.c.
================
*/
static inline float CalculateRadiosityLightReach(float area, float intensity, float threshold)
{
	if (intensity <= 0 || threshold <= 0)
	{
		return 0.0f;
	}
	if (area > 0)
	{
		return (float)sqrt((area * intensity) / (M_PI * threshold));
	}
	else
	{
		return (float)sqrt(intensity / (M_PI * threshold));
	}
}

typedef struct light_s
{
	struct light_s *next;
	emittype_t type;
	struct shaderInfo_s *si;

	vec3_t origin;
	vec3_t normal; // for surfaces, spotlights, and suns
	float dist;	   // plane location along normal

	attenuationModel_t attenuationModel;
	int photons;
	int style;
	vec3_t color;
	float radiusByDist; // for spotlights
	float coneSoftness; // scalar for edge transition
	float attnSoftnessRange; // distance-based fade range for attenuation
	float min_light_add; // specific cutoff energy limit for this light
	float fadeout; // percentage of reach to fade (0.0 to 1.0)
	float prestep; // singularity offset (distance)
	int familyCount; // Number of subdivided lights in this light's family

	qboolean twosided; // fog lights both sides
	qboolean noDeluxeInfluence;
	float gridAmbientScale; // scales light contribution to grid ambient component
	float gridDirectScale; // scales light contribution to grid direct component

	winding_t *w;
	float area;		  // pre-calculated winding area (for seam fix)
	vec3_t emitColor; // full out-of-gamut value
	float reach;	  // pre-calculated max distance
} light_t;

/*
================
CalculateAttenuation

Calculates distance-based energy decay.
================
*/
static inline float CalculateAttenuation(const light_t *light, float dist, attenuationModel_t model, float offset)
{
    float energy = 0.0f;
    float d = dist + offset;
    
    switch (model) {
        case ATTENUATION_INVERSE_SQUARE:
            energy = light->photons / (d * d);

            // The cut happens unconditionally by energy (fast & precise)
            if (energy <= light->min_light_add)
            {
                return 0.0f;
            }

            if (light->attnSoftnessRange > 0.0f)
            {
                // Then from the energy limit, we soften backwards purely by physical distance
                float fadeStartDist = light->reach - light->attnSoftnessRange;
                if (dist > fadeStartDist)
                {
                    float fadeScale = (light->reach - dist) / light->attnSoftnessRange;
                    energy *= fadeScale;
                }
            }
            break;
        case ATTENUATION_UNREAL:
        {
            float distRatio = dist / light->reach;
            if (distRatio >= 1.0f) return 0.0f;
            float distRatio4 = distRatio * distRatio * distRatio * distRatio;
            float window = 1.0f - distRatio4;
            if (window < 0.0f) window = 0.0f;
            window = window * window;
            energy = (light->photons / (d * d)) * window;
            break;
        }
        case ATTENUATION_SMOOTHSTEP:
        {
            float distRatio = dist / light->reach;
            if (distRatio >= 1.0f) return 0.0f;
            float window = 1.0f - (distRatio * distRatio);
            energy = light->photons * (window * window) / (offset * offset);
            break;
        }
        case ATTENUATION_INVERSE_SQUARE_PI:
            energy = light->photons / (M_PI * d * d);
            break;
        case ATTENUATION_INVERSE:
            energy = light->photons / d;

            if (energy <= light->min_light_add)
            {
                return 0.0f;
            }

            if (light->attnSoftnessRange > 0.0f)
            {
                float fadeStartDist = light->reach - light->attnSoftnessRange;
                if (dist > fadeStartDist)
                {
                    float fadeScale = (light->reach - dist) / light->attnSoftnessRange;
                    energy *= fadeScale;
                }
            }
            break;
        case ATTENUATION_LINEAR:
            energy = (light->photons * 0.000125f) - d;
            // no cutoff check needed: formula naturally reaches zero at finite distance
            break;
    }
    
    return (energy > 0.0f) ? energy : 0.0f;
}

typedef struct
{
	dbrush_t *b;
	vec3_t bounds[2];
} skyBrush_t;

extern vec3_t sunDirection, sunLight, ambientColor;
extern qboolean hasSun;
extern vec3_t skyColor, groundColor;    // hemisphere ambient colors
extern float  *maoAmbient;             // [numGridPoints*3] pre-baked ambient RGB
extern float  *maoDir;                 // [numGridPoints*3] pre-baked ambient direction
extern int     ambient_grid_samples;    // rays per grid point (default 48)
extern int     ambient_samples;         // rays per lightmap texel (default 32)
extern float   ambient_testradius;      // max ray length in world units (default 512)
extern float   ambient_gatheradius;   // radius for gathering from irradiance probes (default 256)
extern qboolean ambient_enabled;         // true if sky/ground color is non-zero
extern int numSkyBrushes;
extern int *surfaceWorkOrder;
extern int numLights;
extern skyBrush_t *skyBrushes;
extern vec3_t gridMins, gridSize;
extern int gridBounds[3], numGridPoints;

float CalculateShadingModel(float dot);

extern float *lightFloats;
extern float *deluxeFloats;
extern int *lightSurfaceIndex;
extern byte *lightAlphaMask;
extern byte *unreachableMask;

extern qboolean debugLightmaps;
extern qboolean debugLightmapsAlpha;

extern vec3_t *texelOrigins;
extern vec3_t *texelNormals;

//===============================================================

// light.c
typedef struct
{
	// local data
	vec3_t origin;	  // Bounding sphere center
	float radius;	  // Bounding sphere radius
	float maxReach;	  // Radiosity culling reach
	int emitterStart; // Radiosity emitter indexing
	int emitterCount;
	vec3_t entityOrigin; // Offset for inline models
	qboolean isEntity;	 // Entity membership flag
	qboolean surfaceIsPlanar;
	mesh_t *patchMesh; // Cached geometry for MST_PATCH
    int radInterval;

	// sidecar data
	float smoothingRadius;
    shaderInfo_t *si_override;

    // vertex color & alpha override
    qboolean overrideVertexColor;
    vec3_t vertexColor;
    qboolean overrideVertexAlpha;
    float vertexAlpha;
    float superSampleRadius;
    int upscale;
    qboolean castShadows;
    float sampleSize;
    char smoothgroup[32];
} localSurface_t;
extern localSurface_t *localSurfaces;
extern qboolean *brushCastsShadow;
void BuildLocalSurfaces(void);

typedef struct
{
	qboolean passSolid;
	vec3_t filter; // starts out 1.0, 1.0, 1.0, may be reduced if
				   // transparent surfaces are crossed

	vec3_t hit;		   // the impact point of a completely opaque surface
	float hitFraction; // 0 = at start, 1.0 = at end
} trace_t;

typedef struct
{
	vec3_t start, end;
	int numOpenLeafs;

	trace_t *trace;
	int patchshadows;
	qboolean forceFrontOnly;
	qboolean isLightgrid;
	int ignoreSurface;
} traceWork_t;

void InitTrace(void);
void InitTracingGeometry(void);
qboolean Trace_SampleFilter(struct shaderInfo_s *si, float s, float t, vec3_t filter, qboolean isLightFilter);
qboolean PointInTrisoup(vec3_t origin, vec3_t normal);
qboolean BoxInSolid(vec3_t origin, float margin, qboolean structuralonly);

void TraceLine(const vec3_t start, const vec3_t stop, trace_t *trace,
			   qboolean testAll, traceWork_t *tw);
qboolean GatherGridAmbientBleed(vec3_t origin, vec3_t envColor, traceWork_t *tw);
qboolean PointInSolid(vec3_t start);
qboolean TriSoupSamplePoint(dsurface_t *ds, float st[2], vec3_t origin, vec3_t normal, vec3_t outCentroid);
qboolean PatchSamplePoint(mesh_t *mesh, float st[2], vec3_t origin, vec3_t normal, vec3_t outCentroid);
mesh_t *SubdividePatchForLighting(dsurface_t *ds, float ssize);
struct MyRayQueryContext
{
	struct RTCRayQueryContext context;
	traceWork_t *tw;
	int patchshadows;
};

//===============================================================

//===============================================================

typedef struct
{
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
extern float directScale;
extern qboolean nodirect;
extern qboolean noambient;
extern qboolean deluxeSort;

extern qboolean lightmapBorder;
extern int novertexlighting;
extern int nogridlighting;


void SmoothLightmaps(float radius);
void PostProcessLightmaps(void);

// Program flow
void LightMain(void);
extern light_t *lights;
extern qboolean patchshadows;

int CompareSurfaces(const void *a, const void *b);
extern long long numTotalLuxels;
extern float lightgridMaxDisplayIntensity;
void LightWorld(void);
void RunMAOPass(void);
void LightAmbient(void);
void DilateDeluxeDirections(void);
void PrecacheTexelGeometry(void);
void TraceLights(int num);
void TraceGrid(int num);
void LightingAtSample(const vec3_t origin, const vec3_t normal, vec3_t color,
                      vec3_t dir, vec3_t energy,
                      qboolean testOcclusion, qboolean forceSunLight,
                      qboolean applyColorFilter, light_t **lightList,
                      int numLights, traceWork_t *tw, float deluxeMinAngle);
void VertexLighting(dsurface_t *ds, qboolean testOcclusion,
					qboolean forceSunLight, float scale, light_t **lightList,
					int numLights, traceWork_t *tw);
void CountLightmaps(void);
qboolean TriSoupSamplePoint(dsurface_t *ds, float st[2], vec3_t origin, vec3_t normal, vec3_t outCentroid);
