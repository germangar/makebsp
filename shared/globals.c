#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "globals.h"
#include "json_parser.h"
#include "../common/cmdlib.h"
#include "../common/surfaceflags.h"

char source[1024];
char name[1024];

int numGames = 2;
float shadingModelSoftBias = SHADING_MODEL_LAMBERT_SOFTBIAS;
float sunSoftBias = SHADING_MODEL_LAMBERT_SOFTBIAS;
float lightgridAmbientBias = 1.5f;
float lightgridDirectBias = 1.5f;
float lightgridSmoothAmbient = 0.0f;
int lightgridSmoothAmbientPasses = 1;
float lightgridSmoothDirect = 0.0f;
int lightgridSmoothDirectPasses = 1;
float lightgridMinAmbient = 0.0f;
float lightgridMaxAmbient = 0.0f;

vec3_t blockSize = {1024, 1024, 1024};
qboolean g_lowmem = qfalse;
qboolean g_debugExportLightmaps = qfalse;
qboolean g_debugMagentaTrisoups = qfalse;
qboolean g_debugCyanPatches = qfalse;
qboolean g_debugGreenPlanar = qfalse;
qboolean nodecimateplanar = qfalse;

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
		0x100000,   // maxMapDrawVerts
		0x100000,   // maxMapDrawIndexes
		0x40000,    // maxMapDrawSurfs
		0x100000,   // maxMapPlanes
		0x100000,   // maxMapNodes
		0x100000,   // maxMapLeafs
		0x100000,   // maxMapBrushSides
		0x100000,   // maxMapLeafSurfaces
		0x100000,   // maxMapLeafBrushes
		512,
		qfalse,     // externalLightmaps: if true, lightmaps are stored as external .tga files instead of inside the BSP
		qtrue,      // keepLights

		4,          // defaultSampleSize
		{ 64, 64, 64 }, // defaultGridSize
		qtrue,      // chamferEdges
		1.25f,      // chamferConvexWidth
		0.0f,       // chamferConcaveWidth
		0.0f,       // decalExtrusion
		6.0f,       // defaultTrisoupSubdivisions
        qtrue,      // enforceSampleSize
        qtrue,      // forceUVGen
        HDR_8BIT,   // hdr
		3.0f,       // hdr8BitScale
		qtrue,      // lightmapsRGB
		qfalse,     // lightgridRGB
		1.5f,       // lightgridAmbientBias
		1.5f,       // lightgridDirectBias
		256.0f,     // lightgridSmoothAmbient
		4,          // lightgridSmoothAmbientPasses
		128.0f,     // lightgridSmoothDirect
		3,          // lightgridSmoothDirectPasses
		0.0f,       // lightgridMinAmbient
		0.0f,       // lightgridMaxAmbient
		qtrue,      // texturesRGB
		qtrue,      // colorsRGB
		4,          // radiosityPasses
		1.0f,       // radiosityIntensity
		1.0f,       // radiosityColorRatio
        4,          // radiosityInterval
        0.5f,       // rad_ao_intensity
        0.0f,       // rad_ao_min
        32.0f,      // rad_ao_max
        512.0f,     // ambientTestRadius
        256.0f,     // ambientGatherRadius
		SHADING_MODEL_LAMBERT, // shading
		SHADING_MODEL_LAMBERT, // sunShading
		ATTENUATION_INVERSE_SQUARE, // attenuationModel
        TONEMAP_REINHARD, // exposureFilter
        1.25f,      // saturation
        SATRAMP_HALF_POWER, // saturationRamp
        0.1f,       // cutoff
        0.0f,       // fadeout
        0.1f,       // backSplashSpot
        0.0f,       // backSplashSurface
		qtrue,      // deluxeMap
		15.0f,      // deluxeMinAngle


		0.0f,       // superSampleRadius
		0,          // antialiasingPasses
		4,          // defaultSmoothPasses
		0.35f,      // defaultSmoothRadius
		"",         // flareShader
		"halo",     // haloShader
		{
			{ "nowalljump", 0, SURF_NOWALLJUMP, 0 }
		},
		1           // numCustomSurfaceParms
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
		0x100000,   // maxMapDrawVerts
		0x100000,   // maxMapDrawIndexes
		0x40000,    // maxMapDrawSurfs
		0x100000,   // maxMapPlanes
		0x100000,   // maxMapNodes
		0x100000,   // maxMapLeafs
		0x100000,   // maxMapBrushSides
		0x100000,   // maxMapLeafSurfaces
		0x100000,   // maxMapLeafBrushes
		128,
		qfalse,     // exportLightmaps
		qfalse,     // keepLights

		8,          // defaultSampleSize
		{ 64, 64, 64 }, // defaultGridSize
		qtrue,      // chamferEdges
		1.25f,      // chamferConvexWidth
		0.0f,       // chamferConcaveWidth
		0.0f,       // decalExtrusion
		6.0f,       // defaultTrisoupSubdivisions
        qtrue,       // enforceSampleSize
        qtrue,       // forceUVGen
        HDR_OFF,    // hdr
		1.0f,       // hdr8BitScale
		qfalse,     // lightmapsRGB
		qfalse,     // lightgridRGB
		1.5f,       // lightgridAmbientBias
		1.5f,       // lightgridDirectBias
		128.0f,     // lightgridSmoothAmbient
		1,          // lightgridSmoothAmbientPasses
		0.0f,       // lightgridSmoothDirect
		1,          // lightgridSmoothDirectPasses
		0.0f,       // lightgridMinAmbient
		0.0f,       // lightgridMaxAmbient
		qfalse,     // texturesRGB
		qfalse,     // colorsRGB
		4,          // radiosityPasses
		1.0f,       // radiosityIntensity
		0.75f,      // radiosityColorRatio
        4,          // radiosityInterval
        0.5f,       // rad_ao_intensity
        0.0f,       // rad_ao_min
        32.0f,      // rad_ao_max
        512.0f,     // ambientTestRadius
        256.0f,     // ambientGatherRadius
		SHADING_MODEL_LAMBERT, // shading
		SHADING_MODEL_LAMBERT, // sunShading
		ATTENUATION_INVERSE_SQUARE, // attenuationModel
        TONEMAP_LINEAR, // exposureFilter
        1.0f,       // saturation
        SATRAMP_OFF, // saturationRamp
        0.1f,       // cutoff
        0.0f,       // fadeout
        0.1f,       // backSplashSpot
        0.0f,       // backSplashSurface
		qfalse,     // deluxeMap
		40.0f,      // deluxeMinAngle


		0.0f,       // superSampleRadius
		0,          // antialiasingPasses
		4,          // defaultSmoothPasses
		0.35f,      // defaultSmoothRadius
		"",         // flareShader
		""          // haloShader
	}
};


