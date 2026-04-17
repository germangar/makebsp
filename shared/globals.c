#include <stddef.h>
#include "globals.h"

int samplesize = 0;
char source[1024];
char name[1024];

int numGames = 2;

game_t games[MAX_GAMES] = {
	{
		"quake3",
		".",
		"IBSP",
		46,
		17,
		64,
		999,
		6000,
		128,
		// Global Map Limits removed
		8,          // defaultSampleSize
        HDR_OFF,    // hdr
		qfalse,     // lightmapsRGB
		qfalse,     // lightgridRGB
		qfalse,     // texturesRGB
		qfalse,     // colorsRGB
		FALLOFF_LAMBERT,
		qfalse,     // deluxeMap
		6,          // defaultSmoothPasses
		1.0f,       // defaultSmoothRadius
        TONEMAP_LINEAR // exposureFilter
	},
	{
		"qfusion",
		".",
		"FBSP",
		1,
		18,
		65535,
		65535,
		393210,
		512,
		// Global Map Limits removed
		4,          // defaultSampleSize
        HDR_8BIT,   // hdr
		qtrue,      // lightmapsRGB
		qfalse,     // lightgridRGB
		qtrue,      // texturesRGB
		qtrue,      // colorsRGB
		FALLOFF_LAMBERT,
		qtrue,       // deluxeMap
		6,           // defaultSmoothPasses
		1.0f,        // defaultSmoothRadius
        TONEMAP_REINHARD // exposureFilter
	}
};

game_t *g_game = &games[1];
