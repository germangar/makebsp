#include "surface_extra.h"
#include "../common/cmdlib.h"
#include "../common/scriplib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LIGHT_TOOL
#include "../light/light.h"
#elif defined(Q3MAP_TOOL)
#include "../q3map/qbsp.h"
#endif

// ---------------------------------------------------------------------------
// Writing Logic (Used by Q3MAP_TOOL)
// ---------------------------------------------------------------------------

void WriteSurfaceExtraFile(const char *path) {
#ifdef Q3MAP_TOOL
    char srfPath[1024];
    char baseName[256];
    FILE *f;
    int i;

    ExtractFileBase(path, baseName);
    sprintf(srfPath, "cache/%s.srf", baseName);
    Q_mkdir("cache");

    f = fopen(srfPath, "w");
    if (!f) {
        _printf("WARNING: Could not write surface extra file %s\n", srfPath);
        return;
    }

    for (i = 0; i < numMapDrawSurfs; i++) {
        if (mapDrawSurfs[i].radFillMode != RAD_FILL_DEFAULT) {
            fprintf(f, "surface %d {\n", i);
            if (mapDrawSurfs[i].radFillMode == RAD_FILL_VOXEL) {
                fprintf(f, "\trad_fill voxel\n");
            } else if (mapDrawSurfs[i].radFillMode == RAD_FILL_BILINEAR) {
                fprintf(f, "\trad_fill bilinear\n");
            }
            fprintf(f, "}\n\n");
        }
    }

    fclose(f);
#endif
}

// ---------------------------------------------------------------------------
// Loading Logic (Used by LIGHT_TOOL)
// ---------------------------------------------------------------------------

void LoadSurfaceExtraFile(const char *path) {
#ifdef LIGHT_TOOL
    char srfPath[1024];
    char baseName[256];
    
    ExtractFileBase(path, baseName);
    sprintf(srfPath, "cache/%s.srf", baseName);

    if (!FileExists(srfPath)) {
        return;
    }
    
    LoadScriptFile(srfPath);

    while (GetToken(qtrue)) {
        if (strcmp(token, "surface") == 0) {
            GetToken(qfalse);
            int surfaceNum = atoi(token);
            
            if (surfaceNum < 0 || surfaceNum >= numDrawSurfaces) {
                _printf("WARNING: sidecar surface index %d out of range (max %d)\n", surfaceNum, numDrawSurfaces - 1);
                // skip block
                MatchToken("{");
                while (GetToken(qtrue)) {
                    if (strcmp(token, "}") == 0) break;
                }
                continue;
            }

            MatchToken("{");
            while (1) {
                if (!GetToken(qtrue)) break;
                if (strcmp(token, "}") == 0) break;

                if (strcmp(token, "rad_fill") == 0) {
                    GetToken(qfalse);
                    if (strcmp(token, "voxel") == 0) {
                        localSurfaces[surfaceNum].radFillMode = RAD_FILL_VOXEL;
                    } else if (strcmp(token, "bilinear") == 0) {
                        localSurfaces[surfaceNum].radFillMode = RAD_FILL_BILINEAR;
                    }
                }
            }
        }
    }
#endif
}

void ClearCacheDirectory(void) {
    _printf("Clearing cache directory...\n");
#ifdef _WIN32
    system("powershell -NoProfile -Command \"if (Test-Path cache) { Get-ChildItem cache | Remove-Item -Force -Recurse }\"");
#else
    system("rm -rf cache/*");
#endif
}
