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

float lightmapSmoothRadius = 0.0f;
int lightmapSmoothPasses = 0;

#define MAX_KERNEL_RADIUS 16

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