game_t *game = &gameTemplates[0];
static game_t activeGame;

char activeGamedirs[MAX_ACTIVE_GAMEDIRS][MAX_QPATH];
int numActiveGamedirs = 0;

void AddActiveGamedir(const char *dir) {
    if (!dir || !dir[0]) return;
    for (int i = 0; i < numActiveGamedirs; i++) {
        if (!Q_stricmp(activeGamedirs[i], dir)) return;
    }
    if (numActiveGamedirs < MAX_ACTIVE_GAMEDIRS) {
        strncpy(activeGamedirs[numActiveGamedirs], dir, MAX_QPATH - 1);
        activeGamedirs[numActiveGamedirs][MAX_QPATH - 1] = '\0';
        numActiveGamedirs++;
    }
}

/*
============
InitGame

Unified game profile initialization logic for both q3map and light tools.
============
*/
game_t *InitGame(int argc, char **argv) {
    char gamesDir[1024];
    sprintf(gamesDir, "%smakebspdata", executablePath);

    // 1. Export standard profiles if missing (ensures [exeDir]/makebspdata/qfusion.json exists)
    JSON_ExportStandardPackages(gamesDir);

    // 2. Initialize the local 'activeGame' struct by copying the default game_t into it.
    memcpy(&activeGame, &gameTemplates[0], sizeof(game_t));

    // 3. Pre-scan CLI for -game switch
    const char *gameName = NULL;
    for (int j = 1; j < argc; j++) {
        if (!strcmp(argv[j], "-game") && j + 1 < argc) {
            gameName = argv[j + 1];
        }
    }

    // 4. Load the specific game JSON to override defaults in the local struct (if requested)
    if (gameName != NULL) {
        char gameJsonPath[1024];
        sprintf(gameJsonPath, "%s/%s.json", gamesDir, gameName);
        if (FileExists(gameJsonPath)) {
            _printf("Loading game profile: %s\n", gameJsonPath);
            JSON_LoadGame(gameJsonPath, &activeGame);
        } else {
            _printf("WARNING: Game profile '%s' not found at '%s'. Using defaults.\n", gameName, gameJsonPath);
        }
    }

    // 5. Point the global game to our local struct
    game = &activeGame;

    // 6. Register default game gamedir as active
    AddActiveGamedir(game->gameDir);

    return game;
}

void GetMapOutputDir(const char *source, char *out) {
    char baseName[256];
    // Extract the map name regardless of extension or leading path
    ExtractFileBase(source, baseName);
    
    // User wants: writedir/maps/<mapname>/
    sprintf(out, "%smaps/%s/", writedir, baseName);
    
    // Final normalization to ensure consistent slashes
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
