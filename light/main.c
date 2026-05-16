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

extern qboolean upscale;
int radiosityPasses = 0;
extern tonemap_t tonemapMode;
qboolean g_fast = qfalse;

int main(int argc, char **argv) {
    int i;
    double start, end;


    _printf("----- Lighting (Ag Build v1.1) ----\n");

    verbose = qfalse;
    upscale = qfalse;
    areaScale = 0.25;
    pointScale = 7500;

    openclEnabled = qtrue;


    // Initialize game profile from JSON and CLI
    game = InitGame(argc, argv);

    superSampleMode = SUPERSAMPLE_NONE;

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
            debugLightmapsAlpha = qtrue;
            _printf("Lightmap debug visualization enabled (ALPHA/ACCURATE mode)\n");
        } else if (!strcmp(argv[i], "-game")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-game requires a profile name");
            i++; // Handled in pre-scan
        } else if (!strcmp(argv[i], "-sRGB")) {
            game->lightmapsRGB = qtrue;
        } else if (!strcmp(argv[i], "-falloff")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-falloff requires a type (lambert, halflambert, etc.)");
            char *arg = argv[++i];
            if (!strcmp(arg, "halflambert")) {
                game->falloff = FALLOFF_HALFLAMBERT;
                falloffSoftBias = FALLOFF_HALFLAMBERT_SOFTBIAS;
                sunSoftBias = FALLOFF_HALFLAMBERT_SOFTBIAS;
            } else if (!strcmp(arg, "lambert")) {
                game->falloff = FALLOFF_LAMBERT;
                falloffSoftBias = FALLOFF_LAMBERT_SOFTBIAS;
                sunSoftBias = FALLOFF_LAMBERT_SOFTBIAS;
            } else if (!strcmp(arg, "quadratic")) {
                game->falloff = FALLOFF_QUADRATIC;
                falloffSoftBias = FALLOFF_QUADRATIC_SOFTBIAS;
                sunSoftBias = FALLOFF_QUADRATIC_SOFTBIAS;
            } else if (!strcmp(arg, "doublequadratic")) {
                game->falloff = FALLOFF_DOUBLEQUADRATIC;
                falloffSoftBias = FALLOFF_DOUBLEQUADRATIC_SOFTBIAS;
                sunSoftBias = FALLOFF_DOUBLEQUADRATIC_SOFTBIAS;
            } else if (!strcmp(arg, "unreal")) {
                game->falloff = FALLOFF_UNREAL;
                falloffSoftBias = FALLOFF_UNREAL_SOFTBIAS;
                sunSoftBias = FALLOFF_UNREAL_SOFTBIAS;
            } else {
                Error("Unknown falloff type: %s", arg);
            }
        } else if (!strcmp(argv[i], "-falloff_softbias")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-falloff_softbias requires a numeric value");
            falloffSoftBias = (float)atof(argv[i + 1]);
            if (falloffSoftBias < 0.0f) falloffSoftBias = 0.0f;
            if (falloffSoftBias > 1.0f) falloffSoftBias = 1.0f;
            i++;
        } else if (!strcmp(argv[i], "-falloff_sun_softbias")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-falloff_sun_softbias requires a numeric value");
            sunSoftBias = (float)atof(argv[i + 1]);
            if (sunSoftBias < 0.0f) sunSoftBias = 0.0f;
            if (sunSoftBias > 1.0f) sunSoftBias = 1.0f;
            i++;
        } else if (!strcmp(argv[i], "-falloff_sun")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-falloff_sun requires a type (lambert, halflambert, etc.)");
            char *arg = argv[++i];
            if (!strcmp(arg, "halflambert")) {
                game->sunFalloff = FALLOFF_HALFLAMBERT;
            } else if (!strcmp(arg, "lambert")) {
                game->sunFalloff = FALLOFF_LAMBERT;
            } else if (!strcmp(arg, "quadratic")) {
                game->sunFalloff = FALLOFF_QUADRATIC;
            } else if (!strcmp(arg, "doublequadratic")) {
                game->sunFalloff = FALLOFF_DOUBLEQUADRATIC;
            } else if (!strcmp(arg, "unreal")) {
                game->sunFalloff = FALLOFF_UNREAL;
            } else {
                Error("Unknown sun falloff type: %s", arg);
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
        } else if (!strcmp(argv[i], "-supersampling")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-supersampling requires a mode (0, 1, or 2)");
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
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-smooth requires a number of passes");
            game->defaultSmoothPasses = atoi(argv[i + 1]);
            if (game->defaultSmoothPasses < 0) game->defaultSmoothPasses = 0;
            _printf("Smoothing passes set to %d\n", game->defaultSmoothPasses);
            i++;
        } else if (!strcmp(argv[i], "-antialiasing")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-antialiasing requires a number of passes");
            game->antialiasingPasses = atoi(argv[i + 1]);
            _printf("Anti-Aliasing post-process pass enabled (Mode %d)\n", game->antialiasingPasses);
            i++;
        } else if (!strcmp(argv[i], "-smoothradius")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-smoothradius requires a radius value");
            game->defaultSmoothRadius = (float)atof(argv[i + 1]);
            if (game->defaultSmoothRadius < 0.1f)
                game->defaultSmoothRadius = 0.1f;
            i++;
        } else if (!strcmp(argv[i], "-radiosity")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-radiosity requires a number of passes");
            game->radiosityPasses = atoi(argv[i + 1]);
            if (game->radiosityPasses < 0)
                game->radiosityPasses = 0;
            i++;
        } else if (!strcmp(argv[i], "-rad_depthmin")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_depthmin requires a numeric value");
            rad_depth_min = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_depthmax")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_depthmax requires a numeric value");
            rad_depth_max = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_min_energy")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_min_energy requires a numeric value");
            rad_min_energy = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_interval")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_interval requires a numeric value");
            rad_interval = atoi(argv[i + 1]);
            if (rad_interval < 1) rad_interval = 1;
            i++;
        } else if (!strcmp(argv[i], "-rad_color_ratio")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_color_ratio requires a numeric value");
            game->radiosityColorRatio = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_intensity")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_intensity requires a numeric value");
            game->radiosityIntensity = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-rad_depthintensity")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_depthintensity requires a numeric value");
            rad_depth_intensity = (float)atof(argv[i + 1]);
            if (rad_depth_intensity < 0.0f) rad_depth_intensity = 0.0f;
            if (rad_depth_intensity > 1.0f) rad_depth_intensity = 1.0f;
            i++;
        } else if (!strcmp(argv[i], "-rad_voxelsize")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_voxelsize requires a numeric value");
            rad_voxel_size = (float)atof(argv[i + 1]);
            if (rad_voxel_size < 0.1f) rad_voxel_size = 0.1f;
            i++;
        } else if (!strcmp(argv[i], "-rad_anglematch")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-rad_anglematch requires a numeric value");
            rad_angle_match = (float)atof(argv[i + 1]);
            i++;
        } else if (!strcmp(argv[i], "-exposurefilter")) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') Error("-exposurefilter requires a mode (softknee, reinhard, or filmic)");
            const char *mode = argv[i + 1];
            if (!strcmp(mode, "softknee")) {
                game->exposureFilter = TONEMAP_SOFTKNEE;
            } else if (!strcmp(mode, "reinhard")) {
                game->exposureFilter = TONEMAP_REINHARD;
            } else if (!strcmp(mode, "filmic")) {
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
                "   point <W>      = set the point light scale to W\n"
                "   notrace        = don't cast any shadows\n"
                "   upscale        = enable 2x lightmap upscaling for anti-aliasing\n"
                "   falloff <type>  = set the falloff model (lambert, halflambert, quadratic, doublequadratic, unreal)\n"
                "   falloff_softbias <F> = override the default soft bias for the falloff model\n"
                "   falloff_sun <type> = override the sun falloff model\n"
                "   falloff_sun_softbias <F> = override the sun soft bias\n"
                "   deluxe <0|1>    = enable (1) or disable (0) deluxemapping\n"
                "   brutetrace      = disable all tracing optimizations for debugging\n"
                "   debuglightmaps = generate BMP files showing lightmap allocation (FAST)\n"
                "   debuglightmapsalpha = generate BMP files showing exact lit pixels (SLOW)\n"
                "   smooth <passes> = number of post-process smoothing passes to run\n"
                "   smoothradius <R> = set radius for blurring (world) and jitter (super-sampling)\n"
                "   antialiasing <passes> = number of anti-aliasing post-process passes to run\n"
                "   supersampling <mode> = trace-time super-sampling mode:\n"
                "                     0 = OFF\n"
                "                     1 = super-sampling EVERYTHING\n"
                "                     2 = super-sampling models only\n"
                "   radiosity <N>    = set the number of radiosity passes (high-fidelity bounce)\n"
                "   rad_depthmin <F> = set inner distance limit for radiosity plateau\n"
                "   rad_depthmax <F> = set outer distance limit for radiosity gradient\n"
                "   rad_min_energy <F>= set min luxel energy to spawn an emitter\n"
                "   rad_interval <I>  = set sparse grid interval (1=Every luxel, 4=4x4 blocks)\n"
                "   rad_color_ratio <F>= set greyscale(0.0) vs color(1.0) bleeding\n"
                "   rad_voxelsize <F> = set the world-space size of reconstruction voxels\n"
                "   rad_anglematch <A>= set the angle in degrees for surface compatibility\n"
                "   rad_bounce_scale <F>= set final bounce energy multiplier\n"
                "   rad_depthintensity <F>= set min bounce carryover (0.0 to 1.0) for creases\n"
                "   exposurefilter <type>   = highlight compression (softknee, reinhard, filmic)\n"
                "   lightmaprange    = normalize intensities to the peak light found\n"
                "   fast             = enable optimized (rasterized) voxelization and CSR filters\n");
        exit(0);
    }

    start = I_FloatTime();

    SetQdirFromPath(argv[i]);
    if (game->gamePath[0] && strcmp(game->gamePath, ".")) {
        strcat(gamedir, game->gamePath);
        strcat(gamedir, "/");
    }

