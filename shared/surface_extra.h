#ifndef SURFACE_EXTRA_H
#define SURFACE_EXTRA_H

#include "../common/qtypes.h"

typedef enum {
    RAD_FILL_DEFAULT = 0,
    RAD_FILL_VOXEL,
    RAD_FILL_BILINEAR
} radFillMode_t;

void WriteSurfaceExtraFile(const char *path);
void LoadSurfaceExtraFile(const char *path);
void ClearCacheDirectory(void);

#endif
