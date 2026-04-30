#include "lightdata.h"
#include "../common/bspfile.h"
#include "../common/cmdlib.h"
#include "../shared/globals.h"
#include "../shared/mesh.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

drawVert32_t *internalDrawVerts = NULL;
float *lightFloats = NULL;
float *radiosityFloats = NULL;
float *accumRadiosityFloats = NULL;
float *irradianceVecFloats = NULL;  // 9 floats per pixel (3 irradiance vec3s, one per RGB channel)
byte *lightAlphaMask = NULL;
bspGridPoint32_t *gridData32 = NULL;

float maxLightIntensity = 0.0f;
tonemap_t tonemapMode = TONEMAP_LINEAR;

#define HOTSPOT_TAME_FACTOR 1.05f


/*
===============================================================================
DOWNSCALE LIGHTMAP IMAGES (Experimental).
Generate the whole lightmaps at a higher resolution and scale them
back down only for writting. It currently has a lot of edge bleeding. 
Don't use.
===============================================================================
*/

static void DilateLightmapAtlas(int width, int passes) {
	int lm, x, y, i, j, p;
	int numLMs = (numLightBytes / 3) / (width * width);
	float *temp = malloc(numLightBytes * sizeof(float));
	byte *tempMask = malloc(numLMs * width * width);

	_printf("Dilating lightmaps (%d passes)...\n", passes);

	for (p = 0; p < passes; p++) {
		memcpy(temp, lightFloats, numLightBytes * sizeof(float));
		memcpy(tempMask, lightAlphaMask, numLMs * width * width);

		for (lm = 0; lm < numLMs; lm++) {
			for (y = 0; y < width; y++) {
				for (x = 0; x < width; x++) {
					int idx = (lm * width * width) + y * width + x;
					if (tempMask[idx]) continue;

					float sum[3] = {0,0,0};
					float weight = 0;
					for (j = -1; j <= 1; j++) {
						for (i = -1; i <= 1; i++) {
							if (i == 0 && j == 0) continue;
							int nx = x + i;
							int ny = y + j;
							if (nx >= 0 && nx < width && ny >= 0 && ny < width) {
								int nidx = (lm * width * width) + ny * width + nx;
								if (tempMask[nidx]) {
									VectorAdd(sum, &temp[nidx * 3], sum);
									weight += 1.0f;
								}
							}
						}
					}
					if (weight > 0) {
						VectorScale(sum, 1.0f / weight, &lightFloats[idx * 3]);
						lightAlphaMask[idx] = 1; // Mark as "bled"
					}
				}
			}
		}
	}
	free(temp);
	free(tempMask);
}

static void DownscaleSurfaceLightmap(dsurface_t *ds, int ratio, float *oldFloats, byte *oldMask, int oldW, float *newFloats, byte *newMask, int newW) {
	int x, y, dx, dy;
	int sLM = ds->lightmapNum[0];
	int sOldX = ds->lightmapOffset[0][0];
	int sOldY = ds->lightmapOffset[0][1];
	int sOldW = ds->lightmapWidth;
	int sOldH = ds->lightmapHeight;

	int sNewX = sOldX / ratio;
	int sNewY = sOldY / ratio;
	int sNewW = sOldW / ratio;
	int sNewH = sOldH / ratio;

	if (sNewW <= 0) sNewW = 1;
	if (sNewH <= 0) sNewH = 1;

	// This follows the logic in lm_postprocess.c:1700-1719
	for (y = 0; y < sNewH; y++) {
		for (x = 0; x < sNewW; x++) {
			int nX = sNewX + x;
			int nY = sNewY + y;
			int newP = (sLM * newW * newW) + nY * newW + nX;

			float sumColor[3] = {0,0,0}, sumWeight = 0.0f;
			for (dy = 0; dy < ratio; dy++) {
				for (dx = 0; dx < ratio; dx++) {
					int X = nX * ratio + dx;
					int Y = nY * ratio + dy;
					if (X >= oldW || Y >= oldW) continue;

					int oldP = (sLM * oldW * oldW) + Y * oldW + X;
					if (oldMask[oldP] != 0) {
						VectorAdd(sumColor, &oldFloats[oldP * 3], sumColor);
						sumWeight += 1.0f;
					}
				}
			}

			if (sumWeight > 0.01f) {
				VectorScale(sumColor, 1.0f / sumWeight, &newFloats[newP * 3]);
				newMask[newP] = 1;
			}
		}
	}

	// Update metadata
	for (int j = 0; j < 4; j++) {
		ds->lightmapOffset[j][0] = sNewX;
		ds->lightmapOffset[j][1] = sNewY;
	}
	ds->lightmapWidth = sNewW;
	ds->lightmapHeight = sNewH;
}

