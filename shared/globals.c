#include <stddef.h>
#include "globals.h"

int samplesize = 0;
char source[1024];
char name[1024];

int numGames = 2;
float falloffSoftBias = FALLOFF_LAMBERT_SOFTBIAS;
float sunSoftBias = FALLOFF_LAMBERT_SOFTBIAS;

vec3_t blockSize = {1024, 1024, 1024};

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
		0,          // radiosityPasses
		1.0f,       // radiosityIntensity
		0.1f,       // radiosityColorRatio
		FALLOFF_LAMBERT, // falloff
		FALLOFF_LAMBERT, // sunFalloff
		qfalse,     // deluxeMap
		qfalse,     // forceUVGen
		qfalse,     // snapUVs
		0,          // antialiasingPasses
		0,          // defaultSmoothPasses
		1.0f,       // defaultSmoothRadius
        TONEMAP_LINEAR, // exposureFilter
        qtrue       // enforceSampleSize
	},
	{
		"qfusion",
		".",
		"FBSP",
		1,
		21,
		64,
		999,
		6000,
		128,
		128,        // writeLightmapSize
		// Global Map Limits removed
		8,          // defaultSampleSize
        HDR_OFF,    // hdr
		1.0f,       // hdr8BitScale
		qtrue,      // lightmapsRGB
		qtrue,      // lightgridRGB
		qtrue,      // texturesRGB
		qtrue,      // colorsRGB
		0,          // radiosityPasses
		1.0f,       // radiosityIntensity
		0.1f,       // radiosityColorRatio
		FALLOFF_LAMBERT, // falloff
		FALLOFF_LAMBERT, // sunFalloff
		qfalse,     // deluxeMap
		qfalse,     // forceUVGen
		qfalse,     // snapUVs
		0,          // antialiasingPasses
		0,          // defaultSmoothPasses
		1.0f,       // defaultSmoothRadius
        TONEMAP_LINEAR, // exposureFilter
        qtrue       // enforceSampleSize
	}
};


game_t *g_game = &games[1];
