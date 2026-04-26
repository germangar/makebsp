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
#include "../shared/surface_extra.h"
#include "radiosity.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

extern qboolean upscale;
int radiosityPasses = 0;
extern tonemap_t tonemapMode;

qboolean rad_voxel = qtrue;

int main(int argc, char **argv) {
    int i;
    double start, end;

    _printf("----- Lighting (Ag Build v1.1) ----\n");

    verbose = qfalse;
    upscale = qfalse;
    areaScale = 0.25;
    pointScale = 7500;
    lightmapSmoothPasses = -1;
    lightmapSmoothRadius = -1.0f;
    superSampleMode = SUPERSAMPLE_NONE;
    embree = qtrue;
    openclEnabled = qtrue;

    JSON_ExportStandardPackages("games");
    JSON_LoadPackages("games");

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
        } else if (!strcmp(argv[i], "-area")) {
            areaScale *= atof(argv[i + 1]);
            _printf("area light scaling at %f\n", areaScale);
            i++;
        } else if (!strcmp(argv[i], "-point")) {
            pointScale *= atof(argv[i + 1]);
            _printf("point light scaling at %f\n", pointScale);
            i++;
        } else if (!strcmp(argv[i], "-notrace")) {
            notrace = qtrue;
            _printf("No occlusion tracing\n");
        } else if (!strcmp(argv[i], "-upscale")) {
            upscale = qtrue;
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
            debugLightmaps = qtrue;
            debugLightmapsAlpha = qtrue;
            _printf("Lightmap debug visualization enabled (ALPHA/ACCURATE mode)\n");
        } else if (!strcmp(argv[i], "-game")) {
            char *arg = argv[++i];
            int j;
            qboolean found = qfalse;
            for (j = 0; j < numGames; j++) {
                if (games[j].arg && !strcmp(games[j].arg, arg)) {
                    g_game = &games[j];
                    found = qtrue;
                    break;
                }
            }
            if (!found) {
                Error("Unknown game: %s", arg);
            }
        } else if (!strcmp(argv[i], "-sRGB")) {
            g_game->lightmapsRGB = qtrue;
            lightmapsRGBOverridden = qtrue;
        } else if (!strcmp(argv[i], "-falloff")) {
            char *arg = argv[++i];
            if (!strcmp(arg, "halflambert")) {
                g_game->falloff = FALLOFF_HALFLAMBERT;
            } else if (!strcmp(arg, "lambert")) {
                g_game->falloff = FALLOFF_LAMBERT;
            } else if (!strcmp(arg, "quadratic")) {
                g_game->falloff = FALLOFF_QUADRATIC;
            } else if (!strcmp(arg, "doublequadratic")) {
                g_game->falloff = FALLOFF_DOUBLEQUADRATIC;
            } else if (!strcmp(arg, "unreal")) {
                g_game->falloff = FALLOFF_UNREAL;
            } else if (!strcmp(arg, "wrapped")) {
                g_game->falloff = FALLOFF_WRAPPED;
            } else {
                Error("Unknown falloff type: %s", arg);
            }
            falloffOverridden = qtrue;
            overrideFalloff = g_game->falloff;
        } else if (!strcmp(argv[i], "-deluxe")) {
            g_game->deluxeMap = qtrue;
            deluxeMapOverridden = qtrue;
            _printf("Deluxemaps enabled\n");
        } else if (!strcmp(argv[i], "-embree")) {
            embree = qtrue;
        } else if (!strcmp(argv[i], "-surface")) {
            embree = qfalse;
        } else if (!strcmp(argv[i], "-bruteforce")) {
            bruteTrace = qtrue;
            _printf("BRUTE FORCE tracing enabled (all culling disabled)\n");
        } else if (!strcmp(argv[i], "-supersampling")) {
            int mode = atoi(argv[i + 1]);
            if (mode == 1) {
                superSampleMode = SUPERSAMPLE_ALL;
                _printf("Super-sampling enabled for ALL surfaces\n");
            } else if (mode == 2) {
                superSampleMode = SUPERSAMPLE_MODELS;
                _printf("Super-sampling enabled for MODELS only\n");
            } else {
                superSampleMode = SUPERSAMPLE_NONE;
            }
            i++;
        } else if (!strcmp(argv[i], "-smooth")) {
            lightmapSmoothPasses = atoi(argv[i + 1]);
            if (lightmapSmoothPasses < 0) lightmapSmoothPasses = 0;
            _printf("Smoothing passes set to %d\n", lightmapSmoothPasses);
            i++;
        } else if (!strcmp(argv[i], "-aa")) {
            lightmapAA = atoi(argv[i + 1]);
            _printf("Anti-Aliasing post-process pass enabled (Mode %d)\n", lightmapAA);
            i++;
        } else if (!strcmp(argv[i], "-smoothradius")) {
            lightmapSmoothRadius = (float)atof(argv[i + 1]);
            if (lightmapSmoothRadius < 0)
                lightmapSmoothRadius = 0;
            i++;
        } else if (!strcmp(argv[i], "-radiosity")) {
            radiosityPasses = atoi(argv[i + 1]);
            if (radiosityPasses < 0)
                radiosityPasses = 0;
            i++;
        } else if (!strcmp(argv[i], "-rad_depthmin")) {
            rad_depth_min = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_depthmax")) {
            rad_depth_max = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_min_energy")) {
            rad_min_energy = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_interval")) {
            rad_interval = atoi(argv[i + 1]);
            if (rad_interval < 1) rad_interval = 1;
            i++;
        } else if (!strcmp(argv[i], "-rad_color_ratio")) {
            rad_color_ratio = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_bounce_scale")) {
            rad_bounce_scale = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_depthintensity")) {
            rad_depth_intensity = (float)atof(argv[i + 1]);
            if (rad_depth_intensity < 0.0f) rad_depth_intensity = 0.0f;
            if (rad_depth_intensity > 1.0f) rad_depth_intensity = 1.0f;
            i++;
        } else if (!strcmp(argv[i], "-rad_fill")) {
            const char *mode = argv[++i];
            if (!strcmp(mode, "voxel")) {
                rad_voxel = qtrue;
                _printf("Voxel reconstruction fill enabled\n");
            } else if (!strcmp(mode, "bilinear")) {
                rad_voxel = qfalse;
                _printf("Bilinear interpolation fill enabled\n");
            } else {
                Error("Unknown rad_fill mode: %s (use 'voxel' or 'bilinear')", mode);
            }
        } else if (!strcmp(argv[i], "-rad_voxelsize")) {
            rad_voxel_size = (float)atof(argv[i + 1]);
            if (rad_voxel_size < 0.1f) rad_voxel_size = 0.1f;
            i++;
        } else if (!strcmp(argv[i], "-rad_anglematch")) {
            rad_angle_match = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-exposurefilter")) {
            const char *mode = argv[i + 1];
            if (!strcmp(mode, "softknee")) {
                tonemapMode = TONEMAP_SOFTKNEE;
            } else if (!strcmp(mode, "reinhard")) {
                tonemapMode = TONEMAP_REINHARD;
            } else if (!strcmp(mode, "filmic")) {
                tonemapMode = TONEMAP_FILMIC;
            } else {
                tonemapMode = TONEMAP_LINEAR;
            }
            i++;
        } else if (!strcmp(argv[i], "-lightmaprange")) {
            g_game->hdr = HDR_8BIT;
        } else {
            break;
        }
    }
    
    if (tonemapMode == (tonemap_t)-1) {
        tonemapMode = g_game->exposureFilter;
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
                "   point <W>      = set the point light scale to W\n"
                "   notrace        = don't cast any shadows\n"
                "   upscale        = enable 2x lightmap upscaling for anti-aliasing\n"
                "   nogrid         = don't calculate light grid for dynamic model "
                "lighting\n"
                "   novertex       = don't calculate vertex lighting\n"
                "   falloff <type>  = set the falloff model (lambert, halflambert,\n"
                "                     quadratic, doublequadratic, unreal, wrapped)\n"
                "   brutetrace      = disable all tracing optimizations for debugging\n"
                "   debuglightmaps = generate BMP files showing lightmap allocation (FAST)\n"
                "   debuglightmapsalpha = generate BMP files showing exact lit pixels (SLOW)\n"
                "   bruteforce     = skip all culling and use legacy trace\n"
                "   embree         = use high-performance Embree tracing path (DEFAULT)\n"
                "   surface        = use legacy surface tracing path\n"
                "   smooth <passes> = number of post-process smoothing passes to run\n"
                "   smoothradius <R> = set radius for blurring (world) and jitter (super-sampling)\n"
                "   supersampling <mode> = trace-time super-sampling mode:\n"
                "                     0 = OFF\n"
                "                     1 = super-sampling EVERYTHING\n"
                "                     2 = (DEFAULT) super-sampling models only\n"
                "   radiosity <N>    = set the number of radiosity passes (high-fidelity bounce)\n"
                "   rad_depthmin <F> = set inner distance limit for radiosity plateau\n"
                "   rad_depthmax <F> = set outer distance limit for radiosity gradient\n"
                "   rad_min_energy <F>= set min luxel energy to spawn an emitter\n"
                "   -rad_interval <I>  = set sparse grid interval (1=Every luxel, 4=4x4 blocks)\n"
                "   rad_color_ratio <F>= set greyscale(0.0) vs color(1.0) bleeding\n"
                "   rad_fill <mode>   = GI reconstruction mode ('voxel' or 'bilinear')\n"
                "   rad_voxelsize <F> = set the world-space size of reconstruction voxels\n"
                "   rad_anglematch <A>= set the angle in degrees for surface compatibility\n"
                "   rad_bounce_scale <F>= set final bounce energy multiplier\n"
                "   rad_depthintensity <F>= set min bounce carryover (0.0 to 1.0) for creases\n"
                "   exposurefilter <type>   = highlight compression (softknee, reinhard, filmic)\n"
                "   lightmaprange    = normalize intensities to the peak light found\n");
        exit(0);
    }

    start = I_FloatTime();

    SetQdirFromPath(argv[i]);
    if (g_game->gamePath[0] && strcmp(g_game->gamePath, ".")) {
        strcat(gamedir, g_game->gamePath);
        strcat(gamedir, "/");
    }

