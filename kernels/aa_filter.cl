/*
================
aa_filter.cl

Image-space Anti-Aliasing filter kernel.

Previously relied on a CPU-built Sample Resolution Table (SRT).
Now uses gpu_get_filtered_texel() from lm_common.cl directly, which
enables in-GPU ping-pong across passes and eliminates intermediate
CPU-GPU transfers.

Kernel args:
  atlasIn       [totalPixels*3]        -- input RGB (ping buffer)
  atlasOut      [totalPixels*3]        -- output RGB (pong buffer)
  mask          [totalPixels]          -- alpha mask (0 = invalid texel)
  surfaces      [numPlanarSurfaces]    -- GpuPlanarSurface metadata
  partnerData   [totalPartnerLinks]    -- CSR partner indices
  partnerOffsets[numPlanarSurfaces+1]  -- CSR offsets
  validList     [numValid]             -- atlas indices of valid planar texels
  pixelToSurface[totalPixels]          -- atlas index -> planarSurfaces index (-1 if N/A)
  pixelToX      [totalPixels]          -- atlas index -> local surface x
  pixelToY      [totalPixels]          -- atlas index -> local surface y
  pattern       [numSamples*2]         -- jitter offsets (x,y per sample)
  numSamples
  radius
================
*/

/* lm_common.cl is prepended by BuildOpenCLProgramWithCommon() */

__kernel void aa_filter(
    __global const float            *atlasIn,
    __global       float            *atlasOut,
    __global const uchar            *mask,
    __global const GpuPlanarSurface *surfaces,
    __global const int              *partnerData,
    __global const int              *partnerOffsets,
    __global const int              *validList,
    __global const int              *pixelToSurface,
    __global const int              *pixelToX,
    __global const int              *pixelToY,
    __global const float            *pattern,
    int   numSamples,
    float radius)
{
    int tid = get_global_id(0);

    int   atlasIdx = validList[tid];
    int   sIdx     = pixelToSurface[atlasIdx];
    if (sIdx < 0) {
        atlasOut[atlasIdx*3+0] = atlasIn[atlasIdx*3+0];
        atlasOut[atlasIdx*3+1] = atlasIn[atlasIdx*3+1];
        atlasOut[atlasIdx*3+2] = atlasIn[atlasIdx*3+2];
        return;
    }
    int   lx       = pixelToX[atlasIdx];
    int   ly       = pixelToY[atlasIdx];

    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
    int   cnt  = 0;

    for (int k = 0; k < numSamples; k++) {
        float px = (float)lx + pattern[k*2+0] * radius;
        float py = (float)ly + pattern[k*2+1] * radius;

        float r, g, b;
        if (gpu_get_filtered_texel(sIdx, px, py,
                                    surfaces, partnerData, partnerOffsets,
                                    atlasIn, mask,
                                    &r, &g, &b)) {
            sumR += r; sumG += g; sumB += b;
            cnt++;
        }
    }

    if (cnt > 0) {
        float inv = 1.0f / (float)cnt;
        atlasOut[atlasIdx*3+0] = sumR * inv;
        atlasOut[atlasIdx*3+1] = sumG * inv;
        atlasOut[atlasIdx*3+2] = sumB * inv;
    } else {
        /* preserve original if no valid samples found */
        atlasOut[atlasIdx*3+0] = atlasIn[atlasIdx*3+0];
        atlasOut[atlasIdx*3+1] = atlasIn[atlasIdx*3+1];
        atlasOut[atlasIdx*3+2] = atlasIn[atlasIdx*3+2];
    }
}
