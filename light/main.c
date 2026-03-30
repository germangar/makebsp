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
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

extern qboolean upscale;

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
    superSampleMode = SUPERSAMPLE_MODELS;
    embree = qtrue;

    JSON_ExportStandardPackages("games");
    JSON_LoadPackages("games");

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-tempname")) {
            i++;
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
        } else if (!strcmp(argv[i], "-samplesize")) {
            samplesize = atoi(argv[i + 1]);
            if (samplesize < 1) samplesize = 1;
            i++;
            _printf("lightmap sample size is %dx%d units\n", samplesize, samplesize);
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
        } else if (!strcmp(argv[i], "-oldtrace")) {
            oldTrace = qtrue;
            _printf("Legacy BSP-brush tracing enabled\n");
        } else if (!strcmp(argv[i], "-embree")) {
            embree = qtrue;
        } else if (!strcmp(argv[i], "-surface")) {
            embree = qfalse;
        } else if (!strcmp(argv[i], "-bruteforce")) {
            bruteTrace = qtrue;
            _printf("BRUTE FORCE tracing enabled (all culling disabled)\n");
        } else if (!strcmp(argv[i], "-smooth")) {
            int mode = atoi(argv[i + 1]);
            if (mode == 1) {
                superSampleMode = SUPERSAMPLE_MODELS; // Combined: Smoothing (world) + Super-sampling (models)
                lightmapSmoothPasses = -1; // Use g_game defaults
            } else if (mode == 2) {
                superSampleMode = SUPERSAMPLE_ALL; // Trace-time Super-sampling for EVERYTHING (no smoothing)
                lightmapSmoothPasses = 0; 
            } else if (mode == 0) {
                superSampleMode = SUPERSAMPLE_NONE; // OFF
                lightmapSmoothPasses = 0;
            }
            i++;
        } else if (!strcmp(argv[i], "-smoothradius")) {
            lightmapSmoothRadius = (float)atof(argv[i + 1]);
            if (lightmapSmoothRadius < 0)
                lightmapSmoothRadius = 0;
            i++;
        } else {
            break;
        }
    }

    ThreadSetDefault();

    if (i != argc - 1) {
        if (i < argc) {
            _printf("Error: Unrecognized switch or extra argument '%s'\n", argv[i]);
        }
        _printf("usage: light [-<switch> [-<switch> ...]] <mapname>\n"
                "\n"
                "Switches:\n"
                "   v              = verbose output\n"
                "   threads <X>    = set number of threads to X\n"
                "   area <V>       = set the area light scale to V\n"
                "   point <W>      = set the point light scale to W\n"
                "   notrace        = don't cast any shadows\n"
                "   upscale        = enable 2x lightmap upscaling for anti-aliasing\n"
                "   nogrid         = don't calculate light grid for dynamic model "
                "lighting\n"
                "   novertex       = don't calculate vertex lighting\n"
                "   samplesize <N> = set the lightmap pixel size to NxN units\n"
                "   falloff <type>  = set the falloff model (lambert, halflambert,\n"
                "                     quadratic, doublequadratic, unreal, wrapped)\n"
                "   brutetrace      = disable all tracing optimizations for debugging\n"
                "   debuglightmaps = generate BMP files showing lightmap allocation (FAST)\n"
                "   debuglightmapsalpha = generate BMP files showing exact lit pixels (SLOW)\n"
                "   oldtrace       = use legacy BSP-brush occlusion for all surfaces\n"
                "   bruteforce     = skip all culling and use legacy trace\n"
                "   embree         = use high-performance Embree tracing path (DEFAULT)\n"
                "   surface        = use legacy surface tracing path\n"
                "   smooth <mode>  = lightmap anti-aliasing mode:\n"
                "                     0 = OFF\n"
                "                     1 = (DEFAULT) smoothing world + super-sampling models\n"
                "                     2 = super-sampling EVERYTHING (post-process smoothing OFF)\n"
                "   smoothradius <R> = set radius for blurring (world) and jitter (super-sampling)\n");
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
    UpConvertLightingData();

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
    if (oldTrace || bruteTrace) tLog = "Legacy";
    else if (embree) tLog = "Embree";

    _printf("Active game: %s (BSP format: %s)\n", g_game->arg, g_game->bspIdent);
    _printf("Falloff mode: %s\n", fLog);
    _printf("Lighting flags: %s %s %s\n", 
            g_game->lightmapsRGB ? "sRGB" : "Linear",
            g_game->deluxeMap ? "Deluxe" : "Standard",
            tLog);
    if (lightmapSmoothPasses < 0) lightmapSmoothPasses = g_game->defaultSmoothPasses;
    if (lightmapSmoothRadius < 0.0f) lightmapSmoothRadius = g_game->defaultSmoothRadius;

    if (superSampleMode != SUPERSAMPLE_NONE) {
        const char *modeLog = (superSampleMode == SUPERSAMPLE_ALL) ? "Everything" : "Models Only";
        int ssCnt = (lightmapSmoothRadius >= 2.0f) ? 16 : 8;
        _printf("Super-sampling Mode %d (%s): %d samples per texel (radius %.2f)\n", superSampleMode, modeLog, ssCnt, lightmapSmoothRadius);
    }

    if (samplesize == 0) {
        samplesize = g_game->defaultSampleSize;
        _printf("Defaulting lightmap sample size to %dx%d units\n", samplesize,
                samplesize);
    }

    // Parse entity strings into structs
    ParseEntities();

    // Call core lighting process
    LightMain();

    if (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) {
        _printf("Smoothing (%d passes, radius %.2f): ", lightmapSmoothPasses, lightmapSmoothRadius);
        for (int pnum = 1; pnum <= lightmapSmoothPasses; pnum++) {
            _printf("%d...", pnum);
            SmoothLightmaps(lightmapSmoothRadius);
        }
        _printf(" Done\n");
    }

    _printf("writing %s\n", source);
    DownConvertLightingData();
    WriteBSPFile(source);

    end = I_FloatTime();
    _printf("%5.0f seconds elapsed\n", end - start);

    return 0;
}