void DownscaleLightmaps(int oldW, int newW) {
	int i, j;
	int ratio = oldW / newW;
	int numLMs = (numLightBytes / 3) / (oldW * oldW);
	int newTotalPixels = numLMs * newW * newW;

	_printf("--- DownscaleLightmaps (PER-SURFACE) (%dx%d -> %dx%d) ---\n", oldW, oldW, newW, newW);

	float *newFloats = calloc(newTotalPixels * 3, sizeof(float));
	byte *newMask = calloc(newTotalPixels, sizeof(byte));

	for (i = 0; i < numDrawSurfaces; i++) {
		if (drawSurfaces[i].lightmapNum[0] >= 0) {
			DownscaleSurfaceLightmap(&drawSurfaces[i], ratio, lightFloats, lightAlphaMask, oldW, newFloats, newMask, newW);
		}
	}

	// Update vertex UVs
	for (i = 0; i < numDrawVerts; i++) {
		drawVert_t *dv = &drawVerts[i];
		for (j = 0; j < 4; j++) {
			dv->lightmap[j][0] = ((dv->lightmap[j][0] * (float)oldW - 0.5f) / (float)ratio + 0.5f) / (float)newW;
			dv->lightmap[j][1] = ((dv->lightmap[j][1] * (float)oldW - 0.5f) / (float)ratio + 0.5f) / (float)newW;
		}
	}

	free(lightFloats);
	free(lightAlphaMask);
	lightFloats = newFloats;
	lightAlphaMask = newMask;

	// Update global state
	g_game->lightmapSize = newW;
	numLightBytes = newTotalPixels * 3;

	// 4. Bleed the final low-resolution atlas to prevent bilinear bleeding in the engine
	// Running 2 passes in the new resolution is equivalent to 2*ratio pixels in the old one.
	DilateLightmapAtlas(newW, 2);

	if (num_entities > 0) {
		SetKeyValue(&entities[0], "__lightmapsize", va("%d", newW));
	}
}


/*
===============================================================================
COLOR CONVERSION HELPERS
===============================================================================
*/

void InternalColorToBytes(const float *color, byte *colorBytes, qboolean sRGB) {
	float max;
	vec3_t sample;
	int i;

	VectorCopy(color, sample);

	if (sRGB) {
		for (i = 0; i < 3; i++) {
			float l = sample[i] / 255.0f;
			if (l <= 0.0031308f)
				l *= 12.92f;
			else
				l = 1.055f * (float)pow(l, 1.0f / 2.4f) - 0.055f;
			sample[i] = l * 255.0f;
		}
	}

	// clamp with color normalization
	max = sample[0];
	if (sample[1] > max) {
		max = sample[1];
	}
	if (sample[2] > max) {
		max = sample[2];
	}
	if (max > 255) {
		VectorScale(sample, 255.0f / max, sample);
	}

	for (i = 0; i < 3; i++) {
		int c = (int)floor(sample[i] + 0.5f);
		if (c < 0) {
			c = 0;
		} else if (c > 255) {
			c = 255;
		}
		colorBytes[i] = (byte)c;
	}
}

void InternalColorToBytesScaled(const float *color, byte *colorBytes, float scale, qboolean sRGB) {
	vec3_t sample;
	int i;
	VectorCopy(color, sample);

	if (tonemapMode != TONEMAP_LINEAR) {
		float maxC = sample[0];
		if (sample[1] > maxC) maxC = sample[1];
		if (sample[2] > maxC) maxC = sample[2];

		if (maxC > 0.001f) {
			float limit = (g_game->hdr == HDR_8BIT) ? maxLightIntensity : 255.0f;
			float threshold = limit * 0.75f;

			if (tonemapMode == TONEMAP_SOFTKNEE) {
				if (maxC > threshold) {
					// Math: y = threshold + (x - threshold) / (1 + ((x - threshold) / (limit - threshold)))
					float softMax = threshold + (maxC - threshold) / (1.0f + ((maxC - threshold) / (limit - threshold)));
					VectorScale(sample, softMax / maxC, sample);
				}
			} else if (tonemapMode == TONEMAP_REINHARD) {
				// Reinhard relative to 'limit'
				float normalized = maxC / limit;
				float reinhard = normalized / (1.0f + normalized);
				VectorScale(sample, (reinhard * limit) / maxC, sample);
			} else if (tonemapMode == TONEMAP_FILMIC) {
				// Filmic exponential relative to 'limit'
				float normalized = maxC / limit;
				float filmic = 1.0f - (float)exp(-normalized);
				VectorScale(sample, (filmic * limit) / maxC, sample);
			}
		}
	}

	VectorScale(sample, scale, sample);

	if (sRGB) {
		for (i = 0; i < 3; i++) {
			float l = sample[i] / 255.0f;
			if (l <= 0.0031308f)
				l *= 12.92f;
			else
				l = 1.055f * (float)pow(l, 1.0f / 2.4f) - 0.055f;
			sample[i] = l * 255.0f;
		}
	}

	for (i = 0; i < 3; i++) {
		int c = (int)floor(sample[i] + 0.5f);
		if (c < 0) {
			c = 0;
		} else if (c > 255) {
			c = 255;
		}
		colorBytes[i] = (byte)c;
	}
}

