#include <stddef.h>
#include "globals.h"

int samplesize = 0;
char source[1024];
char name[1024];

int numGames = 2;
float falloffSoftBias = FALLOFF_LAMBERT_SOFTBIAS;
float sunSoftBias = FALLOFF_LAMBERT_SOFTBIAS;

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
		128,        // writeLightmapSize
		// Global Map Limits removed
		8,          // defaultSampleSize
        HDR_OFF,    // hdr
		1.0f,       // hdr8BitScale
		qfalse,     // lightmapsRGB
		qfalse,     // lightgridRGB
		qfalse,     // texturesRGB
		qfalse,     // colorsRGB
		4,          // radiosityPasses
		1.0f,       // radiosityIntensity
		0.75f,      // radiosityColorRatio
		FALLOFF_LAMBERT, // falloff
		FALLOFF_LAMBERT, // sunFalloff
		qfalse,     // deluxeMap
		qtrue,      // forceUVGen
		qtrue,      // snapUVs
		0,          // antialiasingPasses
		4,          // defaultSmoothPasses
		0.35f,      // defaultSmoothRadius
		TONEMAP_LINEAR, // exposureFilter
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
		512,        // writeLightmapSize
		// Global Map Limits removed
		4,          // defaultSampleSize
        HDR_8BIT,   // hdr
		3.0f,       // hdr8BitScale
		qtrue,      // lightmapsRGB
		qfalse,     // lightgridRGB
		qtrue,      // texturesRGB
		qtrue,      // colorsRGB
		4,          // radiosityPasses
		1.0f,       // radiosityIntensity
		1.0f,       // radiosityColorRatio
		FALLOFF_LAMBERT, // falloff
		FALLOFF_LAMBERT, // sunFalloff
		qtrue,       // deluxeMap
		qtrue,       // forceUVGen
		qtrue,       // snapUVs
		0,           // antialiasingPasses
		4,           // defaultSmoothPasses
		0.25f,       // defaultSmoothRadius
		TONEMAP_REINHARD, // exposureFilter
	}
};

game_t *g_game = &games[1];
