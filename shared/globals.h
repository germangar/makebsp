#ifndef GLOBALS_H
#define GLOBALS_H

#include "../common/mathlib.h"
#include "../common/qtypes.h"
#include "../common/qfiles.h"

typedef enum {
    ATTENUATION_INVERSE_SQUARE,
    ATTENUATION_INVERSE_SQUARE_PI,
    ATTENUATION_INVERSE,
    ATTENUATION_LINEAR,
    ATTENUATION_UNREAL,
    ATTENUATION_SMOOTHSTEP
} attenuationModel_t;

typedef struct {
    float smoothingRadius;
    float lightValue;
    vec3_t lightColor;
    float backsplashFraction;
    float lightSubdivide;
    int overrideVertexColor;
    vec3_t vertexColor;
    float superSampleRadius;
    qboolean isHalo;
    int upscale;
    float cutoff;
    float fadeout;
    qboolean hasAttenuationOverride;
    attenuationModel_t attenuationModel;
    int noDeluxeInfluence;
    int noDeluxeInfluenceBacksplash;
    int castShadows;
    qboolean isPlanar;
    float sampleSize;
} extraSurface_t;

typedef enum { SHADING_MODEL_LAMBERT, SHADING_MODEL_HALFLAMBERT, SHADING_MODEL_QUADRATIC, SHADING_MODEL_DOUBLEQUADRATIC, SHADING_MODEL_UNREAL } shadingModel_t;

#define SHADING_MODEL_LAMBERT_SOFTBIAS 0.15f
#define SHADING_MODEL_HALFLAMBERT_SOFTBIAS 0.25f
#define SHADING_MODEL_QUADRATIC_SOFTBIAS 0.15f
#define SHADING_MODEL_DOUBLEQUADRATIC_SOFTBIAS 0.15f
#define SHADING_MODEL_UNREAL_SOFTBIAS 0.15f

typedef enum {
    HDR_OFF = 0,
    HDR_8BIT = 1,
    HDR_16BIT = 2,
    HDR_32BIT = 3
} hdrFormat_t;


typedef enum { TONEMAP_LINEAR, TONEMAP_SOFTKNEE, TONEMAP_REINHARD, TONEMAP_FILMIC } tonemap_t;
typedef enum { SATRAMP_OFF, SATRAMP_FILMIC, SATRAMP_POWER, SATRAMP_HALF_POWER, SATRAMP_MIDTONE } satRamp_t;

extern char source[1024];
extern char name[1024];

#define MAX_CUSTOM_SURFACEPARMS 64

typedef struct {
    char name[64];
    int clearSolid;
    int surfaceFlags;
    int contents;
} customSurfaceParm_t;

typedef struct {
	const char	*arg;			/* -game x */
	const char	*rootDir;		/* default root directory */
	const char	*userDir;		/* default user directory (overrides rootDir for loading) */
	const char	*gameDir;		/* default base game data dir */
	const char	*bspIdent;		/* bsp file ident (e.g. IBSP, RBSP, FBSP) */
	int			bspVersion;		/* bsp file version (e.g. 46, 47, 1) */
	int			lumpCount;		/* number of lumps in bsp file */

	// Limits
	int         maxLMSurfaceVerts;
	int         maxSurfaceVerts;
	int         maxSurfaceIndexes;
	int         lightmapSize;
	qboolean    externalLightmaps;

	// Lighting flags
	int         defaultSampleSize;
	qboolean    chamferEdges;
	float       chamferConvexWidth;
	float       chamferConcaveWidth;
	float       decalExtrusion;
    qboolean    enforceSampleSize;
    qboolean    forceUVGen;
    hdrFormat_t hdr;
    float       hdr8BitScale;
	qboolean	lightmapsRGB;
	qboolean	lightgridRGB;
	qboolean	texturesRGB;
	qboolean	colorsRGB;
	int         radiosityPasses;
	float       radiosityIntensity;
	float       radiosityColorRatio;
    int         radiosityInterval;
    float       rad_ao_intensity;
    float       rad_ao_min;
    float       rad_ao_max;
    float       ambientTestRadius;
    float       ambientGatherRadius;
	shadingModel_t   shadingModel;
	shadingModel_t   sunShadingModel;
	attenuationModel_t attenuationModel;
    tonemap_t   exposureFilter;
    float       saturation;
    satRamp_t   saturationRamp;
    float       minLightAdd;
    float       fadeout;
    float       backSplashSpot;
    float       backSplashSurface;
	qboolean    deluxeMap;
	float       deluxeMinAngle;

	float       superSampleRadius;
	int         antialiasingPasses;
	int         defaultSmoothPasses;
	float       defaultSmoothRadius;

	const char	*flareShader;
	const char	*haloShader;

	customSurfaceParm_t customSurfaceParms[MAX_CUSTOM_SURFACEPARMS];
	int                 numCustomSurfaceParms;
} game_t;

extern float shadingModelSoftBias;
extern float sunSoftBias;
extern vec3_t blockSize;
extern qboolean g_lowmem;
extern qboolean g_debugExportLightmaps;
extern qboolean g_debugMagentaTrisoups;
extern qboolean g_debugCyanPatches;
extern qboolean g_debugGreenPlanar;
extern qboolean nodecimateplanar;

#define MAX_GAMES 128
extern int numGames;
extern game_t *game;
extern game_t gameTemplates[MAX_GAMES];

#define MAX_ACTIVE_GAMEDIRS 32
extern char activeGamedirs[MAX_ACTIVE_GAMEDIRS][MAX_QPATH];
extern int numActiveGamedirs;
void AddActiveGamedir(const char *dir);

game_t *InitGame(int argc, char **argv);
void ClearCacheDirectory(void);
void GetMapOutputDir(const char *source, char *out);

#define LIGHTMAP_WIDTH  (game->lightmapSize)
#define LIGHTMAP_HEIGHT (game->lightmapSize)

#endif
