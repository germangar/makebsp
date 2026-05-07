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
		"qfusion",
		".",
		"FBSP",
		1,
		18,
		65535,      // maxLMSurfaceVerts
		65535,      // maxSurfaceVerts
		393210,     // maxSurfaceIndexes
		512,
		512,        // writeLightmapSize
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
		qtrue,      // deluxeMap
		qtrue,      // forceUVGen
		qtrue,      // snapUVs
		0,          // antialiasingPasses
		4,          // defaultSmoothPasses
		0.25f,      // defaultSmoothRadius
        TONEMAP_REINHARD, // exposureFilter
        qtrue       // enforceSampleSize
	},
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
        qtrue       // enforceSampleSize
	}
};


game_t *g_game = &games[0];
