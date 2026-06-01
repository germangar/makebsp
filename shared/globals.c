#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "globals.h"
#include "json_parser.h"
#include "../common/cmdlib.h"

int samplesize = 0;
char source[1024];
char name[1024];

int numGames = 2;
float shadingModelSoftBias = SHADING_MODEL_LAMBERT_SOFTBIAS;
float sunSoftBias = SHADING_MODEL_LAMBERT_SOFTBIAS;

vec3_t blockSize = {1024, 1024, 1024};
qboolean g_lowmem = qfalse;

game_t gameTemplates[MAX_GAMES] = {
	{
		"qfusion",
		".",        // rootDir
        "",         // userDir
#ifdef RELEASE_BUILD
        "basewf",   // gameDir
#else
        "",         // gameDir
#endif
		"FBSP",
		1,
		18,
		65535,      // maxLMSurfaceVerts
		65535,      // maxSurfaceVerts
		393210,     // maxSurfaceIndexes
		512,
		512,        // writeLightmapSize

		4,          // defaultSampleSize
        qtrue,      // enforceSampleSize
        qtrue,      // forceUVGen
        HDR_8BIT,   // hdr
		3.0f,       // hdr8BitScale
		qtrue,      // lightmapsRGB
		qfalse,     // lightgridRGB
		qtrue,      // texturesRGB
		qtrue,      // colorsRGB
		4,          // radiosityPasses
		1.0f,       // radiosityIntensity
		1.0f,       // radiosityColorRatio
        4,          // radiosityInterval
        0.5f,       // rad_ao_intensity
        0.0f,       // rad_ao_min
        32.0f,      // rad_ao_max
		SHADING_MODEL_LAMBERT, // falloff
		SHADING_MODEL_LAMBERT, // sunFalloff
		ATTENUATION_INVERSE_SQUARE, // attenuationModel
        TONEMAP_REINHARD, // exposureFilter
        0.1f,       // cutoff
        0.0f,       // fadeout
        0.1f,       // backSplashSpot
        0.0f,       // backSplashSurface
		qtrue,      // deluxeMap
		15.0f,      // deluxeMinAngle
		1.0f,       // deluxeAmbientExaggerate
		1.0f,       // deluxeRadiosityExaggerate


		0.0f,       // superSampleRadius
		qfalse,     // upscale
		0,          // antialiasingPasses
		4,          // defaultSmoothPasses
		0.35f,      // defaultSmoothRadius
		"",         // flareShader
		"halo"      // haloShader
	},
	{
		"quake3",
		".",        // rootDir
        "",         // userDir
        "baseq3",   // gameDir
		"IBSP",
		46,
		17,
		64,
		999,
		6000,
		128,
		128,        // writeLightmapSize

		8,          // defaultSampleSize
        qtrue,       // enforceSampleSize
        qtrue,       // forceUVGen
        HDR_OFF,    // hdr
		1.0f,       // hdr8BitScale
		qfalse,     // lightmapsRGB
		qfalse,     // lightgridRGB
		qfalse,     // texturesRGB
		qfalse,     // colorsRGB
		4,          // radiosityPasses
		1.0f,       // radiosityIntensity
		0.75f,      // radiosityColorRatio
        4,          // radiosityInterval
        0.5f,       // rad_ao_intensity
        0.0f,       // rad_ao_min
        32.0f,      // rad_ao_max
		SHADING_MODEL_LAMBERT, // falloff
		SHADING_MODEL_LAMBERT, // sunFalloff
		ATTENUATION_INVERSE_SQUARE, // attenuationModel
        TONEMAP_LINEAR, // exposureFilter
        0.1f,       // cutoff
        0.0f,       // fadeout
        0.1f,       // backSplashSpot
        0.0f,       // backSplashSurface
		qfalse,     // deluxeMap
		40.0f,      // deluxeMinAngle
		1.0f,       // deluxeAmbientExaggerate
		1.0f,       // deluxeRadiosityExaggerate


		0.0f,       // superSampleRadius
		qfalse,     // upscale
		0,          // antialiasingPasses
		4,          // defaultSmoothPasses
		0.35f,      // defaultSmoothRadius
		"",         // flareShader
		""          // haloShader
	}
};


game_t *game = &gameTemplates[0];
static game_t activeGame;

/*
============
InitGame

Unified game profile initialization logic for both q3map and light tools.
============
*/
game_t *InitGame(int argc, char **argv) {
    char gamesDir[1024];
    sprintf(gamesDir, "%smakebsp", executablePath);

    // 1. Export standard profiles if missing (ensures [exeDir]/makebsp/qfusion.json exists)
    JSON_ExportStandardPackages(gamesDir);

    // 2. Initialize the local 'activeGame' struct by copying the default game_t into it.
    memcpy(&activeGame, &gameTemplates[0], sizeof(game_t));

    // 3. Pre-scan CLI for -game switch
    const char *gameName = "qfusion";
    for (int j = 1; j < argc; j++) {
        if (!strcmp(argv[j], "-game") && j + 1 < argc) {
            gameName = argv[j + 1];
            break;
        }
    }

    // 4. Load the specific game JSON to override defaults in the local struct
    char gameJsonPath[1024];
    sprintf(gameJsonPath, "%s/%s.json", gamesDir, gameName);
    if (FileExists(gameJsonPath)) {
        _printf("Loading game profile: %s\n", gameJsonPath);
        JSON_LoadGame(gameJsonPath, &activeGame);
    }

    // 5. Point the global game to our local struct
    game = &activeGame;

    return game;
}

void GetMapOutputDir(const char *source, char *out) {
    char baseName[256];
    ExtractFileBase(source, baseName);
    // User wants: writedir/maps/<mapname>/
    sprintf(out, "%smaps/%s/", writedir, baseName);
    NormalizePath(out);
}

void ClearCacheDirectory(void) {
    char baseDir[1024];
    char cachePath[1024];
    GetMapOutputDir(source, baseDir);
    sprintf(cachePath, "%scache", baseDir);
    _printf("Clearing cache directory: %s\n", cachePath);
#ifdef _WIN32
    char cmd[2048];
    sprintf(cmd, "powershell -NoProfile -Command \"if (Test-Path '%s') { Get-ChildItem '%s' | Remove-Item -Force -Recurse }\"", cachePath, cachePath);
    system(cmd);
#else
    char cmd[2048];
    sprintf(cmd, "rm -rf %s/*", cachePath);
    system(cmd);
#endif
}
