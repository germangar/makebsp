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
		4,          // radiosityPasses
		1.0f,       // radiosityIntensity
		0.75f,      // radiosityColorRatio
		FALLOFF_LAMBERT, // falloff
		FALLOFF_SOFTLAMBERT, // sunFalloff
		qfalse,     // deluxeMap
		qtrue,      // forceUVGen
		qtrue,      // snapUVs
		0,          // antialiasingPasses
		4,          // defaultSmoothPasses
		0.35f,      // defaultSmoothRadius
        TONEMAP_LINEAR, // exposureFilter
		0.15f       // softLambertBias
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
		4,          // radiosityPasses
		1.0f,       // radiosityIntensity
		1.0f,       // radiosityColorRatio
		FALLOFF_LAMBERT, // falloff
		FALLOFF_SOFTLAMBERT, // sunFalloff
		qtrue,       // deluxeMap
		qtrue,       // forceUVGen
		qtrue,       // snapUVs
		2,           // antialiasingPasses
		0,           // defaultSmoothPasses
		0.5f,        // defaultSmoothRadius
        TONEMAP_REINHARD, // exposureFilter
		0.15f        // softLambertBias
	}
};

game_t *g_game = &games[1];
