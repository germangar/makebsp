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
#include "../shared/json_parser.h"
#include "radiosity.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

extern qboolean nodirect;
int radiosityPasses = 0;
extern tonemap_t tonemapMode;
qboolean g_fast = qfalse;

qboolean directonly = qfalse;
qboolean radiosityonly = qfalse;
qboolean ambientonly = qfalse;

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
        samplesize = atoi(val);
        _printf("Inferred lightmap sample size %dx%d from worldspawn (__texelsize)\n", samplesize, samplesize);
    }

    const char *lmSizeVal = ValueForKey(ent, "__lightmapImageSize");
    if (!lmSizeVal[0]) {
        Error("Worldspawn missing required key '__lightmapImageSize'.\n"
                "This BSP was likely compiled with an old version of q3map.\n"
                "Please re-run the BSP phase.");
    }
    int bspLmSize = atoi(lmSizeVal);
    if (bspLmSize != game->lightmapSize) {
        Error("Lightmap size mismatch!\n"
                "BSP was built for %dx%d lightmaps, but the lighting tool is configured for %dx%d.\n"
                "Check your game profile or -lightmapsize command line setting.",
                bspLmSize, bspLmSize, game->lightmapSize, game->lightmapSize);
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

    val = ValueForKey(ent, "upscale");
    if (val[0] && !HasArg("-upscale", argc, argv)) {
        game->upscale = atoi(val) != 0;
    }
}

