#ifndef SURFACE_EXTRA_H
#define SURFACE_EXTRA_H

#include "../common/qtypes.h"

typedef enum {
    RAD_FILL_DEFAULT = 0,
    RAD_FILL_VOXEL,
    RAD_FILL_BILINEAR
} radFillMode_t;

typedef struct {
    radFillMode_t radFillMode;
} surfaceExtra_t;

void SetSurfaceExtraRadFillMode(int surfaceNum, radFillMode_t mode);
radFillMode_t GetSurfaceExtraRadFillMode(int surfaceNum);

void WriteSurfaceExtraFile(const char *path);
void LoadSurfaceExtraFile(const char *path);
void ClearCacheDirectory(void);

#endif
