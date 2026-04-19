#ifndef __RADIOSITY_H__
#define __RADIOSITY_H__

/*
===========================================================================
Radiosity / Global Illumination
===========================================================================

Three-phase baked GI system, Embree-only:
  Phase 1 — Emit       : lit luxels → emitter_t array
  Phase 2 — Integrate  : analytic area form-factor → radiosityFloats
  Phase 3 — Merge      : radiosityFloats → lightFloats

Call LightRadiosity() AFTER LightMain() and BEFORE SmoothLightmaps().
*/

// Tuning parameters (managed via main.c CLI)
#define RAD_DEPTH_MIN_DEFAULT 8.0f
#define RAD_DEPTH_MAX_DEFAULT 24.0f
#define RAD_DEPTH_INTENSITY_DEFAULT 0.5f

extern float rad_bounce_scale;
extern float rad_color_ratio;
extern float rad_min_energy;
extern float rad_depth_min;
extern float rad_depth_max;
extern float rad_depth_intensity;
extern int   rad_interval;
extern float rad_voxel_size;
extern float rad_angle_match; // Angle in degrees

void LightRadiosity(int radiosityPasses);

#endif
