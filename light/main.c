/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "../common/cmdlib.h"
#include "light.h"
#include "radiosity.h"

#include <string.h>

#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

extern qboolean nodirect;
extern qboolean noambient;
qboolean deluxeSort = qfalse;
int radiosityPasses = 0;
extern tonemap_t tonemapMode;
qboolean g_fast = qfalse;

qboolean directonly = qfalse;
qboolean radiosityonly = qfalse;
qboolean ambientonly = qfalse;
qboolean upscale = qfalse;
float deluxeAmbientExaggerate = 1.0f;
float deluxeRadiosityExaggerate = 1.0f;

static qboolean HasArg(const char *arg, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], arg)) return qtrue;
    }
    return qfalse;
}

static void ParseWorldspawnKeys(int argc, char **argv)
{
    entity_t *ent = &entities[0];
    
    // These keys have been set by the bsp compiler
    const char *val = ValueForKey(ent, "__texelsize");
    if (val[0]) {
        game->defaultSampleSize = atoi(val);
        qprintf("Inferred lightmap sample size %dx%d from worldspawn (__texelsize)\n", game->defaultSampleSize, game->defaultSampleSize);
    }

    const char *lmSizeVal = ValueForKey(ent, "_lightmapImageSize");
    if (!lmSizeVal[0]) {
        Error("Worldspawn missing required key '_lightmapImageSize'.\n"
                "This BSP was likely compiled with an old version of q3map.\n"
                "Please re-run the BSP phase.");
    }
    int bspLmSize = atoi(lmSizeVal);
    if (bspLmSize != game->lightmapSize) {
        _printf("Adapting lightmap atlas size from profile default (%d) to BSP value (%d).\n",
                game->lightmapSize, bspLmSize);
        game->lightmapSize = bspLmSize;
        game->externalLightmaps = qtrue;
    }

    // map keys
    val = ValueForKey(ent, "cutoff");
    if (val[0] && !HasArg("-cutoff", argc, argv)) {
        game->minLightAdd = (float)atof(val);
        if (game->minLightAdd < 0.001f)
            game->minLightAdd = 0.001f;
    }

    val = ValueForKey(ent, "fadeout");
    if (val[0] && !HasArg("-fadeout", argc, argv)) {
        game->fadeout = (float)atof(val);
        if (game->fadeout < 0.0f)
            game->fadeout = 0.0f;
        else if (game->fadeout > 1.0f)
            game->fadeout = 1.0f;
    }

    val = ValueForKey(ent, "backsplashspot");
    if (val[0] && !HasArg("-backsplashspot", argc, argv)) {
        game->backSplashSpot = (float)atof(val);
        if (game->backSplashSpot < 0.0f) game->backSplashSpot = 0.0f;
        else if (game->backSplashSpot > 1.0f) game->backSplashSpot = 1.0f;
    }

    val = ValueForKey(ent, "backsplashsurface");
    if (val[0] && !HasArg("-backsplashsurface", argc, argv)) {
        game->backSplashSurface = (float)atof(val);
        if (game->backSplashSurface < 0.0f) game->backSplashSurface = 0.0f;
        else if (game->backSplashSurface > 1.0f) game->backSplashSurface = 1.0f;
    }

    val = ValueForKey(ent, "smooth");
    if (val[0] && !HasArg("-smooth", argc, argv)) {
        game->defaultSmoothRadius = (float)atof(val);
        if (game->defaultSmoothRadius < 0.1f)
            game->defaultSmoothRadius = 0.1f;
    }

    val = ValueForKey(ent, "smoothpasses");
    if (val[0] && !HasArg("-smoothpasses", argc, argv)) {
        game->defaultSmoothPasses = atoi(val);
        if (game->defaultSmoothPasses < 0) game->defaultSmoothPasses = 0;
    }

    val = ValueForKey(ent, "haloshader");
    if (val[0]) {
        game->haloShader = copystring(val);
        _printf("Worldspawn override: haloShader = %s\n", game->haloShader);
    }

    val = ValueForKey(ent, "antialiasing");
    if (val[0] && !HasArg("-antialiasing", argc, argv)) {
        game->antialiasingPasses = atoi(val);
    }

    val = ValueForKey(ent, "deluxe_minangle");
    if (val[0] && !HasArg("-deluxe_minangle", argc, argv)) {
        game->deluxeMinAngle = atof(val);
        if (game->deluxeMinAngle < 0.0f) game->deluxeMinAngle = 0.0f;
        if (game->deluxeMinAngle > 89.0f) game->deluxeMinAngle = 89.0f;
    }

    val = ValueForKey(ent, "exposurefilter");
    if (val[0] && !HasArg("-exposurefilter", argc, argv)) {
        if (!Q_stricmp(val, "softknee")) game->exposureFilter = TONEMAP_SOFTKNEE;
        else if (!Q_stricmp(val, "reinhard")) game->exposureFilter = TONEMAP_REINHARD;
        else if (!Q_stricmp(val, "filmic")) game->exposureFilter = TONEMAP_FILMIC;
        else game->exposureFilter = TONEMAP_LINEAR;
    }
    val = ValueForKey(ent, "saturation");
    if (val[0] && !HasArg("-saturation", argc, argv)) {
        game->saturation = atof(val);
        if (game->saturation < 0.0f) game->saturation = 0.0f;
    }

    val = ValueForKey(ent, "saturationramp");
    if (val[0] && !HasArg("-saturationramp", argc, argv)) {
        if (!Q_stricmp(val, "filmic")) game->saturationRamp = SATRAMP_FILMIC;
        else if (!Q_stricmp(val, "power")) game->saturationRamp = SATRAMP_POWER;
        else if (!Q_stricmp(val, "halfpower")) game->saturationRamp = SATRAMP_HALF_POWER;
        else if (!Q_stricmp(val, "midtone")) game->saturationRamp = SATRAMP_MIDTONE;
        else game->saturationRamp = SATRAMP_OFF;
    }

    val = ValueForKey(ent, "shading");
    if (val[0] && !HasArg("-shading", argc, argv)) {
        if (!Q_stricmp(val, "halflambert")) {
            game->shadingModel = SHADING_MODEL_HALFLAMBERT;
            shadingModelSoftBias = SHADING_MODEL_HALFLAMBERT_SOFTBIAS;
            sunSoftBias = SHADING_MODEL_HALFLAMBERT_SOFTBIAS;
        } else if (!Q_stricmp(val, "lambert")) {
            game->shadingModel = SHADING_MODEL_LAMBERT;
            shadingModelSoftBias = SHADING_MODEL_LAMBERT_SOFTBIAS;
            sunSoftBias = SHADING_MODEL_LAMBERT_SOFTBIAS;
        } else if (!Q_stricmp(val, "quadratic")) {
            game->shadingModel = SHADING_MODEL_QUADRATIC;
            shadingModelSoftBias = SHADING_MODEL_QUADRATIC_SOFTBIAS;
            sunSoftBias = SHADING_MODEL_QUADRATIC_SOFTBIAS;
        } else if (!Q_stricmp(val, "doublequadratic")) {
            game->shadingModel = SHADING_MODEL_DOUBLEQUADRATIC;
            shadingModelSoftBias = SHADING_MODEL_DOUBLEQUADRATIC_SOFTBIAS;
            sunSoftBias = SHADING_MODEL_DOUBLEQUADRATIC_SOFTBIAS;
        } else if (!Q_stricmp(val, "unreal")) {
            game->shadingModel = SHADING_MODEL_UNREAL;
            shadingModelSoftBias = SHADING_MODEL_UNREAL_SOFTBIAS;
            sunSoftBias = SHADING_MODEL_UNREAL_SOFTBIAS;
        } else {
            Error("Unknown shading mode: %s", val);
        }
    }

    val = ValueForKey(ent, "attenuation");
    if (val[0] && !HasArg("-attenuation", argc, argv)) {
        if (!Q_stricmp(val, "soft")) {
            game->attenuationModel = ATTENUATION_INVERSE;
        } else if (!Q_stricmp(val, "linear")) {
            game->attenuationModel = ATTENUATION_LINEAR;
        } else if (!Q_stricmp(val, "standard")) {
            game->attenuationModel = ATTENUATION_INVERSE_SQUARE;
        } else if (!Q_stricmp(val, "unreal")) {
            game->attenuationModel = ATTENUATION_UNREAL;
        } else if (!Q_stricmp(val, "smoothstep")) {
            game->attenuationModel = ATTENUATION_SMOOTHSTEP;
        } else {
            Error("Unknown attenuation mode: %s", val);
        }
    }

    val = ValueForKey(ent, "radiosity");
    if (val[0] && !HasArg("-radiosity", argc, argv)) {
        game->radiosityIntensity = (float)atof(val);
        if (game->radiosityIntensity < 0.0f) game->radiosityIntensity = 0.0f;
    }

    val = ValueForKey(ent, "rad_color_ratio");
    if (val[0] && !HasArg("-rad_color_ratio", argc, argv)) {
        game->radiosityColorRatio = (float)atof(val);
    }

    val = ValueForKey(ent, "deluxe");
    if (val[0] && !HasArg("-deluxe", argc, argv)) {
        game->deluxeMap = atoi(val) != 0;
    }

    val = ValueForKey(ent, "rad_interval");
    if (val[0] && !HasArg("-rad_interval", argc, argv)) {
        game->radiosityInterval = atoi(val);
        if (game->radiosityInterval < 1) game->radiosityInterval = 1;
    }

    val = ValueForKey(ent, "rad_ao_intensity");
    if (val[0] && !HasArg("-rad_ao_intensity", argc, argv)) {
        game->rad_ao_intensity = atof(val);
        if (game->rad_ao_intensity < 0.0f) game->rad_ao_intensity = 0.0f;
        if (game->rad_ao_intensity > 1.0f) game->rad_ao_intensity = 1.0f;
    }

    val = ValueForKey(ent, "rad_ao_min");
    if (val[0] && !HasArg("-rad_ao_min", argc, argv)) {
        game->rad_ao_min = atof(val);
    }

    val = ValueForKey(ent, "rad_ao_max");
    if (val[0] && !HasArg("-rad_ao_max", argc, argv)) {
        game->rad_ao_max = atof(val);
    }

    val = ValueForKey(ent, "supersample");
    if (val[0] && !HasArg("-supersample", argc, argv)) {
        game->superSampleRadius = atof(val);
        if (game->superSampleRadius < 0.0f) game->superSampleRadius = 0.0f;
    }

    val = ValueForKey(ent, "ambient_testradius");
    if (val[0] && !HasArg("-ambient_testradius", argc, argv)) {
        ambient_testradius = (float)atof(val);
        if (ambient_testradius < 32.0f) ambient_testradius = 32.0f;
    }

    val = ValueForKey(ent, "ambient_gatheradius");
    if (val[0] && !HasArg("-ambient_gatheradius", argc, argv)) {
        ambient_gatheradius = (float)atof(val);
        if (ambient_gatheradius < 32.0f) ambient_gatheradius = 32.0f;
    }

    val = ValueForKey(ent, "_gridambientbias");
    if (!val[0]) val = ValueForKey(ent, "gridambientbias");
    if (val[0] && !HasArg("-gridambientbias", argc, argv)) {
        lightgridAmbientBias = (float)atof(val);
    }

    val = ValueForKey(ent, "_gridminlight");
    if (!val[0]) val = ValueForKey(ent, "gridminlight");
    if (val[0] && !HasArg("-gridminlight", argc, argv)) {
        lightgridMinLight = (float)atof(val);
    }

    val = ValueForKey(ent, "_gridmaxlight");
    if (!val[0]) val = ValueForKey(ent, "gridmaxlight");
    if (val[0] && !HasArg("-gridmaxlight", argc, argv)) {
        lightgridMaxLight = (float)atof(val);
    }

}