#ifdef _WIN32
    InitPakFile(gamedir, NULL);
#endif

    strcpy(source, ExpandArg(argv[i]));
    StripExtension(source);
    DefaultExtension(source, ".bsp");

    LoadShaderInfo();



    // Print active configuration summary
    const char *fLog = "lambert";
    if (game->falloff == FALLOFF_HALFLAMBERT) fLog = "halflambert";
    else if (game->falloff == FALLOFF_QUADRATIC) fLog = "quadratic";
    else if (game->falloff == FALLOFF_DOUBLEQUADRATIC) fLog = "doublequadratic";
    else if (game->falloff == FALLOFF_UNREAL) fLog = "unreal";

    _printf("Active game: %s (BSP format: %s)\n", game->arg, game->bspIdent);
    _printf("Falloff mode: %s (Bias %.2f)\n", fLog, falloffSoftBias);
    
    const char *sfLog = "lambert";
    if (game->sunFalloff == FALLOFF_HALFLAMBERT) sfLog = "halflambert";
    else if (game->sunFalloff == FALLOFF_QUADRATIC) sfLog = "quadratic";
    else if (game->sunFalloff == FALLOFF_DOUBLEQUADRATIC) sfLog = "doublequadratic";
    else if (game->sunFalloff == FALLOFF_UNREAL) sfLog = "unreal";
    
    _printf("Sun Falloff mode: %s (Bias %.2f)\n", sfLog, sunSoftBias);
    _printf("Lighting flags: %s %s %s\n", 
            game->lightmapsRGB ? "sRGB" : "Linear",
            game->deluxeMap ? "Deluxe" : "Standard",
            (game->hdr == HDR_8BIT) ? "range" : "clamped");
    _printf("Lightmap size: %d (Write: %d)\n", game->lightmapSize, game->writeLightmapSize);



    _printf("Smoothing: %d passes (radius %.2f), AA: %d passes\n", game->defaultSmoothPasses, game->defaultSmoothRadius, game->antialiasingPasses);
    _printf("Radiosity: %d passes (intensity %.2f, color ratio %.2f)\n", game->radiosityPasses, game->radiosityIntensity, game->radiosityColorRatio);

    if (superSampleMode != SUPERSAMPLE_NONE) {
        const char *modeLog = (superSampleMode == SUPERSAMPLE_ALL) ? "Everything" : "Models Only";
        int ssCnt = (game->defaultSmoothRadius >= 2.0f) ? 16 : 8;
        _printf("Super-sampling Mode %d (%s): %d samples per texel (radius %.2f)\n", superSampleMode, modeLog, ssCnt, game->defaultSmoothRadius);
    }

    _printf("reading %s\n", source);
    LoadBSPFile(source);

    // Parse entity strings into structs
    ParseEntities();

    // Determine samplesize and lightmap size from worldspawn or game default
    if (num_entities > 0) {
        const char *val = ValueForKey(&entities[0], "__texelsize");
        if (val[0]) {
            samplesize = atoi(val);
            _printf("Inferred lightmap sample size %dx%d from worldspawn (__texelsize)\n", samplesize, samplesize);
        }

        const char *lmSizeVal = ValueForKey(&entities[0], "__lightmapImageSize");
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
        _printf("Verified lightmap image size %dx%d from worldspawn (__lightmapImageSize)\n", bspLmSize, bspLmSize);
    }

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
