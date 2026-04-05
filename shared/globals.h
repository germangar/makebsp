#ifndef GLOBALS_H
#define GLOBALS_H

#include "../common/qtypes.h"

typedef enum { FALLOFF_LAMBERT, FALLOFF_HALFLAMBERT, FALLOFF_QUADRATIC, FALLOFF_DOUBLEQUADRATIC, FALLOFF_UNREAL, FALLOFF_WRAPPED } falloff_t;

typedef enum {
    HDR_OFF = 0,
    HDR_8BIT = 1,
    HDR_16BIT = 2,
    HDR_32BIT = 3
} hdrFormat_t;

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

	// Lighting flags
	int         defaultSampleSize;
    hdrFormat_t hdr;
	qboolean	lightmapsRGB;
	qboolean	lightgridRGB;
	qboolean	texturesRGB;
	qboolean	colorsRGB;
	falloff_t   falloff;
	qboolean    deluxeMap;
	int         defaultSmoothPasses;
	float       defaultSmoothRadius;
} game_t;

#define MAX_GAMES 128
extern int numGames;
extern game_t *g_game;
extern game_t games[MAX_GAMES];

#define LIGHTMAP_WIDTH  (g_game->lightmapSize)
#define LIGHTMAP_HEIGHT (g_game->lightmapSize)

#endif