int main(int argc, char **argv) {
    int i;
    double start, end;

    GetExecutablePath(argv[0]);

    _printf("\n----- Lighting (Ag Build v1.1) ----\n");

    verbose = qfalse;
    areaScale = 0.25;

    openclEnabled = qtrue;

    // Initialize game profile from JSON and CLI
    game = InitGame(argc, argv);
    lightgridAmbientBias = game->lightgridAmbientBias;
    lightgridMinLight = game->lightgridMinLight;
    lightgridMaxLight = game->lightgridMaxLight;
    
    ambient_testradius = game->ambientTestRadius;
    ambient_gatheradius = game->ambientGatherRadius;

    // Pre-scan CLI for VFS path construction
    const char *cliPakPaths[MAX_VFS_PATHS];
    int numCliPakPaths = 0;
    const char *cliUserDirs[MAX_VFS_PATHS];
    int numCliUserDirs = 0;
    const char *cliBasePaths[MAX_VFS_PATHS];
    int numCliBasePaths = 0;
    const char *modGameDirs[MAX_VFS_PATHS];
    int numModGameDirs = 0;
    const char *baseGameDir = game->gameDir;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-fs_pakpath") && i + 1 < argc)
        {
            if (numCliPakPaths < MAX_VFS_PATHS) cliPakPaths[numCliPakPaths++] = argv[i + 1];
            i++;
        }
        else if ((!strcmp(argv[i], "-userdir") || !strcmp(argv[i], "-fs_homepath")) && i + 1 < argc)
        {
            if (numCliUserDirs < MAX_VFS_PATHS) cliUserDirs[numCliUserDirs++] = argv[i + 1];
            i++;
        }
        else if ((!strcmp(argv[i], "-basepath") || !strcmp(argv[i], "-rootdir") || !strcmp(argv[i], "-fs_basepath")) && i + 1 < argc)
        {
            if (numCliBasePaths < MAX_VFS_PATHS) cliBasePaths[numCliBasePaths++] = argv[i + 1];
            i++;
        }
        else if ((!strcmp(argv[i], "-gamedir") || !strcmp(argv[i], "-fs_game")) && i + 1 < argc)
        {
            if (numModGameDirs < MAX_VFS_PATHS) {
                modGameDirs[numModGameDirs++] = argv[i + 1];
                AddActiveGamedir(argv[i + 1]);
            }
            i++;
        }
    }

    // Default fallbacks if no CLI arguments provided
    if (numCliUserDirs == 0 && game->userDir && game->userDir[0])
        cliUserDirs[numCliUserDirs++] = game->userDir;
        
    if (numCliBasePaths == 0)
        cliBasePaths[numCliBasePaths++] = (game->rootDir && game->rootDir[0]) ? game->rootDir : ".";

    // 1. Pak Paths (Highest priority for searching and preferred write destination)
    for (i = 0; i < numCliPakPaths; i++)
    {
        AddVFSPath(cliPakPaths[i], "");
    }

    // 2. Mod GameDirs Layer
    // Mod directories take precedence over the base game directory, across both user and base paths
    for (int j = 0; j < numModGameDirs; j++)
    {
        for (i = 0; i < numCliUserDirs; i++)
            AddVFSPath(cliUserDirs[i], modGameDirs[j]);
        for (i = 0; i < numCliBasePaths; i++)
            AddVFSPath(cliBasePaths[i], modGameDirs[j]);
    }

    // 3. Base GameDir Layer (Deepest fallback)
    // Only add userdir/baseGameDir if we are not working on a mod.
    if (numModGameDirs == 0)
    {
        for (i = 0; i < numCliUserDirs; i++)
            AddVFSPath(cliUserDirs[i], baseGameDir);
    }
    
    // Always add the rootdir/baseGameDir as the final fallback for base game assets
    for (i = 0; i < numCliBasePaths; i++)
    {
        AddVFSPath(cliBasePaths[i], baseGameDir);
    }

    InitVFSWriteDir();

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-tempname")) {
            i++;
        } else if (!strcmp(argv[i], "-opencl")) {
            openclEnabled = atoi(argv[++i]) ? qtrue : qfalse;
            _printf("OpenCL %s\n", openclEnabled ? "enabled" : "disabled");
        } else if (!strcmp(argv[i], "-v")) {
            verbose = qtrue;
        } else if (!strcmp(argv[i], "-threads")) {
            numthreads = atoi(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-directonly")) {
            directonly = qtrue;
        } else if (!strcmp(argv[i], "-radiosityonly")) {
            radiosityonly = qtrue;
        } else if (!strcmp(argv[i], "-ambientonly")) {
            ambientonly = qtrue;
        } else if (!strcmp(argv[i], "-area")) {
            areaScale *= atof(argv[i + 1]);
            _printf("area light scaling at %f\n", areaScale);
            i++;
        } else if (!strcmp(argv[i], "-nodirect")) {
            nodirect = qtrue;
            _printf("No direct lighting\n");
        } else if (!strcmp(argv[i], "-noambient")) {
            noambient = qtrue;
            _printf("No ambient lighting\n");
        } else if (!strcmp(argv[i], "-upscale")) {
            upscale = qtrue;
            _printf("Upscale detail tracing enabled (2x grid)\n");
        } else if (!strcmp(argv[i], "-novertex")) {
            novertexlighting = qtrue;
            _printf("no vertex lighting = true\n");
        } else if (!strcmp(argv[i], "-nogrid")) {
            nogridlighting = qtrue;
            _printf("no grid lighting = true\n");
        } else if (!strcmp(argv[i], "-gridambientbias")) {
            lightgridAmbientBias = atof(argv[i + 1]);
            _printf("grid ambient bias = %f\n", lightgridAmbientBias);
            i++;
        } else if (!strcmp(argv[i], "-gridminlight")) {
            lightgridMinLight = atof(argv[i + 1]);
            _printf("grid minlight = %f\n", lightgridMinLight);
            i++;
        } else if (!strcmp(argv[i], "-gridmaxlight")) {
            lightgridMaxLight = atof(argv[i + 1]);
            _printf("grid maxlight = %f\n", lightgridMaxLight);
            i++;
        } else if (!strcmp(argv[i], "-border")) {
            lightmapBorder = qtrue;
            _printf("Adding debug border to lightmaps\n");
        } else if (!strcmp(argv[i], "-debuglightmaps")) {
            debugLightmaps = qtrue;
            _printf("Lightmap debug visualization enabled (FAST mode)\n");
        } else if (!strcmp(argv[i], "-debuglightmapsalpha")) {
            debugLightmapsAlpha = qtrue;
            _printf("Lightmap debug visualization enabled (ALPHA/ACCURATE mode)\n");
        } else if (!strcmp(argv[i], "-game")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-game requires a profile name");
            i++; // Handled in pre-scan
        } else if (!strcmp(argv[i], "-basepath") || !strcmp(argv[i], "-rootdir") ||
                   !strcmp(argv[i], "-fs_basepath")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("%s requires a directory path", argv[i]);
            i++; // Handled in pre-scan
        } else if (!strcmp(argv[i], "-userdir") || !strcmp(argv[i], "-fs_homepath") || !strcmp(argv[i], "-fs_pakpath")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("%s requires a directory path", argv[i]);
            i++; // Handled in pre-scan
        } else if (!strcmp(argv[i], "-gamedir") || !strcmp(argv[i], "-fs_game")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("%s requires a directory path", argv[i]);
            i++; // Handled in pre-scan
        } else if (!strcmp(argv[i], "-connect")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("%s requires an IP address", argv[i]);
            Broadcast_Setup(argv[++i]);
        } else if (!strcmp(argv[i], "-sRGB")) {
            game->lightmapsRGB = qtrue;
        } else if (!strcmp(argv[i], "-shading")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-shading requires a type (lambert, halflambert, etc.)");
            char *arg = argv[++i];
            if (!Q_stricmp(arg, "halflambert")) {
                game->shadingModel = SHADING_MODEL_HALFLAMBERT;
                shadingModelSoftBias = SHADING_MODEL_HALFLAMBERT_SOFTBIAS;
                sunSoftBias = SHADING_MODEL_HALFLAMBERT_SOFTBIAS;
            } else if (!Q_stricmp(arg, "lambert")) {
                game->shadingModel = SHADING_MODEL_LAMBERT;
                shadingModelSoftBias = SHADING_MODEL_LAMBERT_SOFTBIAS;
                sunSoftBias = SHADING_MODEL_LAMBERT_SOFTBIAS;
            } else if (!Q_stricmp(arg, "quadratic")) {
                game->shadingModel = SHADING_MODEL_QUADRATIC;
                shadingModelSoftBias = SHADING_MODEL_QUADRATIC_SOFTBIAS;
                sunSoftBias = SHADING_MODEL_QUADRATIC_SOFTBIAS;
            } else if (!Q_stricmp(arg, "doublequadratic")) {
                game->shadingModel = SHADING_MODEL_DOUBLEQUADRATIC;
                shadingModelSoftBias = SHADING_MODEL_DOUBLEQUADRATIC_SOFTBIAS;
                sunSoftBias = SHADING_MODEL_DOUBLEQUADRATIC_SOFTBIAS;
            } else if (!Q_stricmp(arg, "unreal")) {
                game->shadingModel = SHADING_MODEL_UNREAL;
                shadingModelSoftBias = SHADING_MODEL_UNREAL_SOFTBIAS;
                sunSoftBias = SHADING_MODEL_UNREAL_SOFTBIAS;
            } else {
                Error("Unknown shading mode: %s", arg);
            }
        } else if (!strcmp(argv[i], "-attenuation")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-attenuation requires a type (standard, soft, linear, unreal, smoothstep)");
            char *arg = argv[++i];
            if (!Q_stricmp(arg, "soft")) {
                game->attenuationModel = ATTENUATION_INVERSE;
            } else if (!Q_stricmp(arg, "linear")) {
                game->attenuationModel = ATTENUATION_LINEAR;
            } else if (!Q_stricmp(arg, "standard")) {
                game->attenuationModel = ATTENUATION_INVERSE_SQUARE;
            } else if (!Q_stricmp(arg, "unreal")) {
                game->attenuationModel = ATTENUATION_UNREAL;
            } else if (!Q_stricmp(arg, "smoothstep")) {
                game->attenuationModel = ATTENUATION_SMOOTHSTEP;
            } else {
                Error("Unknown attenuation mode: %s", arg);
            }
        } else if (!strcmp(argv[i], "-shading_softbias")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-shading_softbias requires a numeric value");
            shadingModelSoftBias = (float)atof(argv[i + 1]);
            if (shadingModelSoftBias < 0.0f) shadingModelSoftBias = 0.0f;
            if (shadingModelSoftBias > 1.0f) shadingModelSoftBias = 1.0f;
            i++;
        } else if (!strcmp(argv[i], "-sunshading_softbias")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-sunshading_softbias requires a numeric value");
            sunSoftBias = (float)atof(argv[i + 1]);
            if (sunSoftBias < 0.0f) sunSoftBias = 0.0f;
            if (sunSoftBias > 1.0f) sunSoftBias = 1.0f;
            i++;
        } else if (!strcmp(argv[i], "-sunshading")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-sunshading requires a type (lambert, halflambert, etc.)");
            char *arg = argv[++i];
            if (!Q_stricmp(arg, "halflambert")) {
                game->sunShadingModel = SHADING_MODEL_HALFLAMBERT;
            } else if (!Q_stricmp(arg, "lambert")) {
                game->sunShadingModel = SHADING_MODEL_LAMBERT;
            } else if (!Q_stricmp(arg, "quadratic")) {
                game->sunShadingModel = SHADING_MODEL_QUADRATIC;
            } else if (!Q_stricmp(arg, "doublequadratic")) {
                game->sunShadingModel = SHADING_MODEL_DOUBLEQUADRATIC;
            } else if (!Q_stricmp(arg, "unreal")) {
                game->sunShadingModel = SHADING_MODEL_UNREAL;
            } else {
                Error("Unknown sun shading mode: %s", arg);
            }
        } else if (!strcmp(argv[i], "-deluxe")) {
            if (i + 1 >= argc || argv[i+1][0] == '-') Error("-deluxe requires 1 or 0");
            game->deluxeMap = atoi(argv[++i]) ? qtrue : qfalse;
            _printf("Deluxemaps %s via command line override\n", game->deluxeMap ? "enabled" : "disabled");
        } else if (!strcmp(argv[i], "-deluxe_minangle")) {
            if (i + 1 >= argc || argv[i+1][0] == '-') Error("-deluxe_minangle requires an angle in degrees");
            game->deluxeMinAngle = atof(argv[++i]);
            if (game->deluxeMinAngle < 0.0f) game->deluxeMinAngle = 0.0f;
            if (game->deluxeMinAngle > 89.0f) game->deluxeMinAngle = 89.0f;
            _printf("Deluxe Min Angle floor set to %.1f degrees\n", game->deluxeMinAngle);
        } else if (!strcmp(argv[i], "-deluxe_ambient_exaggerate")) {
            if (i + 1 >= argc || argv[i+1][0] == '-') Error("-deluxe_ambient_exaggerate requires a scalar factor");
            deluxeAmbientExaggerate = atof(argv[++i]);
            if (deluxeAmbientExaggerate < 0.0f) deluxeAmbientExaggerate = 0.0f;
            _printf("Deluxe Ambient Exaggerate multiplier set to %.2f\n", deluxeAmbientExaggerate);
        } else if (!strcmp(argv[i], "-deluxe_radiosity_exaggerate")) {
            if (i + 1 >= argc || argv[i+1][0] == '-') Error("-deluxe_radiosity_exaggerate requires a scalar factor");
            deluxeRadiosityExaggerate = atof(argv[++i]);
            if (deluxeRadiosityExaggerate < 0.0f) deluxeRadiosityExaggerate = 0.0f;
            _printf("Deluxe Radiosity Exaggerate multiplier set to %.2f\n", deluxeRadiosityExaggerate);
        } else if (!strcmp(argv[i], "-deluxesort")) {
            deluxeSort = qtrue;
            _printf("Deluxe sorting enabled (no-influence lights processed last)\n");
        } else if (!strcmp(argv[i], "-supersample")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-supersample requires a radius value (e.g. 0.5 or 1.0)");
            game->superSampleRadius = atof(argv[i + 1]);
            if (game->superSampleRadius < 0.0f)
                game->superSampleRadius = 0.0f;
            if (game->superSampleRadius > 0.0f) {
                _printf("Super-sampling enabled: radius %.3f with 8x pattern\n", game->superSampleRadius);
            } else {
                _printf("Super-sampling disabled\n");
            }
            i++;
        } else if (!strcmp(argv[i], "-smoothpasses")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-smoothpasses requires a number of passes");
            game->defaultSmoothPasses = atoi(argv[i + 1]);
            if (game->defaultSmoothPasses < 0) game->defaultSmoothPasses = 0;
            _printf("Smoothing passes set to %d\n", game->defaultSmoothPasses);
            i++;
        } else if (!strcmp(argv[i], "-antialiasing")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-antialiasing requires a number of passes");
            game->antialiasingPasses = atoi(argv[i + 1]);
            _printf("Anti-Aliasing post-process pass enabled (Mode %d)\n", game->antialiasingPasses);
            i++;
        } else if (!strcmp(argv[i], "-smooth")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-smooth requires a radius value");
            game->defaultSmoothRadius = (float)atof(argv[i + 1]);
            if (game->defaultSmoothRadius < 0.1f)
                game->defaultSmoothRadius = 0.1f;
            i++;
        } else if (!strcmp(argv[i], "-rad_passes")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_passes requires a number of passes");
            game->radiosityPasses = atoi(argv[i + 1]);
            if (game->radiosityPasses < 0)
                game->radiosityPasses = 0;
            i++;
        } else if (!strcmp(argv[i], "-rad_ao_min")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_ao_min requires a numeric value");
            game->rad_ao_min = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_ao_max")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_ao_max requires a numeric value");
            game->rad_ao_max = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_min_energy")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_min_energy requires a numeric value");
            rad_min_energy = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_interval")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_interval requires a numeric value");
            game->radiosityInterval = atoi(argv[i + 1]);
            if (game->radiosityInterval < 1) game->radiosityInterval = 1;
            i++;
        } else if (!strcmp(argv[i], "-rad_color_ratio")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_color_ratio requires a numeric value");
            game->radiosityColorRatio = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-radiosity")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-radiosity requires a numeric value");
            game->radiosityIntensity = (float)atof(argv[i + 1]);
            if (game->radiosityIntensity < 0.0f) game->radiosityIntensity = 0.0f;
            i++;
        } else if (!strcmp(argv[i], "-rad_ao_intensity")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_ao_intensity requires a numeric value");
            game->rad_ao_intensity = (float)atof(argv[i + 1]);
            if (game->rad_ao_intensity < 0.0f) game->rad_ao_intensity = 0.0f;
            if (game->rad_ao_intensity > 1.0f) game->rad_ao_intensity = 1.0f;
            i++;
        } else if (!strcmp(argv[i], "-ambient_grid_samples")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-ambient_grid_samples requires a numeric value");
            ambient_grid_samples = atoi(argv[i + 1]);
            if (ambient_grid_samples < 4)   ambient_grid_samples = 4;
            if (ambient_grid_samples > 512) ambient_grid_samples = 512;
            i++;
        } else if (!strcmp(argv[i], "-ambient_samples")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-ambient_samples requires a numeric value");
            ambient_samples = atoi(argv[i + 1]);
            if (ambient_samples < 4)   ambient_samples = 4;
            if (ambient_samples > 512) ambient_samples = 512;
            i++;
        } else if (!strcmp(argv[i], "-ambient_testradius")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-ambient_testradius requires a numeric value");
            ambient_testradius = (float)atof(argv[i + 1]);
            if (ambient_testradius < 32.0f) ambient_testradius = 32.0f;
            _printf("Ambient test radius set to %.1f wu\n", ambient_testradius);
            i++;
        } else if (!strcmp(argv[i], "-ambient_gatheradius")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-ambient_gatheradius requires a numeric value");
            ambient_gatheradius = (float)atof(argv[i + 1]);
            if (ambient_gatheradius < 32.0f) ambient_gatheradius = 32.0f;
            _printf("Ambient gather radius set to %.1f wu\n", ambient_gatheradius);
            i++;
        } else if (!strcmp(argv[i], "-rad_voxelsize")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_voxelsize requires a numeric value");
            rad_voxel_size = (float)atof(argv[i + 1]);
            if (rad_voxel_size < 0.1f) rad_voxel_size = 0.1f;
            i++;

        } else if (!strcmp(argv[i], "-exposurefilter")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-exposurefilter requires a mode (softknee, reinhard, or filmic)");
            const char *mode = argv[i + 1];
            if (!Q_stricmp(mode, "softknee")) {
                game->exposureFilter = TONEMAP_SOFTKNEE;
            } else if (!Q_stricmp(mode, "reinhard")) {
                game->exposureFilter = TONEMAP_REINHARD;
            } else if (!Q_stricmp(mode, "filmic")) {
                game->exposureFilter = TONEMAP_FILMIC;
            } else {
                game->exposureFilter = TONEMAP_LINEAR;
            }
            i++;
        } else if (!strcmp(argv[i], "-saturation")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-saturation requires a numeric value");
            game->saturation = (float)atof(argv[i + 1]);
            if (game->saturation < 0.0f) game->saturation = 0.0f;
            i++;
        } else if (!strcmp(argv[i], "-saturationramp")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-saturationramp requires a mode (off, filmic, power, midtone)");
            if (!Q_stricmp(argv[i + 1], "filmic")) game->saturationRamp = SATRAMP_FILMIC;
            else if (!Q_stricmp(argv[i + 1], "power")) game->saturationRamp = SATRAMP_POWER;
            else if (!Q_stricmp(argv[i + 1], "halfpower")) game->saturationRamp = SATRAMP_HALF_POWER;
            else if (!Q_stricmp(argv[i + 1], "midtone")) game->saturationRamp = SATRAMP_MIDTONE;
            else game->saturationRamp = SATRAMP_OFF;
            i++;
        } else if (!strcmp(argv[i], "-lightmaprange")) {
            game->hdr = HDR_8BIT;
        } else if (!strcmp(argv[i], "-fast")) {
            g_fast = qtrue;
            _printf("Optimized voxelization mode (FAST) enabled\n");
        } else if (!strcmp(argv[i], "-lowmem")) {
            g_lowmem = qtrue;
            _printf("Low-memory mode enabled (using memory-mapped files)\n");
        } else if (!strcmp(argv[i], "-exportlightmaps")) {
            g_debugExportLightmaps = qtrue;
            _printf("Exporting a copy of the lightmaps as images for visual inspection\n");
        } else if (!strcmp(argv[i], "-magentatrisoups")) {
            g_debugMagentaTrisoups = qtrue;
            _printf("Coloring TRISOUP lightmaps flat magenta\n");
        } else if (!strcmp(argv[i], "-cyanpatches")) {
            g_debugCyanPatches = qtrue;
            _printf("Coloring PATCH lightmaps flat cyan\n");
        } else if (!strcmp(argv[i], "-greenplanar")) {
            g_debugGreenPlanar = qtrue;
            _printf("Coloring PLANAR lightmaps flat green\n");
        } else {
            break;
        }
    }

    if (i != argc - 1) {
        if (i < argc) {
            _printf("Error: Unrecognized switch or extra argument '%s'\n", argv[i]);
        }
        _printf("usage: light [-<switch> [-<switch> ...]] <mapname>\n"
                "\n"
                "Switches:\n"
                "   v              = verbose output\n"
                "   opencl <0|1>   = enable (1, default) or disable (0) GPU acceleration\n"
                "   threads <X>    = set number of threads to X\n"
                "   area <V>       = set the area light scale to V\n"
                "   -nodirect      = skip direct lighting passes\n"
               "   -noambient     = skip ambient lighting passes\n"
                "   upscale        = enable 2x lightmap upscaling for anti-aliasing\n"
                "   shading <type>  = set the shading model (lambert, halflambert, quadratic, doublequadratic, unreal)\n"
                "   shading_softbias <F> = override the default soft bias for the shading model\n"
                "   sunshading <type> = override the sun shading model\n"
                "   sunshading_softbias <F> = override the sun soft bias\n"
                "   -lowmem        = use memory-mapped files for massive radiosity passes\n"
                "   -exportlightmaps = Export a copy of the lightmaps as images for visual inspection\n"
                "   -magentatrisoups = Color TRISOUP lightmaps flat magenta (for export debugging)\n"
                "   -cyanpatches    = Color PATCH lightmaps flat cyan (for export debugging)\n"
                "   -greenplanar    = Color PLANAR lightmaps flat green (for export debugging)\n"
                "   deluxe <0|1>    = enable (1) or disable (0) deluxemapping\n"
                "   deluxe_minangle <A> = clamp the minimum angle of incidence for deluxe vectors (in degrees)\n"
                "   deluxe_ambient_exaggerate <F> = scalar factor to exaggerate deluxemap incidence angle during ambient pass\n"
                "   deluxe_radiosity_exaggerate <F> = scalar factor to exaggerate deluxemap incidence angle during radiosity pass\n"
                "   brutetrace      = disable all tracing optimizations for debugging\n"
                "   debuglightmaps = generate BMP files showing lightmap allocation (FAST)\n"
                "   debuglightmapsalpha = generate BMP files showing exact lit pixels (SLOW)\n"
                "    -deluxemode <0-2>   Output deluxe lighting to direction lightmaps. 0=none, 1=average, 2=bumpmap.\n"
                "    -directonly         Skip radiosity and ambient passes.\n"
                "    -radiosityonly      Skip ambient and clear direct lighting before merging radiosity.\n"
                "    -ambientonly        Skip direct and radiosity passes.\n"
                "   smoothpasses <passes> = number of post-process smoothing passes to run\n"
                "   smoothradius <R> = set radius for blurring (world) and jitter (super-sampling)\n"
                "   antialiasing <passes> = number of anti-aliasing post-process passes to run\n"
                "   supersample <mode> = trace-time super-sampling mode:\n"
                "                     0 = OFF\n"
                "                     1 = super-sampling EVERYTHING\n"
                "                     2 = super-sampling models only\n"
                "   rad_passes <N>   = set the number of radiosity passes (high-fidelity bounce)\n"
                "   rad_ao_min <F>   = set inner distance limit for radiosity plateau\n"
                "   rad_ao_max <F>   = set transition range width for radiosity gradient (starts at min)\n"
                "   rad_min_energy <F>= set min luxel energy to spawn an emitter\n"
                "   rad_interval <I>  = set sparse grid interval (1=Every luxel, default 4=4x4 blocks)\n"
                "   rad_color_ratio <F>= set greyscale(0.0) vs color(1.0) bleeding\n"
                "   rad_voxelsize <F> = set the world-space size of reconstruction voxels\n"
                "   radiosity <F>       = set final bounce energy multiplier\n"
                "   rad_ao_intensity <F>= set crease ambient occlusion amount (0.0=none, 1.0=max crease darkness, default: 0.5)\n"
                "   ambient_grid_samples <N>      = set hemisphere ray count per GRID point for macro ambient (default: 48)\n"
                "   ambient_samples <N> = set hemisphere ray count per LIGHTMAP TEXEL for macro ambient (default: 32)\n"
                "   ambient_testradius <F>       = set macro ambient occlusion ray length in world units (default: 512)\n"
                "   ambient_gatheradius <F> = set gather radius for spherical interpolation in world units (default: 256)\n"
                "   rad_voxelsize <F>    = set radiosity voxel size in world units (default: 256.0)\n"
                "                         Worldspawn: ambient_sky <R G B>\n"
                "   exposurefilter <type>   = highlight compression (softknee, reinhard, filmic)\n"
                "   saturation <F>   = set global light saturation multiplier (default: 1.0)\n"
                "   saturationramp <mode> = saturation roll-off curve (off, filmic, power, midtone)\n"
                "   lightmaprange    = normalize intensities to the peak light found\n"
                "   fast             = enable optimized (rasterized) voxelization and CSR filters\n");
        exit(0);
    }

    
    start = I_FloatTime();

    // Print active VFS paths
    {
        int p;
        for (p = 0; p < numVFSPaths; p++)
            _printf("vfsPath[%d]: %s\n", p, vfsPaths[p]);
        _printf("writedir: %s\n", writedir);
    }

