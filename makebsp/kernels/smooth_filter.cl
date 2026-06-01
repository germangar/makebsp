/*
================
smooth_filter.cl

2D Gaussian smoothing filter for planar lightmap surfaces.

Performs one pass of Gaussian blur using gpu_get_filtered_texel()
for cross-surface seam correctness. Run multiple times for multi-pass
smoothing by the CPU ping-pong loop — no readback between passes.

Direction smoothing uses luminance-weighted averaging: each neighbor's
direction contribution is scaled by its luminance so that dark/unlit
texels do not contaminate the dominant light direction at the boundary.
After accumulation, directions and normals are re-normalized.

Kernel args:
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
  smoothingRadii[numPlanarSurfaces]    -- per-surface radii
  upscale                              -- 1 or 2
  deluxeIn      [totalPixels*3]        -- input deluxe directions (may be NULL)
  deluxeOut     [totalPixels*3]        -- output deluxe directions
  normalIn      [totalPixels*3]        -- input surface normals (may be NULL)
  normalOut     [totalPixels*3]        -- output surface normals
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
    float upscale,
    __global const float            *deluxeIn,
    __global       float            *deluxeOut,
    __global const float            *normalIn,
    __global       float            *normalOut)
{
    int tid = get_global_id(0);

    int   atlasIdx = validList[tid];
    int   sIdx     = pixelToSurface[atlasIdx];
    if (sIdx < 0) {
        atlasOut[atlasIdx*3+0] = atlasIn[atlasIdx*3+0];
        atlasOut[atlasIdx*3+1] = atlasIn[atlasIdx*3+1];
        atlasOut[atlasIdx*3+2] = atlasIn[atlasIdx*3+2];
        if (deluxeIn && deluxeOut) {
            deluxeOut[atlasIdx*3+0] = deluxeIn[atlasIdx*3+0];
            deluxeOut[atlasIdx*3+1] = deluxeIn[atlasIdx*3+1];
            deluxeOut[atlasIdx*3+2] = deluxeIn[atlasIdx*3+2];
        }
        if (normalIn && normalOut) {
            normalOut[atlasIdx*3+0] = normalIn[atlasIdx*3+0];
            normalOut[atlasIdx*3+1] = normalIn[atlasIdx*3+1];
            normalOut[atlasIdx*3+2] = normalIn[atlasIdx*3+2];
        }
        return;
    }
    int   lx       = pixelToX[atlasIdx];
    int   ly       = pixelToY[atlasIdx];

    float localRadius = smoothingRadii[sIdx];
    if (localRadius <= 0.0f) {
        atlasOut[atlasIdx*3+0] = atlasIn[atlasIdx*3+0];
        atlasOut[atlasIdx*3+1] = atlasIn[atlasIdx*3+1];
        atlasOut[atlasIdx*3+2] = atlasIn[atlasIdx*3+2];
        if (deluxeIn && deluxeOut) {
            deluxeOut[atlasIdx*3+0] = deluxeIn[atlasIdx*3+0];
            deluxeOut[atlasIdx*3+1] = deluxeIn[atlasIdx*3+1];
            deluxeOut[atlasIdx*3+2] = deluxeIn[atlasIdx*3+2];
        }
        if (normalIn && normalOut) {
            normalOut[atlasIdx*3+0] = normalIn[atlasIdx*3+0];
            normalOut[atlasIdx*3+1] = normalIn[atlasIdx*3+1];
            normalOut[atlasIdx*3+2] = normalIn[atlasIdx*3+2];
        }
        return;
    }

    float sigma = localRadius * upscale / 3.0f;
    if (sigma < 0.5f * upscale) sigma = 0.5f * upscale;
    float tS2 = 2.0f * sigma * sigma;
    int kR = (int)ceil(localRadius * upscale);
    if (kR > 16 * (int)upscale) kR = 16 * (int)upscale;

    /* Light color accumulation */
    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f, sumW = 0.0f;

    /* Luminance-weighted direction/normal accumulation */
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f, dirW = 0.0f;
    float nrmX = 0.0f, nrmY = 0.0f, nrmZ = 0.0f;

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

                /* Luminance weight for direction: perceptual luma of this neighbor */
                float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                float dw = w * lum;

                /* Direction smoothing: need the atlas index for the neighbor */
                if ((deluxeIn && deluxeOut) || (normalIn && normalOut)) {
                    /* gpu_get_filtered_texel found a valid sample; get its atlas pixel.
                     * We approximate by reading from the same surface at (px, py) —
                     * For the common case (interior texels) we can find the neighbor directly. */
                    int nx = lx + di;
                    int ny = ly + dj;
                    /* Clamp to surface bounds */
                    GpuPlanarSurface s = surfaces[sIdx];
                    if (nx >= 0 && nx < s.width && ny >= 0 && ny < s.height) {
                        int nAtlas = (s.lmNum * LIGHTMAP_HEIGHT + s.lmOffY + ny) * LIGHTMAP_WIDTH + s.lmOffX + nx;
                        if (deluxeIn && deluxeOut) {
                            dirX += dw * deluxeIn[nAtlas*3+0];
                            dirY += dw * deluxeIn[nAtlas*3+1];
                            dirZ += dw * deluxeIn[nAtlas*3+2];
                        }
                        if (normalIn && normalOut) {
                            nrmX += dw * normalIn[nAtlas*3+0];
                            nrmY += dw * normalIn[nAtlas*3+1];
                            nrmZ += dw * normalIn[nAtlas*3+2];
                        }
                        dirW += dw;
                    }
                }
            }
        }
    }

    /* Write light color */
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

    /* Write deluxe direction (re-normalized) */
    if (deluxeIn && deluxeOut) {
        if (dirW > 0.0001f) {
            float len = sqrt(dirX*dirX + dirY*dirY + dirZ*dirZ);
            if (len > 0.001f) {
                float invLen = 1.0f / len;
                deluxeOut[atlasIdx*3+0] = dirX * invLen;
                deluxeOut[atlasIdx*3+1] = dirY * invLen;
                deluxeOut[atlasIdx*3+2] = dirZ * invLen;
            } else {
                deluxeOut[atlasIdx*3+0] = deluxeIn[atlasIdx*3+0];
                deluxeOut[atlasIdx*3+1] = deluxeIn[atlasIdx*3+1];
                deluxeOut[atlasIdx*3+2] = deluxeIn[atlasIdx*3+2];
            }
        } else {
            deluxeOut[atlasIdx*3+0] = deluxeIn[atlasIdx*3+0];
            deluxeOut[atlasIdx*3+1] = deluxeIn[atlasIdx*3+1];
            deluxeOut[atlasIdx*3+2] = deluxeIn[atlasIdx*3+2];
        }
    }

    /* Write surface normal (re-normalized) */
    if (normalIn && normalOut) {
        if (dirW > 0.0001f) {
            float len = sqrt(nrmX*nrmX + nrmY*nrmY + nrmZ*nrmZ);
            if (len > 0.001f) {
                float invLen = 1.0f / len;
                normalOut[atlasIdx*3+0] = nrmX * invLen;
                normalOut[atlasIdx*3+1] = nrmY * invLen;
                normalOut[atlasIdx*3+2] = nrmZ * invLen;
            } else {
                normalOut[atlasIdx*3+0] = normalIn[atlasIdx*3+0];
                normalOut[atlasIdx*3+1] = normalIn[atlasIdx*3+1];
                normalOut[atlasIdx*3+2] = normalIn[atlasIdx*3+2];
            }
        } else {
            normalOut[atlasIdx*3+0] = normalIn[atlasIdx*3+0];
            normalOut[atlasIdx*3+1] = normalIn[atlasIdx*3+1];
            normalOut[atlasIdx*3+2] = normalIn[atlasIdx*3+2];
        }
    }
}