int main(int argc, char **argv) {
    int i;
    double start, end;


    _printf("----- Lighting (Ag Build v1.1) ----\n");

    verbose = qfalse;
    areaScale = 0.25;

    openclEnabled = qtrue;

    // Initialize game profile from JSON and CLI
    game = InitGame(argc, argv);

    // Pre-scan CLI for path overrides
    const char *cliUserDir = NULL;
    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-basepath") || !strcmp(argv[i], "-rootdir")) && i + 1 < argc) {
            strcpy(rootDir, argv[i + 1]);
        } else if (!strcmp(argv[i], "-userdir") && i + 1 < argc) {
            cliUserDir = argv[i + 1];
        }
    }

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
        } else if (!strcmp(argv[i], "-upscale")) {
            game->upscale = qtrue;
            _printf("Upscale detail tracing enabled (2x grid)\n");
        } else if (!strcmp(argv[i], "-novertex")) {
            novertexlighting = qtrue;
            _printf("no vertex lighting = true\n");
        } else if (!strcmp(argv[i], "-nogrid")) {
            nogridlighting = qtrue;
            _printf("no grid lighting = true\n");
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
        } else if (!strcmp(argv[i], "-basepath") || !strcmp(argv[i], "-rootdir")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-basepath/-rootdir requires a directory path");
            i++; // Handled in pre-scan
        } else if (!strcmp(argv[i], "-userdir")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-userdir requires a directory path");
            i++; // Handled in pre-scan
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
            game->deluxeAmbientExaggerate = atof(argv[++i]);
            if (game->deluxeAmbientExaggerate < 0.0f) game->deluxeAmbientExaggerate = 0.0f;
            _printf("Deluxe Ambient Exaggerate multiplier set to %.2f\n", game->deluxeAmbientExaggerate);
        } else if (!strcmp(argv[i], "-deluxe_radiosity_exaggerate")) {
            if (i + 1 >= argc || argv[i+1][0] == '-') Error("-deluxe_radiosity_exaggerate requires a scalar factor");
            game->deluxeRadiosityExaggerate = atof(argv[++i]);
            if (game->deluxeRadiosityExaggerate < 0.0f) game->deluxeRadiosityExaggerate = 0.0f;
            _printf("Deluxe Radiosity Exaggerate multiplier set to %.2f\n", game->deluxeRadiosityExaggerate);
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
        } else if (!strcmp(argv[i], "-mao_samples")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-mao_samples requires a numeric value");
            mao_grid_samples = atoi(argv[i + 1]);
            if (mao_grid_samples < 4)   mao_grid_samples = 4;
            if (mao_grid_samples > 512) mao_grid_samples = 512;
            i++;
        } else if (!strcmp(argv[i], "-mao_ambient_samples")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-mao_ambient_samples requires a numeric value");
            mao_ambient_samples = atoi(argv[i + 1]);
            if (mao_ambient_samples < 4)   mao_ambient_samples = 4;
            if (mao_ambient_samples > 512) mao_ambient_samples = 512;
            i++;
        } else if (!strcmp(argv[i], "-mao_radius")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-mao_radius requires a numeric value");
            mao_radius = (float)atof(argv[i + 1]);
            if (mao_radius < 32.0f) mao_radius = 32.0f;
            _printf("MAO radius set to %.1f wu\n", mao_radius);
            i++;
        } else if (!strcmp(argv[i], "-mao_gather_radius")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-mao_gather_radius requires a numeric value");
            mao_gather_radius = (float)atof(argv[i + 1]);
            if (mao_gather_radius < 32.0f) mao_gather_radius = 32.0f;
            _printf("MAO gather radius set to %.1f wu\n", mao_gather_radius);
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
        } else if (!strcmp(argv[i], "-lightmaprange")) {
            game->hdr = HDR_8BIT;
        } else if (!strcmp(argv[i], "-fast")) {
            g_fast = qtrue;
            _printf("Optimized voxelization mode (FAST) enabled\n");
        } else if (!strcmp(argv[i], "-lowmem")) {
            g_lowmem = qtrue;
            _printf("Low-memory mode enabled (using memory-mapped files)\n");
        } else {
            break;
        }
    }
    


    ThreadSetDefault();
    if (openclEnabled) {
        InitOpenCL();
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
                "   upscale        = enable 2x lightmap upscaling for anti-aliasing\n"
                "   falloff <type>  = set the shading model (lambert, halflambert, quadratic, doublequadratic, unreal)\n"
                "   falloff_softbias <F> = override the default soft bias for the shading model\n"
                "   falloff_sun <type> = override the sun shading model\n"
                "   falloff_sun_softbias <F> = override the sun soft bias\n"
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
                "   mao_samples <N>      = set hemisphere ray count per GRID point for macro ambient (default: 48)\n"
                "   mao_ambient_samples <N> = set hemisphere ray count per LIGHTMAP TEXEL for macro ambient (default: 32)\n"
                "   mao_radius <F>       = set macro ambient occlusion ray length in world units (default: 512)\n"
                "   mao_gather_radius <F> = set gather radius for spherical interpolation in world units (default: 256)\n"
                "   rad_voxelsize <F>    = set radiosity voxel size in world units (default: 256.0)\n"
                "                         Worldspawn: _ambient_sky <R G B>, _ambient_ground <R G B>\n"
                "   exposurefilter <type>   = highlight compression (softknee, reinhard, filmic)\n"
                "   lightmaprange    = normalize intensities to the peak light found\n"
                "   fast             = enable optimized (rasterized) voxelization and CSR filters\n");
        exit(0);
    }

    
    start = I_FloatTime();

    if (!rootDir[0] && game->rootDir && game->rootDir[0]) {
        strcpy(rootDir, game->rootDir);
    }
    
    // Resolve base paths using game profile and CLI overrides
    const char *finalUserDir = cliUserDir ? cliUserDir : (game->userDir ? game->userDir : "");
    SetBasePaths(finalUserDir);

    if (game->gameDir[0] && strcmp(game->gameDir, ".")) {
        strcat(gamePath, game->gameDir);
        strcat(gamePath, "/");
        if (userPath[0]) {
            strcat(userPath, game->gameDir);
            strcat(userPath, "/");
        }
        if (writedir[0]) {
            strcat(writedir, game->gameDir);
            strcat(writedir, "/");
        }
    }

    _printf("rootDir: %s\n", rootDir);
    _printf("gamePath: %s\n", gamePath);
    if (userPath[0]) {
        _printf("userPath: %s\n", userPath);
    }
    _printf("writedir: %s\n", writedir);


#ifdef _WIN32
    if (userPath[0]) {
        InitPakFile(userPath, NULL);
    }
    InitPakFile(gamePath, NULL);
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

    if (samplesize <= 0) {
        samplesize = game->defaultSampleSize;
        _printf("Defaulting lightmap sample size to %dx%d units (from game profile)\n", samplesize, samplesize);
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
    return 0;
}
