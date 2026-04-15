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
#define MIN_RAD_DISTANCE 8.0f

extern float rad_bounce_scale;
extern float rad_color_ratio;
extern float rad_min_energy;
extern float rad_min_dist;
extern int   rad_interval;

void LightRadiosity(int radiosityPasses);

#endif
