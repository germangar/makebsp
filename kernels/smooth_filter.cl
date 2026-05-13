/*
================
smooth_filter.cl

2D Gaussian smoothing filter for planar lightmap surfaces.

Performs one pass of Gaussian blur using gpu_get_filtered_texel()
for cross-surface seam correctness. Run multiple times for multi-pass
smoothing by the CPU ping-pong loop — no readback between passes.

Kernel args (same metadata signature as aa_filter for reuse):
  atlasIn       [totalPixels*3]        -- input RGB (ping buffer)
  atlasOut      [totalPixels*3]        -- output RGB (pong buffer)
  mask          [totalPixels]          -- alpha mask
  surfaces      [numPlanarSurfaces]    -- GpuPlanarSurface metadata
  partnerData   [totalPartnerLinks]    -- CSR partner indices
  partnerOffsets[numPlanarSurfaces+1]  -- CSR offsets
  validList     [numValid]             -- atlas indices of valid planar texels
  pixelToSurface[totalPixels]          -- atlas index -> planarSurfaces index
  pixelToX      [totalPixels]          -- atlas index -> local surface x
  pixelToY      [totalPixels]          -- atlas index -> local surface y
  gaussWeights  [(2*kernelRadius+1)^2] -- pre-computed Gaussian weights (row-major)
  kernelRadius
================
*/

/* lm_common.cl is prepended by BuildOpenCLProgramWithCommon() */

__kernel void smooth_filter(
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
    __global const float            *smoothingRadii,
    float upscale)
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

    float localRadius = smoothingRadii[sIdx];
    if (localRadius <= 0.0f) {
        atlasOut[atlasIdx*3+0] = atlasIn[atlasIdx*3+0];
        atlasOut[atlasIdx*3+1] = atlasIn[atlasIdx*3+1];
        atlasOut[atlasIdx*3+2] = atlasIn[atlasIdx*3+2];
        return;
    }

    float sigma = localRadius * upscale / 3.0f;
    if (sigma < 0.5f * upscale) sigma = 0.5f * upscale;
    float tS2 = 2.0f * sigma * sigma;
    int kR = (int)ceil(localRadius * upscale);
    if (kR > 16 * (int)upscale) kR = 16 * (int)upscale;

    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f, sumW = 0.0f;

    for (int dj = -kR; dj <= kR; dj++) {
        for (int di = -kR; di <= kR; di++) {
            float px = (float)(lx + di) + 0.5f;
            float py = (float)(ly + dj) + 0.5f;

            float r, g, b;
            if (gpu_get_filtered_texel(sIdx, px, py,
                                        surfaces, partnerData, partnerOffsets,
                                        atlasIn, mask,
                                        &r, &g, &b)) {
                float w = exp(-(float)(di*di + dj*dj) / tS2);
                sumR += w * r;
                sumG += w * g;
                sumB += w * b;
                sumW += w;
            }
        }
    }

    if (sumW > 0.0001f) {
        float inv = 1.0f / sumW;
        atlasOut[atlasIdx*3+0] = sumR * inv;
        atlasOut[atlasIdx*3+1] = sumG * inv;
        atlasOut[atlasIdx*3+2] = sumB * inv;
    } else {
        atlasOut[atlasIdx*3+0] = atlasIn[atlasIdx*3+0];
        atlasOut[atlasIdx*3+1] = atlasIn[atlasIdx*3+1];
        atlasOut[atlasIdx*3+2] = atlasIn[atlasIdx*3+2];
    }
}