void ScanLightmapIntensity(void) {
	int i, j;
	maxLightIntensity = 0.0f;

	if (!lightFloats) return;

	_printf("--- ScanLightmapIntensity ---\n");
	for (i = 0; i < numLightBytes / 3; i++) {
        for (j = 0; j < 3; j++) {
            if (lightFloats[i * 3 + j] > maxLightIntensity) {
                maxLightIntensity = lightFloats[i * 3 + j];
            }
        }
    }
	_printf("Peak lightmap intensity found: %.3f\n", maxLightIntensity);
}

void CheckGridData32(void) {
	if (gridData32) free(gridData32);
	gridData32 = malloc(numGridPoints * sizeof(bspGridPoint32_t));
	if (!gridData32) Error("CheckGridData32: malloc failed");
	memset(gridData32, 0, numGridPoints * sizeof(bspGridPoint32_t));
}

/*
===============================================================================
UP-CONVERSION (8-bit -> 32-bit Float)
===============================================================================
*/

static void UpConvertDrawVerts(void) {
	int i, j, k;
	if (internalDrawVerts) free(internalDrawVerts);
	internalDrawVerts = malloc(MAX_MAP_DRAW_VERTS * sizeof(drawVert32_t));
	if (!internalDrawVerts) Error("UpConvertDrawVerts: malloc failed");
	memset(internalDrawVerts, 0, MAX_MAP_DRAW_VERTS * sizeof(drawVert32_t));

	for (i = 0; i < numDrawVerts; i++) {
		VectorCopy(drawVerts[i].xyz, internalDrawVerts[i].xyz);
		internalDrawVerts[i].st[0] = drawVerts[i].st[0];
		internalDrawVerts[i].st[1] = drawVerts[i].st[1];
		for (j = 0; j < 4; j++) {
			internalDrawVerts[i].lightmap[j][0] = drawVerts[i].lightmap[j][0];
			internalDrawVerts[i].lightmap[j][1] = drawVerts[i].lightmap[j][1];
			// Clean additive start
			for (k = 0; k < 3; k++) internalDrawVerts[i].color[j][k] = 0.0f;
		}
		VectorCopy(drawVerts[i].normal, internalDrawVerts[i].normal);
	}
}