#ifdef _WIN32
    {
        int p;
        for (p = 0; p < numVFSPaths; p++)
            InitPakFile(vfsPaths[p], NULL);
    }
#endif

    strcpy(source, ExpandArg(argv[i]));
    StripExtension(source);
    DefaultExtension(source, ".bsp");

    LoadShaderInfo();

    _printf("reading %s\n", source);
    LoadBSPFile(source);

    // Parse entity strings into structs
    ParseEntities();

    if (num_entities <= 0) {
        Error("map %s doesn't have a worldspawn\n", source);
    }

    ParseWorldspawnKeys(argc, argv);

    if (game->defaultSampleSize <= 0) {
        game->defaultSampleSize = 4;
        _printf("Defaulting lightmap sample size to %dx%d units (fallback)\n", game->defaultSampleSize, game->defaultSampleSize);
    }

    ThreadSetDefault();

    if (openclEnabled) {
        InitOpenCL();
    }

    // Consolidate per-surface metadata (Bounds, Entity Origins, Sidecar)
    // Must be called AFTER samplesize is determined so Patches can tessellate
    BuildLocalSurfaces();

    UpConvertLightingData();
    VoxelCache_BakeAll();

    // Call core lighting process
    LightMain();

    DownConvertLightingData();
    WriteBSPFile(source);

    ClearCacheDirectory();

    end = I_FloatTime();
    {
        int totalSeconds = (int)(end - start);
        int hours = totalSeconds / 3600;
        int minutes = (totalSeconds % 3600) / 60;
        int seconds = totalSeconds % 60;
        _printf("Total time elapsed: %02d:%02d:%02d\n", hours, minutes, seconds);
    }

    ShutdownOpenCL();
    Broadcast_Shutdown();
    return 0;
}
