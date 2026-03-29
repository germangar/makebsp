#include <stddef.h>
#include "globals.h"

int samplesize = 0;
char source[1024];
char name[1024];

int numGames = 2;

game_t games[MAX_GAMES] = {
	{
		"quake3",
		"baseq3",
		"IBSP",
		46,
		17,
		64,
		999,
		6000,
		128,
		// Global Map Limits
		0x100000,	// maxMapDrawVerts
		0x40000,	// maxMapDrawSurfs
		0x100000,	// maxMapNodes
		0x100000,	// maxMapLeafs
		0x100000,	// maxMapPlanes
		0x100000,	// maxMapBrushes
		0x100000,	// maxMapDrawIndexes
		8,          // defaultSampleSize
		qfalse,     // lightmapsRGB
		qfalse,     // texturesRGB
		qfalse,     // colorsRGB
		FALLOFF_LAMBERT,
		qfalse,      // deluxeMap
        6,
        1
	},
	{
		"qfusion",
		"base",
		"FBSP",
		1,
		18,
		65535,
		65535,
		393210,
		512,
		// Global Map Limits (QFusion)
		0x100000,	// maxMapDrawVerts
		0x40000,	// maxMapDrawSurfs
		0x100000,	// maxMapNodes
		0x100000,	// maxMapLeafs
		0x100000,	// maxMapPlanes
		0x100000,	// maxMapBrushes
		0x100000,	// maxMapDrawIndexes
		4,          // defaultSampleSize
		qtrue,      // lightmapsRGB
		qtrue,      // texturesRGB
		qtrue,      // colorsRGB
		FALLOFF_HALFLAMBERT,
		qtrue,       // deluxeMap
        6,
        1
	}
};

game_t *g_game = &games[0];