#ifdef _WIN32
    InitPakFile(gamedir, NULL);
#endif

    strcpy(source, ExpandArg(argv[i]));
    StripExtension(source);
    DefaultExtension(source, ".bsp");

    LoadShaderInfo();

    _printf("reading %s\n", source);

    LoadBSPFile(source);

    {
        char mapName[1024];
        strcpy(mapName, ExpandArg(argv[i]));
        StripExtension(mapName);
        LoadSurfaceExtraFile(mapName);
    }

    // Re-apply CLI overrides (user choice takes priority over JSON/header defaults)
    if (falloffOverridden) g_game->falloff = overrideFalloff;
    if (lightmapsRGBOverridden) g_game->lightmapsRGB = qtrue;
    if (deluxeMapOverridden) g_game->deluxeMap = qtrue;

    // Print active configuration summary
    const char *fLog = "lambert";
    if (g_game->falloff == FALLOFF_HALFLAMBERT) fLog = "halflambert";
    else if (g_game->falloff == FALLOFF_QUADRATIC) fLog = "quadratic";
    else if (g_game->falloff == FALLOFF_DOUBLEQUADRATIC) fLog = "doublequadratic";
    else if (g_game->falloff == FALLOFF_UNREAL) fLog = "unreal";
    else if (g_game->falloff == FALLOFF_WRAPPED) fLog = "wrapped";

    const char *tLog = "Surface";
    if (bruteTrace) tLog = "Legacy";
    else if (embree) tLog = "Embree";

    _printf("Active game: %s (BSP format: %s)\n", g_game->arg, g_game->bspIdent);
    _printf("Falloff mode: %s\n", fLog);
    _printf("Lighting flags: %s %s %s %s\n", 
            g_game->lightmapsRGB ? "sRGB" : "Linear",
            g_game->deluxeMap ? "Deluxe" : "Standard",
            (g_game->hdr == HDR_8BIT) ? "range" : "clamped",
            tLog);
    if (lightmapSmoothPasses < 0) lightmapSmoothPasses = g_game->defaultSmoothPasses;
    if (lightmapSmoothRadius < 0.0f) lightmapSmoothRadius = g_game->defaultSmoothRadius;

    if (superSampleMode != SUPERSAMPLE_NONE) {
        const char *modeLog = (superSampleMode == SUPERSAMPLE_ALL) ? "Everything" : "Models Only";
        int ssCnt = (lightmapSmoothRadius >= 2.0f) ? 16 : 8;
        _printf("Super-sampling Mode %d (%s): %d samples per texel (radius %.2f)\n", superSampleMode, modeLog, ssCnt, lightmapSmoothRadius);
    }

    // Parse entity strings into structs
    ParseEntities();

    // Determine samplesize from worldspawn or game default
    if (num_entities > 0) {
        const char *val = ValueForKey(&entities[0], "__texelsize");
        if (val[0]) {
            samplesize = atoi(val);
            _printf("Inferred lightmap sample size %dx%d from worldspawn (__texelsize)\n", samplesize, samplesize);
        }
    }

    if (samplesize <= 0) {
        samplesize = g_game->defaultSampleSize;
        _printf("Defaulting lightmap sample size to %dx%d units (from game profile)\n", samplesize, samplesize);
    }

    UpConvertLightingData();

    // Call core lighting process
    LightMain(radiosityPasses);

    _printf("writing %s\n", source);
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
