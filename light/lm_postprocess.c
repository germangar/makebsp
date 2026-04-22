#include "light.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
===============================================================================

LIGHTMAP POST-PROCESSING (SMOOTHING)

This module implements a multi-threaded, alpha-aware Gaussian blur.
It uses Normalized Convolution (premultiplied-alpha) to ensure that
lighting doesn't bleed into unmapped areas and unmapped areas don't
darken the valid lighting.

===============================================================================
*/

int lightmapAA = 0;
float lightmapSmoothRadius = 0.0f;
int lightmapSmoothPasses = 0;

#define MAX_KERNEL_RADIUS 16

// 8-point Rotated Grid (tilted ~26.6 degrees)
static const float ssPattern8[][2] = {
  { 0.000f,  0.000f},   // center
  {-0.354f, -0.854f},
  { 0.354f, -0.354f},
  { 0.854f,  0.146f},
  { 0.354f,  0.646f},
  {-0.146f,  0.354f},
  {-0.646f, -0.146f},
  {-0.854f,  0.354f},
};
#define SS_PATTERN8_COUNT 8

void AntiAliasLightmaps(void) {
	int x, y, p, s, k;
	int numPixels;
	float *tempFloats;
	dsurface_t *ds;

	if (!lightFloats || !lightAlphaMask) {
		return;
	}

	numPixels = numLightBytes / 3;

	tempFloats = malloc(numPixels * sizeof(float) * 3);
	if (!tempFloats) {
		_printf("WARNING: AntiAliasLightmaps failed to allocate memory\n");
		return;
	}
	// Copy original
	memcpy(tempFloats, lightFloats, numPixels * sizeof(float) * 3);

	float radius = lightmapSmoothRadius > 0.0f ? lightmapSmoothRadius : 1.0f; // Ensure a valid radius

	if (lightmapAA == 1) {
		#pragma omp parallel for private(s, ds, x, y, p, k)
		for (s = 0; s < numDrawSurfaces; s++) {
			ds = &drawSurfaces[s];
			
			if (ds->lightmapNum[0] < 0 || ds->surfaceType == MST_TRIANGLE_SOUP) {
				continue;
			}

			for (y = 0; y < ds->lightmapHeight; y++) {
				for (x = 0; x < ds->lightmapWidth; x++) {
					int globalX = ds->lightmapOffset[0][0] + x;
					int globalY = ds->lightmapOffset[0][1] + y;
					p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + globalY) * LIGHTMAP_WIDTH + globalX;
					
					if (lightAlphaMask[p] != ALPHA_SMOOTH) {
						continue;
					}

					float sumColor[3] = {0, 0, 0};
					float sumWeight = 0.0f;

					for (k = 0; k < SS_PATTERN8_COUNT; k++) {
						float px = (float)x + ssPattern8[k][0] * radius;
						float py = (float)y + ssPattern8[k][1] * radius;

						// Clamp to edges for safe sampling
						if (px < 0.0f) px = 0.0f;
						if (px > (float)(ds->lightmapWidth - 1)) px = (float)(ds->lightmapWidth - 1);
						if (py < 0.0f) py = 0.0f;
						if (py > (float)(ds->lightmapHeight - 1)) py = (float)(ds->lightmapHeight - 1);

						int ix = (int)floorf(px);
						int iy = (int)floorf(py);
						float fx = px - ix;
						float fy = py - iy;

						int nx0 = ix;
						int nx1 = ix + 1;
						int ny0 = iy;
						int ny1 = iy + 1;

						if (nx1 >= ds->lightmapWidth) nx1 = ds->lightmapWidth - 1;
						if (ny1 >= ds->lightmapHeight) ny1 = ds->lightmapHeight - 1;

						int p00 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ny0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + nx0;
						int p10 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ny0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + nx1;
						int p01 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ny1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + nx0;
						int p11 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ny1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + nx1;

						float w00 = (1.0f - fx) * (1.0f - fy);
						float w10 = fx * (1.0f - fy);
						float w01 = (1.0f - fx) * fy;
						float w11 = fx * fy;

						// Zero weight if out of bounds or masked
						if (lightAlphaMask[p00] == 0) w00 = 0.0f;
						if (lightAlphaMask[p10] == 0) w10 = 0.0f;
						if (lightAlphaMask[p01] == 0) w01 = 0.0f;
						if (lightAlphaMask[p11] == 0) w11 = 0.0f;

						float sampleWeight = w00 + w10 + w01 + w11;

						if (sampleWeight > 0.0001f) {
							float r = w00 * tempFloats[p00*3+0] + w10 * tempFloats[p10*3+0] + w01 * tempFloats[p01*3+0] + w11 * tempFloats[p11*3+0];
							float g = w00 * tempFloats[p00*3+1] + w10 * tempFloats[p10*3+1] + w01 * tempFloats[p01*3+1] + w11 * tempFloats[p11*3+1];
							float b = w00 * tempFloats[p00*3+2] + w10 * tempFloats[p10*3+2] + w01 * tempFloats[p01*3+2] + w11 * tempFloats[p11*3+2];
							
							sumColor[0] += r / sampleWeight;
							sumColor[1] += g / sampleWeight;
							sumColor[2] += b / sampleWeight;
							sumWeight += 1.0f; // Each pattern sample gets equal weight if it touched valid pixels
						}
					}

					if (sumWeight > 0.0001f) {
						lightFloats[p * 3 + 0] = sumColor[0] / sumWeight;
						lightFloats[p * 3 + 1] = sumColor[1] / sumWeight;
						lightFloats[p * 3 + 2] = sumColor[2] / sumWeight;
					}
				}
			}
		}
	} else if (lightmapAA == 2) {
		#pragma omp parallel for private(s, ds, x, y, p, k)
		for (s = 0; s < numDrawSurfaces; s++) {
			ds = &drawSurfaces[s];
			
			if (ds->lightmapNum[0] < 0 || ds->surfaceType == MST_TRIANGLE_SOUP) {
				continue;
			}

			int W = ds->lightmapWidth;
			int H = ds->lightmapHeight;
			if (W <= 0 || H <= 0) continue;

			int W2 = W * 2;
			int H2 = H * 2;

			float *temp2x = malloc(W2 * H2 * 3 * sizeof(float));
			byte *mask2x = malloc(W2 * H2 * sizeof(byte));
			float *blur2x = malloc(W2 * H2 * 3 * sizeof(float));
			byte *blurMask2x = malloc(W2 * H2 * sizeof(byte));

			// 1. Upscale
			for (int Y = 0; Y < H2; Y++) {
				for (int X = 0; X < W2; X++) {
					float px = (float)X * 0.5f - 0.25f;
					float py = (float)Y * 0.5f - 0.25f;

					if (px < 0.0f) px = 0.0f;
					if (px > (float)(W - 1) - 0.001f) px = (float)(W - 1) - 0.001f;
					if (py < 0.0f) py = 0.0f;
					if (py > (float)(H - 1) - 0.001f) py = (float)(H - 1) - 0.001f;

					int ix = (int)floorf(px);
					int iy = (int)floorf(py);
					float fx = px - (float)ix;
					float fy = py - (float)iy;

					int nx0 = ix, nx1 = ix + 1;
					int ny0 = iy, ny1 = iy + 1;
					if (nx1 >= W) nx1 = W - 1;
					if (ny1 >= H) ny1 = H - 1;

					int p00 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ny0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + nx0;
					int p10 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ny0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + nx1;
					int p01 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ny1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + nx0;
					int p11 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + ny1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + nx1;

					float w00 = (1.0f - fx) * (1.0f - fy);
					float w10 = fx * (1.0f - fy);
					float w01 = (1.0f - fx) * fy;
					float w11 = fx * fy;

					if (lightAlphaMask[p00] == 0) w00 = 0.0f;
					if (lightAlphaMask[p10] == 0) w10 = 0.0f;
					if (lightAlphaMask[p01] == 0) w01 = 0.0f;
					if (lightAlphaMask[p11] == 0) w11 = 0.0f;

					float sumW = w00 + w10 + w01 + w11;
					if (sumW > 0.0001f) {
						mask2x[Y * W2 + X] = ALPHA_SMOOTH;
						for (int c = 0; c < 3; c++) {
							temp2x[(Y * W2 + X) * 3 + c] = (w00 * tempFloats[p00*3+c] + w10 * tempFloats[p10*3+c] + w01 * tempFloats[p01*3+c] + w11 * tempFloats[p11*3+c]) / sumW;
						}
					} else {
						mask2x[Y * W2 + X] = 0;
						for (int c = 0; c < 3; c++) temp2x[(Y * W2 + X) * 3 + c] = 0.0f;
					}
				}
			}

			// 2. Apply pattern
			for (int Y = 0; Y < H2; Y++) {
				for (int X = 0; X < W2; X++) {
					if (mask2x[Y * W2 + X] == 0) {
						blurMask2x[Y * W2 + X] = 0;
						continue;
					}

					float sumColor[3] = {0, 0, 0};
					float sumWeight = 0.0f;

					for (k = 0; k < SS_PATTERN8_COUNT; k++) {
						float px = (float)X + ssPattern8[k][0] * radius * 2.0f;
						float py = (float)Y + ssPattern8[k][1] * radius * 2.0f;

						int ix = (int)roundf(px);
						int iy = (int)roundf(py);

						if (ix < 0) ix = 0;
						if (ix >= W2) ix = W2 - 1;
						if (iy < 0) iy = 0;
						if (iy >= H2) iy = H2 - 1;

						if (mask2x[iy * W2 + ix] != 0) {
							sumColor[0] += temp2x[(iy * W2 + ix) * 3 + 0];
							sumColor[1] += temp2x[(iy * W2 + ix) * 3 + 1];
							sumColor[2] += temp2x[(iy * W2 + ix) * 3 + 2];
							sumWeight += 1.0f;
						}
					}

					if (sumWeight > 0.0001f) {
						blurMask2x[Y * W2 + X] = ALPHA_SMOOTH;
						blur2x[(Y * W2 + X) * 3 + 0] = sumColor[0] / sumWeight;
						blur2x[(Y * W2 + X) * 3 + 1] = sumColor[1] / sumWeight;
						blur2x[(Y * W2 + X) * 3 + 2] = sumColor[2] / sumWeight;
					} else {
						blurMask2x[Y * W2 + X] = 0;
					}
				}
			}

			// 3. Reduce back
			for (y = 0; y < H; y++) {
				for (x = 0; x < W; x++) {
					p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
					if (lightAlphaMask[p] != ALPHA_SMOOTH) continue;

					float sumColor[3] = {0, 0, 0};
					float sumWeight = 0.0f;

					for (int dy = 0; dy < 2; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							int X = x * 2 + dx;
							int Y = y * 2 + dy;
							if (blurMask2x[Y * W2 + X] != 0) {
								sumColor[0] += blur2x[(Y * W2 + X) * 3 + 0];
								sumColor[1] += blur2x[(Y * W2 + X) * 3 + 1];
								sumColor[2] += blur2x[(Y * W2 + X) * 3 + 2];
								sumWeight += 1.0f;
							}
						}
					}

					if (sumWeight > 0.0001f) {
						lightFloats[p * 3 + 0] = sumColor[0] / sumWeight;
						lightFloats[p * 3 + 1] = sumColor[1] / sumWeight;
						lightFloats[p * 3 + 2] = sumColor[2] / sumWeight;
					}
				}
			}

			free(temp2x);
			free(mask2x);
			free(blur2x);
			free(blurMask2x);
		}
	}

	free(tempFloats);
}

