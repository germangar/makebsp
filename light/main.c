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
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "../libs/pakstuff.h"
#endif

int main(int argc, char **argv) {
  int i;
  double start, end;

  _printf("----- Lighting (Ag Build v1.1) ----\n");

  // Default settings
  verbose = qfalse;
  extra = qfalse;
  areaScale = 0.25;
  pointScale = 7500;
  lightmapSmoothPasses = -1;
  lightmapSmoothRadius = -1.0f;

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
    } else if (!strcmp(argv[i], "-extra")) {
      extra = qtrue;
      _printf("Extra detail tracing\n");
    } else if (!strcmp(argv[i], "-samplesize")) {
      samplesize = atoi(argv[i + 1]);
      if (samplesize < 1)
        samplesize = 1;
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
      strcpy(gamedir, arg);
    } else if (!strcmp(argv[i], "-sRGB")) {
      g_game->lightmapsRGB = qtrue;
      lightmapsRGBOverridden = qtrue;
      _printf("sRGB lightmaps enabled\n");
    } else if (!strcmp(argv[i], "-falloff")) {
      char *arg = argv[++i];
      if (!strcmp(arg, "halflambert")) {
        g_game->falloff = FALLOFF_HALFLAMBERT;
        _printf("Half-Lambert attenuation enabled\n");
      } else if (!strcmp(arg, "lambert")) {
        g_game->falloff = FALLOFF_LAMBERT;
        _printf("Lambert attenuation enabled\n");
      } else if (!strcmp(arg, "quadratic")) {
        g_game->falloff = FALLOFF_QUADRATIC;
        _printf("Quadratic attenuation enabled\n");
      } else if (!strcmp(arg, "doublequadratic")) {
        g_game->falloff = FALLOFF_DOUBLEQUADRATIC;
        _printf("Double Quadratic attenuation enabled\n");
      } else if (!strcmp(arg, "unreal")) {
        g_game->falloff = FALLOFF_UNREAL;
        _printf("Unreal Windowed Inverse Square attenuation enabled\n");
      } else if (!strcmp(arg, "wrapped")) {
        g_game->falloff = FALLOFF_WRAPPED;
        _printf("Wrapped Lambert (0.5) attenuation enabled\n");
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
      _printf("Embree-accelerated tracing enabled\n");
    } else if (!strcmp(argv[i], "-bruteforce")) {
      bruteTrace = qtrue;
      _printf("BRUTE FORCE tracing enabled (all culling disabled)\n");
    } else if (!strcmp(argv[i], "-smooth")) {
      lightmapSmoothPasses = atoi(argv[i + 1]);
      if (lightmapSmoothPasses < 0)
        lightmapSmoothPasses = 0;
      i++;
      _printf("Lightmap smoothing passes set to %d\n", lightmapSmoothPasses);
    } else if (!strcmp(argv[i], "-smoothradius")) {
      lightmapSmoothRadius = (float)atof(argv[i + 1]);
      if (lightmapSmoothRadius < 0)
        lightmapSmoothRadius = 0;
      i++;
      _printf("Lightmap smoothing radius set to %.2f\n", lightmapSmoothRadius);
    } else {
      break;
    }
  }

  ThreadSetDefault();

  if (i != argc - 1) {
    _printf("usage: light [-<switch> [-<switch> ...]] <mapname>\n"
            "\n"
            "Switches:\n"
            "   v              = verbose output\n"
            "   threads <X>    = set number of threads to X\n"
            "   area <V>       = set the area light scale to V\n"
            "   point <W>      = set the point light scale to W\n"
            "   notrace        = don't cast any shadows\n"
            "   extra          = enable super sampling for anti-aliasing\n"
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
            "   embree         = use high-performance Embree tracing path (brute-force only)\n"
            "   smooth <N>     = set number of smoothing passes (default from game profile, -smooth 0 to disable)\n"
            "   smoothradius <R> = set smoothing radius (default from game profile)\n");
    exit(0);
  }

  start = I_FloatTime();

  SetQdirFromPath(argv[i]);

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
  _printf("Active game: %s (BSP format: %s)\n", g_game->arg, g_game->bspIdent);

  // Re-apply CLI overrides
  if (falloffOverridden) {
    g_game->falloff = overrideFalloff;
    _printf("Restoring CLI override: Falloff mode\n");
  }
  if (lightmapsRGBOverridden) {
    g_game->lightmapsRGB = qtrue;
    _printf("Restoring CLI override: sRGB lightmaps\n");
  }
  if (deluxeMapOverridden) {
    g_game->deluxeMap = qtrue;
    _printf("Restoring CLI override: Deluxemaps\n");
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

  if (lightmapSmoothPasses < 0) lightmapSmoothPasses = 0;
  if (lightmapSmoothRadius < 0.0f) lightmapSmoothRadius = 0;

  if (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) {
    _printf("Smoothing (%d passes, radius %.2f): ", lightmapSmoothPasses, lightmapSmoothRadius);
    for (int pnum = 1; pnum <= lightmapSmoothPasses; pnum++) {
        _printf("%d...", pnum);
        SmoothLightmaps();
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