static void UpConvertLightmaps(void) {
	if (lightFloats) free(lightFloats);
	_printf("UpConvert: Allocating %d pixel buffers for lightmaps...\n", numLightBytes / 3);
	lightFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
	if (!lightFloats) Error("UpConvertLightmaps: malloc failed");
	memset(lightFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

	if (!lightAlphaMask) {
		lightAlphaMask = malloc((numLightBytes / 3) * sizeof(byte));
		if (!lightAlphaMask) Error("UpConvert: malloc lightAlphaMask failed");
		memset(lightAlphaMask, 0, (numLightBytes / 3) * sizeof(byte));
	}
}

void UpConvertLightingData(void) {
	_printf("--- UpConvertLightingData ---\n");
	UpConvertDrawVerts();
	UpConvertLightmaps();
	CheckGridData32();
}

/*
===============================================================================
DOWN-CONVERSION (32-bit Float -> 8-bit)
===============================================================================
*/

static void DownConvertDrawVerts(float scale, qboolean lightmapRange) {
	int i, j;
	if (!internalDrawVerts) return;
	_printf("DownConvert: %d DrawVerts\n", numDrawVerts);
	for (i = 0; i < numDrawVerts; i++) {
		for (j = 0; j < 4; j++) {
			if (lightmapRange) {
				InternalColorToBytesScaled(internalDrawVerts[i].color[j], (byte *)drawVerts[i].color[j], scale, g_game->colorsRGB);
			} else {
				InternalColorToBytes(internalDrawVerts[i].color[j], (byte *)drawVerts[i].color[j], g_game->colorsRGB);
			}
		}
	}
}

static void DownConvertLightmaps(float scale, qboolean lightmapRange) {
	int i;
	if (!lightFloats) return;
	_printf("DownConvert: %d Lightmap pixels\n", numLightBytes / 3);
	int processedCount = 0;
	for (i = 0; i < numLightBytes / 3; i++) {
		if (lightmapRange) {
			InternalColorToBytesScaled(&lightFloats[i * 3], &lightBytes[i * 3], scale, g_game->lightmapsRGB);
		} else {
			InternalColorToBytes(&lightFloats[i * 3], &lightBytes[i * 3], g_game->lightmapsRGB);
		}
		if (lightAlphaMask && lightAlphaMask[i]) processedCount++;
	}
	_printf("DownConvert: %d pixels marked in alpha mask\n", processedCount);
}

static void DownConvertGrid(float scale, qboolean lightmapRange) {
	int i, j;
	if (!gridData32) return;
	for (i = 0; i < numGridPoints; i++) {
		for (j = 0; j < 4; j++) {
			if (lightmapRange) {
				InternalColorToBytesScaled(gridData32[i].ambient[j], (byte *)gridData[i].ambient[j], scale, g_game->lightgridRGB);
				InternalColorToBytesScaled(gridData32[i].directed[j], (byte *)gridData[i].directed[j], scale, g_game->lightgridRGB);
			} else {
				InternalColorToBytes(gridData32[i].ambient[j], (byte *)gridData[i].ambient[j], g_game->lightgridRGB);
				InternalColorToBytes(gridData32[i].directed[j], (byte *)gridData[i].directed[j], g_game->lightgridRGB);
			}
		}
		gridData[i].latLong[0] = gridData32[i].latLong[0];
		gridData[i].latLong[1] = gridData32[i].latLong[1];
		for (j = 0; j < 4; j++) {
			gridData[i].styles[j] = gridData32[i].styles[j];
		}
	}
}

void DownConvertLightingData(void) {
	float scale = 1.0f;
	qboolean lightmapRange = (g_game->hdr == HDR_8BIT);

	_printf("--- DownConvertLightingData ---\n");

	if (g_game->writeLightmapSize > 0 && g_game->writeLightmapSize < g_game->lightmapSize) {
		DownscaleLightmaps(g_game->lightmapSize, g_game->writeLightmapSize);
	}

	if (lightmapRange) {
		ScanLightmapIntensity();
		if (maxLightIntensity > 255.0f) {
			maxLightIntensity *= HOTSPOT_TAME_FACTOR;
			scale = 255.0f / maxLightIntensity;
			float engineIntensity = maxLightIntensity / 255.0f;
			_printf("LightingIntensity Normalization active: Scale factor %f (_lightingIntensity %f)\n", scale, engineIntensity);
			SetKeyValue(&entities[0], "_lightingIntensity", va("%f", engineIntensity));
		} else {
			_printf("Normalization: Peak value %.3f <= 255.0, scaling skipped.\n", maxLightIntensity);
			lightmapRange = qfalse;
		}
	}

	DownConvertDrawVerts(scale, lightmapRange);
	DownConvertLightmaps(scale, lightmapRange);
	DownConvertGrid(scale, lightmapRange);

	_printf("DownConvert: Done\n");
}


void AllocateRadiosityFloats(void) {
	if (numLightBytes <= 0) {
		_printf("AllocateRadiosityFloats: ERROR! numLightBytes is %d\n", numLightBytes);
		return;
	}

	if (radiosityFloats)
		free(radiosityFloats);
	if (accumRadiosityFloats)
		free(accumRadiosityFloats);

	_printf("AllocateRadiosityFloats: Allocating %d pixel buffers for radiosity...\n", numLightBytes / 3);
	radiosityFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
	if (!radiosityFloats)
		Error("AllocateRadiosityFloats: malloc failed (radiosity). numLightBytes: %d", numLightBytes);
	_printf("  radiosityFloats: %p. Memsetting...\n", (void *)radiosityFloats);
	memset(radiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));

	_printf("  Allocating accumRadiosityFloats...\n");
	accumRadiosityFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
	if (!accumRadiosityFloats)
		Error("AllocateRadiosityFloats: malloc failed (accum). numLightBytes: %d", numLightBytes);
	_printf("  accumRadiosityFloats: %p. Memsetting...\n", (void *)accumRadiosityFloats);
	memset(accumRadiosityFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
	_printf("AllocateRadiosityFloats: Done.\n");

	// Irradiance vector buffer — 9 floats per pixel (3 channels × 3 components).
	// Only sparse grid pixels are written, but we index into it using the full
	// lightmap pixel index for simplicity. Allocated and freed with the radiosity pass.
	if (irradianceVecFloats) free(irradianceVecFloats);
	irradianceVecFloats = calloc((numLightBytes / 3) * 9, sizeof(float));
	if (!irradianceVecFloats)
		Error("AllocateRadiosityFloats: calloc failed (irradianceVec). numLightBytes: %d", numLightBytes);
}

void FreeRadiosityFloats(void) {
	if (radiosityFloats) {
		free(radiosityFloats);
		radiosityFloats = NULL;
	}
	if (accumRadiosityFloats) {
		free(accumRadiosityFloats);
		accumRadiosityFloats = NULL;
	}
	if (irradianceVecFloats) {
		free(irradianceVecFloats);
		irradianceVecFloats = NULL;
	}
}
