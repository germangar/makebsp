#ifndef GLOBALS_H
#define GLOBALS_H

#include "../common/qtypes.h"

typedef enum { FALLOFF_LAMBERT, FALLOFF_HALFLAMBERT, FALLOFF_QUADRATIC, FALLOFF_DOUBLEQUADRATIC, FALLOFF_UNREAL } falloff_t;

#define FALLOFF_LAMBERT_SOFTBIAS 0.15f
#define FALLOFF_HALFLAMBERT_SOFTBIAS 0.25f
#define FALLOFF_QUADRATIC_SOFTBIAS 0.15f
#define FALLOFF_DOUBLEQUADRATIC_SOFTBIAS 0.15f
#define FALLOFF_UNREAL_SOFTBIAS 0.15f

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
	const char	*gamePath;		/* default base game data dir */
	const char	*bspIdent;		/* bsp file ident (e.g. IBSP, RBSP, FBSP) */
	int			bspVersion;		/* bsp file version (e.g. 46, 47, 1) */
	int			lumpCount;		/* number of lumps in bsp file */

	// Limits
	int         maxLMSurfaceVerts;
	int         maxSurfaceVerts;
	int         maxSurfaceIndexes;
	int         lightmapSize;
	int         writeLightmapSize;

	// Lighting flags
	int         defaultSampleSize;
    hdrFormat_t hdr;
	qboolean	lightmapsRGB;
	qboolean	lightgridRGB;
	qboolean	texturesRGB;
	qboolean	colorsRGB;
	int         radiosityPasses;
	float       radiosityIntensity;
	float       radiosityColorRatio;
	falloff_t   falloff;
	falloff_t   sunFalloff;
	qboolean    deluxeMap;
	qboolean    forceUVGen;
	qboolean    snapUVs;
	int         antialiasingPasses;
	int         defaultSmoothPasses;
	float       defaultSmoothRadius;
    tonemap_t   exposureFilter;
} game_t;

extern float falloffSoftBias;
extern float sunSoftBias;

#define MAX_GAMES 128
extern int numGames;
extern game_t *g_game;
extern game_t games[MAX_GAMES];

#define LIGHTMAP_WIDTH  (g_game->lightmapSize)
#define LIGHTMAP_HEIGHT (g_game->lightmapSize)

#endif
