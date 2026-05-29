#ifndef GLOBALS_H
#define GLOBALS_H

#include "../common/mathlib.h"
#include "../common/qtypes.h"

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
    int hasVertexColor;
    vec3_t vertexColor;
    float superSampleRadius;
    qboolean isHalo;
    int upscale;
    float cutoff;
    float fadeout;
    qboolean hasAttenuationOverride;
    attenuationModel_t attenuationModel;
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

extern int samplesize;
extern char source[1024];
extern char name[1024];

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
	int         writeLightmapSize;
    float       minLightAdd;
    float       fadeout;
    float       backSplashSpot;
    float       backSplashSurface;

	// Lighting flags
	int         defaultSampleSize;
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
	shadingModel_t   shadingModel;
	shadingModel_t   sunShadingModel;
	attenuationModel_t attenuationModel;
	qboolean    deluxeMap;
	float       deluxeMinAngle;
	float       deluxeAmbientExaggerate;
	float       deluxeRadiosityExaggerate;

	float       superSampleRadius;
	qboolean    upscale;
	int         antialiasingPasses;
	int         defaultSmoothPasses;
	float       defaultSmoothRadius;
    tonemap_t   exposureFilter;
    qboolean    enforceSampleSize;

	const char	*flareShader;
	const char	*haloShader;
} game_t;

extern float shadingModelSoftBias;
extern float sunSoftBias;
extern vec3_t blockSize;
extern qboolean g_lowmem;

#define MAX_GAMES 128
extern int numGames;
extern game_t *game;
extern game_t gameTemplates[MAX_GAMES];

game_t *InitGame(int argc, char **argv);
void ClearCacheDirectory(void);

#define LIGHTMAP_WIDTH  (game->lightmapSize)
#define LIGHTMAP_HEIGHT (game->lightmapSize)

#endif