void SmoothLightmaps(float radius) {
	int i, x, y, p, s;
	int numPixels;
	float *tempFloats;
	float kernel[MAX_KERNEL_RADIUS * 2 + 1];
	int kernelRadius;
	float sigma;
	dsurface_t *ds;

	if (radius <= 0.0f) {
		return;
	}

	if (!lightFloats || !lightAlphaMask) {
		return;
	}

	numPixels = numLightBytes / 3;

	// 1. Prepare Gaussian Kernel
	// We treat the radius as 3*sigma
	sigma = radius / 3.0f;
	if (sigma < 0.5f) sigma = 0.5f;

	kernelRadius = (int)ceil(radius);
	if (kernelRadius > MAX_KERNEL_RADIUS) kernelRadius = MAX_KERNEL_RADIUS;

	float kernelSum = 0.0f;
	for (i = -kernelRadius; i <= kernelRadius; i++) {
		kernel[i + kernelRadius] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
		kernelSum += kernel[i + kernelRadius];
	}
	// Normalize kernel
	for (i = 0; i <= kernelRadius * 2; i++) {
		kernel[i] /= kernelSum;
	}

	// 2. Allocate temporary buffer for the two-pass blur
	tempFloats = malloc(numPixels * sizeof(float) * 3);
	if (!tempFloats) {
		_printf("WARNING: SmoothLightmaps failed to allocate memory\n");
		return;
	}
	// Default to original data for untagged areas
	memcpy(tempFloats, lightFloats, numPixels * sizeof(float) * 3);

	// --- Pass 1: Horizontal Blur ---
	#pragma omp parallel for private(s, ds, y, x, i, p)
	for (s = 0; s < numDrawSurfaces; s++) {
		ds = &drawSurfaces[s];
        
		if (ds->lightmapNum[0] < 0 || ds->surfaceType == MST_TRIANGLE_SOUP) {
			continue;
		}

		for (y = 0; y < ds->lightmapHeight; y++) {
			for (x = 0; x < ds->lightmapWidth; x++) {
				int globalY = ds->lightmapOffset[0][1] + y;
				int globalX = ds->lightmapOffset[0][0] + x;
				p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + globalY) * LIGHTMAP_WIDTH + globalX;
				
				float sumColor[3] = {0, 0, 0};
				float sumWeight = 0.0f;

				for (i = -kernelRadius; i <= kernelRadius; i++) {
					int tx = x + i;
					// CLAMP to surface bounds
					if (tx < 0) tx = 0;
					if (tx >= ds->lightmapWidth) tx = ds->lightmapWidth - 1;

					int neighborGlobalX = ds->lightmapOffset[0][0] + tx;
					int neighborIdx = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + globalY) * LIGHTMAP_WIDTH + neighborGlobalX;
					
					float weight = kernel[i + kernelRadius];
					float *pix = &lightFloats[neighborIdx * 3];
					sumColor[0] += weight * pix[0];
					sumColor[1] += weight * pix[1];
					sumColor[2] += weight * pix[2];
					sumWeight += weight;
				}

				float *out = &tempFloats[p * 3];
				if (sumWeight > 0.0001f) {
					out[0] = sumColor[0] / sumWeight;
					out[1] = sumColor[1] / sumWeight;
					out[2] = sumColor[2] / sumWeight;
				}
			}
		}
	}

	// --- Pass 2: Vertical Blur ---
	#pragma omp parallel for private(s, ds, x, y, i, p)
	for (s = 0; s < numDrawSurfaces; s++) {
		ds = &drawSurfaces[s];
        
		if (ds->lightmapNum[0] < 0 || ds->surfaceType == MST_TRIANGLE_SOUP) {
			continue;
		}

		for (x = 0; x < ds->lightmapWidth; x++) {
			for (y = 0; y < ds->lightmapHeight; y++) {
				int globalX = ds->lightmapOffset[0][0] + x;
				int globalY = ds->lightmapOffset[0][1] + y;
				p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + globalY) * LIGHTMAP_WIDTH + globalX;

				float sumColor[3] = {0, 0, 0};
				float sumWeight = 0.0f;

				for (i = -kernelRadius; i <= kernelRadius; i++) {
					int ty = y + i;
					// CLAMP to surface bounds
					if (ty < 0) ty = 0;
					if (ty >= ds->lightmapHeight) ty = ds->lightmapHeight - 1;

					int neighborGlobalY = ds->lightmapOffset[0][1] + ty;
					int neighborIdx = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + neighborGlobalY) * LIGHTMAP_WIDTH + globalX;

					float weight = kernel[i + kernelRadius];
					float *pix = &tempFloats[neighborIdx * 3];
					sumColor[0] += weight * pix[0];
					sumColor[1] += weight * pix[1];
					sumColor[2] += weight * pix[2];
					sumWeight += weight;
				}

				float *finalOut = &lightFloats[p * 3];
				if (sumWeight > 0.0001f) {
					finalOut[0] = sumColor[0] / sumWeight;
					finalOut[1] = sumColor[1] / sumWeight;
					finalOut[2] = sumColor[2] / sumWeight;
				}
			}
		}
	}

	free(tempFloats);
}

/*
================
PostProcessLightmaps

HUB function for all lightmap post-processing steps.
Called at the end of the direct lighting phase.
================
*/
void PostProcessLightmaps(void) {
	_printf("--- Post Processing ---\n");
	
	// 1. Scan for peak intensity (for normalization/HDR scaling)
	ScanLightmapIntensity();

	// 2. Post-process Anti-Aliasing (RGSS Pattern)
	if (lightmapAA) {
		_printf("Applying Anti-Aliasing pass...\n");
		AntiAliasLightmaps();
	}

	// 3. Multitransfert / Gaussian Smoothing
	if (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) {
		_printf("Smoothing (%d passes, radius %.2f): ", lightmapSmoothPasses, lightmapSmoothRadius);
		for (int pnum = 1; pnum <= lightmapSmoothPasses; pnum++) {
			_printf("%d...", pnum);
			SmoothLightmaps(lightmapSmoothRadius);
		}
		_printf(" Done\n");
	}
}
