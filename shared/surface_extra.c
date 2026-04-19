#include "surface_extra.h"
#include "../common/cmdlib.h"
#include "../common/scriplib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static surfaceExtra_t *surfaceExtras = NULL;
static int numSurfaceExtras = 0;
static int maxSurfaceExtras = 0;

static void GrowSurfaceExtras(int num) {
    if (num < maxSurfaceExtras) return;

    int oldMax = maxSurfaceExtras;
    if (maxSurfaceExtras == 0) maxSurfaceExtras = 1024;
    while (maxSurfaceExtras <= num) maxSurfaceExtras *= 2;

    surfaceExtras = realloc(surfaceExtras, maxSurfaceExtras * sizeof(surfaceExtra_t));
    memset(surfaceExtras + oldMax, 0, (maxSurfaceExtras - oldMax) * sizeof(surfaceExtra_t));
    numSurfaceExtras = num + 1;
}

void SetSurfaceExtraRadFillMode(int surfaceNum, radFillMode_t mode) {
    if (surfaceNum < 0) return;
    GrowSurfaceExtras(surfaceNum);
    surfaceExtras[surfaceNum].radFillMode = mode;
}

radFillMode_t GetSurfaceExtraRadFillMode(int surfaceNum) {
    if (surfaceNum < 0 || surfaceNum >= numSurfaceExtras) return RAD_FILL_DEFAULT;
    return surfaceExtras[surfaceNum].radFillMode;
}

void WriteSurfaceExtraFile(const char *path) {
    char srfPath[1024];
    char baseName[256];
    FILE *f;
    int i;

    ExtractFileBase(path, baseName);
    sprintf(srfPath, "cache/%s.srf", baseName);
    // Ensure cache directory exists
    Q_mkdir("cache");

    f = fopen(srfPath, "w");
    if (!f) {
        printf("WARNING: Could not write surface extra file %s\n", srfPath);
        return;
    }

    for (i = 0; i < numSurfaceExtras; i++) {
        if (surfaceExtras[i].radFillMode != RAD_FILL_DEFAULT) {
            fprintf(f, "surface %d {\n", i);
            if (surfaceExtras[i].radFillMode == RAD_FILL_VOXEL) {
                fprintf(f, "\trad_fill voxel\n");
            } else if (surfaceExtras[i].radFillMode == RAD_FILL_BILINEAR) {
                fprintf(f, "\trad_fill bilinear\n");
            }
            fprintf(f, "}\n\n");
        }
    }

    fclose(f);
}

void LoadSurfaceExtraFile(const char *path) {
    char srfPath[1024];
    char baseName[256];
    
    ExtractFileBase(path, baseName);
    sprintf(srfPath, "cache/%s.srf", baseName);

    // If the directory doesn't exist, the file definitely doesn't exist
    if (!FileExists(srfPath)) {
        return;
    }
    
    LoadScriptFile(srfPath);

    while (GetToken(qtrue)) {
        if (strcmp(token, "surface") == 0) {
            GetToken(qfalse);
            int surfaceNum = atoi(token);
            GrowSurfaceExtras(surfaceNum);

            MatchToken("{");
            while (1) {
                if (!GetToken(qtrue)) break;
                if (strcmp(token, "}") == 0) break;

                if (strcmp(token, "rad_fill") == 0) {
                    GetToken(qfalse);
                    if (strcmp(token, "voxel") == 0) {
                        surfaceExtras[surfaceNum].radFillMode = RAD_FILL_VOXEL;
                    } else if (strcmp(token, "bilinear") == 0) {
                        surfaceExtras[surfaceNum].radFillMode = RAD_FILL_BILINEAR;
                    }
                }
            }
        }
    }
}

void ClearCacheDirectory(void) {
    _printf("Clearing cache directory...\n");
#ifdef _WIN32
    // Use PowerShell to clear directory contents silently
    system("powershell -NoProfile -Command \"if (Test-Path cache) { Get-ChildItem cache | Remove-Item -Force -Recurse }\"");
#else
    system("rm -rf cache/*");
#endif
}
