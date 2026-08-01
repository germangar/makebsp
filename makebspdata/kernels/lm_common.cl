/*
================
lm_common.cl

Shared GPU library for lightmap post-processing filters.
All filter kernels are compiled with this file prepended.

Provides:
  - GpuPlanarSurface struct (mirrors planarInfo_t)
  - gpu_sample_masked_bilinear()  -- masked bilinear fetch within one surface
  - gpu_sample_world_bilinear()   -- cross-surface seam lookup via partner list
  - gpu_get_filtered_texel()      -- full match of CPU GetFilteredTexel()

LIGHTMAP_WIDTH and LIGHTMAP_HEIGHT are injected as -D compile flags.
================
*/

#ifndef LM_COMMON_CL
#define LM_COMMON_CL

/*
================
GpuPlanarSurface

Flattened mirror of planarInfo_t. 64 bytes, all float/int members,
no implicit padding.

Partner adjacency is stored separately in CSR layout:
  surface S partners = partnerData[ partnerOffsets[S] .. partnerOffsets[S+1]-1 ]
================
*/
typedef struct {
    float originX,   originY,   originZ;   /* 12 */
    float vecs0X,    vecs0Y,    vecs0Z;    /* 12 */
    float vecs1X,    vecs1Y,    vecs1Z;    /* 12 */
    float invMagSq0, invMagSq1;            /*  8 */
    int   width,     height;               /*  8 */
    int   lmNum;                           /*  4 */
    int   lmOffX,    lmOffY;              /*  8 */
} GpuPlanarSurface;                        /* = 64 bytes */

/*
================
gpu_sample_masked_bilinear

Bilinear sample at (u,v) inside the given surface's lightmap region.
Texels with mask==0 are weighted zero.
Returns false if all four neighbours are masked.
================
*/
static bool gpu_sample_masked_bilinear(
    __global const float *atlas,
    __global const uchar *mask,
    int lmNum, int lmOffX, int lmOffY,
    int w, int h,
    float u, float v,
    float *outR, float *outG, float *outB)
{
    // Shift to node-relative coordinates (centers at 0.5, 1.5...)
    float ux = u - 0.5f;
    float vy = v - 0.5f;

    int   x0 = (int)floor(ux), y0 = (int)floor(vy);
    float fx  = ux - (float)x0, fy = vy - (float)y0;

    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0)  x0 = 0;  if (x0 >= w) x0 = w - 1;
    if (x1 < 0)  x1 = 0;  if (x1 >= w) x1 = w - 1;
    if (y0 < 0)  y0 = 0;  if (y0 >= h) y0 = h - 1;
    if (y1 < 0)  y1 = 0;  if (y1 >= h) y1 = h - 1;

    int row0 = (lmNum * LIGHTMAP_HEIGHT + lmOffY + y0) * LIGHTMAP_WIDTH + lmOffX;
    int row1 = (lmNum * LIGHTMAP_HEIGHT + lmOffY + y1) * LIGHTMAP_WIDTH + lmOffX;
    int p00  = row0 + x0, p10 = row0 + x1;
    int p01  = row1 + x0, p11 = row1 + x1;

    float w00 = (1.0f - fx) * (1.0f - fy);
    float w10 = fx           * (1.0f - fy);
    float w01 = (1.0f - fx) * fy;
    float w11 = fx           * fy;

    if (!mask[p00]) w00 = 0.0f;
    if (!mask[p10]) w10 = 0.0f;
    if (!mask[p01]) w01 = 0.0f;
    if (!mask[p11]) w11 = 0.0f;

    float sumW = w00 + w10 + w01 + w11;
    if (sumW < 0.01f) return false;

    float invW = 1.0f / sumW;
    *outR = (w00*atlas[p00*3+0] + w10*atlas[p10*3+0] + w01*atlas[p01*3+0] + w11*atlas[p11*3+0]) * invW;
    *outG = (w00*atlas[p00*3+1] + w10*atlas[p10*3+1] + w01*atlas[p01*3+1] + w11*atlas[p11*3+1]) * invW;
    *outB = (w00*atlas[p00*3+2] + w10*atlas[p10*3+2] + w01*atlas[p01*3+2] + w11*atlas[p11*3+2]) * invW;
    return true;
}

/*
================
gpu_sample_world_bilinear

Cross-surface seam lookup. Matches SampleLightmapWorldBilinear() in lm_postprocess.c.
Iterates only the shared-edge partners of surface srcIdx.
================
*/
static bool gpu_sample_world_bilinear(
    int srcIdx,
    float wx, float wy, float wz,
    __global const GpuPlanarSurface *surfaces,
    __global const int              *partnerData,
    __global const int              *partnerOffsets,
    __global const float            *atlas,
    __global const uchar            *mask,
    float *outR, float *outG, float *outB)
{
    int pStart = partnerOffsets[srcIdx];
    int pEnd   = partnerOffsets[srcIdx + 1];

    for (int pi = pStart; pi < pEnd; pi++) {
        __global const GpuPlanarSurface *p = &surfaces[partnerData[pi]];

        float dx = wx - p->originX, dy = wy - p->originY, dz = wz - p->originZ;
        float u  = (dx*p->vecs0X + dy*p->vecs0Y + dz*p->vecs0Z) * p->invMagSq0;
        float v  = (dx*p->vecs1X + dy*p->vecs1Y + dz*p->vecs1Z) * p->invMagSq1;

        if (u < -0.51f || u > (float)p->width  - 0.49f ||
            v < -0.51f || v > (float)p->height - 0.49f) continue;

        if (gpu_sample_masked_bilinear(atlas, mask, p->lmNum, p->lmOffX, p->lmOffY,
                                        p->width, p->height, u, v,
                                        outR, outG, outB))
            return true;
    }
    return false;
}

/*
================
gpu_get_filtered_texel

Full port of GetFilteredTexel() from lm_postprocess.c.
Samples at (px, py) in local surface coords of surface sIdx:
  1. Fast path  — strictly inside local bounds → local bilinear
  2. Slow path  — out of bounds → world-space seam lookup via partners
  3. Fallback   — no neighbour found → strictly fail (matches CPU)
================
*/
static bool gpu_get_filtered_texel(
    int sIdx, float px, float py,
    __global const GpuPlanarSurface *surfaces,
    __global const int              *partnerData,
    __global const int              *partnerOffsets,
    __global const float            *atlas,
    __global const uchar            *mask,
    float *outR, float *outG, float *outB)
{
    __global const GpuPlanarSurface *s = &surfaces[sIdx];

    int  x0    = (int)floor(px), y0 = (int)floor(py);
    float fx   = px - (float)x0, fy = py - (float)y0;
    int  maxX  = (fx > 0.001f) ? x0 + 1 : x0;
    int  maxY  = (fy > 0.001f) ? y0 + 1 : y0;

    /* --- Fast path: strictly inside --- */
    if (x0 >= 0 && maxX < s->width && y0 >= 0 && maxY < s->height) {
        if (gpu_sample_masked_bilinear(atlas, mask, s->lmNum, s->lmOffX, s->lmOffY,
                                        s->width, s->height, px, py,
                                        outR, outG, outB))
            return true;
    }

    /* --- Slow path: world-space seam lookup --- */
    float wx = s->originX + px*s->vecs0X + py*s->vecs1X;
    float wy = s->originY + px*s->vecs0Y + py*s->vecs1Y;
    float wz = s->originZ + px*s->vecs0Z + py*s->vecs1Z;

    if (gpu_sample_world_bilinear(sIdx, wx, wy, wz,
                                   surfaces, partnerData, partnerOffsets,
                                   atlas, mask, outR, outG, outB))
        return true;

    return false;
}

#endif /* LM_COMMON_CL */
