#include "lightdata.h"
#include "../common/bspfile.h"
#include "../common/cmdlib.h"
#include "../shared/globals.h"
#include "../shared/mesh.h"
#include "../common/imagelib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

drawVert32_t *internalDrawVerts = NULL;
float *lightFloats = NULL;
float *deluxeFloats = NULL;
int *lightSurfaceIndex = NULL;
float *radiosityFloats = NULL;
float *accumRadiosityFloats = NULL;
float *irradianceVecFloats = NULL;  // 9 floats per pixel (3 irradiance vec3s, one per RGB channel)
float *lambertianVecFloats = NULL;  // 3 floats per pixel (weighted direction sum)
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
	float *tempDeluxe = deluxeFloats ? malloc(numLightBytes * sizeof(float)) : NULL;
	byte *tempMask = malloc(numLMs * width * width);

	_printf("Dilating lightmaps (%d passes)...\n", passes);

	for (p = 0; p < passes; p++) {
		memcpy(temp, lightFloats, numLightBytes * sizeof(float));
		if (tempDeluxe) memcpy(tempDeluxe, deluxeFloats, numLightBytes * sizeof(float));
		memcpy(tempMask, lightAlphaMask, numLMs * width * width);

		for (lm = 0; lm < numLMs; lm++) {
			for (y = 0; y < width; y++) {
				for (x = 0; x < width; x++) {
					int idx = (lm * width * width) + y * width + x;
					float energy = temp[idx * 3] + temp[idx * 3 + 1] + temp[idx * 3 + 2];
					if (tempMask[idx] && energy > 0.0002f) continue;

					float sum[3] = {0,0,0}, dsum[3] = {0,0,0}, weight = 0;
					float litSum[3] = {0,0,0}, litDSum[3] = {0,0,0}, litWeight = 0;

					for (j = -1; j <= 1; j++) {
						for (i = -1; i <= 1; i++) {
							if (i == 0 && j == 0) continue;
							int nx = x + i;
							int ny = y + j;
							if (nx >= 0 && nx < width && ny >= 0 && ny < width) {
								int nidx = (lm * width * width) + ny * width + nx;
								if (tempMask[nidx]) {
									float nEnergy = temp[nidx * 3] + temp[nidx * 3 + 1] + temp[nidx * 3 + 2];
									if (nEnergy > 0.0001f) {
										VectorAdd(litSum, &temp[nidx * 3], litSum);
										if (tempDeluxe) VectorAdd(litDSum, &tempDeluxe[nidx * 3], litDSum);
										litWeight += 1.0f;
									}
									VectorAdd(sum, &temp[nidx * 3], sum);
									if (tempDeluxe) VectorAdd(dsum, &tempDeluxe[nidx * 3], dsum);
									weight += 1.0f;
								}
							}
						}
					}
					if (litWeight > 0) {
						VectorScale(litSum, 1.0f / litWeight, &lightFloats[idx * 3]);
						if (deluxeFloats) VectorScale(litDSum, 1.0f / litWeight, &deluxeFloats[idx * 3]);
						lightAlphaMask[idx] = 1;
					} else if (weight > 0) {
						VectorScale(sum, 1.0f / weight, &lightFloats[idx * 3]);
						if (deluxeFloats) VectorScale(dsum, 1.0f / weight, &deluxeFloats[idx * 3]);
						lightAlphaMask[idx] = 1;
					}
				}
			}
		}
	}
	free(temp);
	if (tempDeluxe) free(tempDeluxe);
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
	float *newDeluxe = deluxeFloats ? calloc(newTotalPixels * 3, sizeof(float)) : NULL;
	byte *newMask = calloc(newTotalPixels, sizeof(byte));

	for (i = 0; i < numDrawSurfaces; i++) {
		if (drawSurfaces[i].lightmapNum[0] >= 0) {
			DownscaleSurfaceLightmap(&drawSurfaces[i], ratio, lightFloats, lightAlphaMask, oldW, newFloats, newMask, newW);
			if (newDeluxe) {
				DownscaleSurfaceLightmap(&drawSurfaces[i], ratio, deluxeFloats, lightAlphaMask, oldW, newDeluxe, NULL, newW);
			}
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

	if (deluxeFloats) {
		free(deluxeFloats);
		deluxeFloats = newDeluxe;
	}

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

	if (g_game->deluxeMap) {
		if (deluxeFloats) free(deluxeFloats);
		if (lightSurfaceIndex) free(lightSurfaceIndex);
		_printf("UpConvert: Allocating deluxeMap buffers...\n");
		deluxeFloats = malloc(numLightBytes * sizeof(float));
		if (!deluxeFloats) Error("UpConvert: malloc deluxeFloats failed");
		memset(deluxeFloats, 0, numLightBytes * sizeof(float));

		lightSurfaceIndex = malloc((numLightBytes / 3) * sizeof(int));
		if (!lightSurfaceIndex) Error("UpConvert: malloc lightSurfaceIndex failed");
		for (int i = 0; i < numLightBytes / 3; i++) lightSurfaceIndex[i] = -1;

		if (irradianceVecFloats) free(irradianceVecFloats);
		irradianceVecFloats = calloc((numLightBytes / 3) * 9, sizeof(float));
		if (!irradianceVecFloats) Error("UpConvert: calloc irradianceVecFloats failed");

		if (lambertianVecFloats) free(lambertianVecFloats);
		lambertianVecFloats = calloc((numLightBytes / 3) * 3, sizeof(float));
		if (!lambertianVecFloats) Error("UpConvert: calloc lambertianVecFloats failed");
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

void ResolveIrradianceVectors(void) {
	if (!irradianceVecFloats || !lightFloats || !deluxeFloats) return;

	_printf("ResolveIrradianceVectors: Unified resolution for %d pixels\n", numLightBytes / 3);
	for (int i = 0; i < numLightBytes / 3; i++) {
		vec3_t v[3];
		vec3_t totalDir = {0,0,0};
		
		for (int c = 0; c < 3; c++) {
			v[c][0] = irradianceVecFloats[i * 9 + c * 3 + 0];
			v[c][1] = irradianceVecFloats[i * 9 + c * 3 + 1];
			v[c][2] = irradianceVecFloats[i * 9 + c * 3 + 2];
			VectorAdd(totalDir, v[c], totalDir);
		}

		vec3_t finalDir;
		if (lambertianVecFloats) {
			VectorCopy(&lambertianVecFloats[i * 3], finalDir);
		} else {
			VectorCopy(totalDir, finalDir);
		}
		
		if (VectorNormalize(finalDir, finalDir) > 0) {
			// Dominant direction found
			VectorCopy(finalDir, &deluxeFloats[i * 3]);
			for (int c = 0; c < 3; c++) {
				lightFloats[i * 3 + c] = DotProduct(v[c], finalDir);
				if (lightFloats[i * 3 + c] < 0) lightFloats[i * 3 + c] = 0;
			}
		} else {
			// No direction (pure ambient or dark)
			VectorClear(&deluxeFloats[i * 3]);
			// (lightFloats already has the scalar sum from initial accumulation? No, 
			// let's ensure it has the magnitude/sum here if we want consistency).
		}
	}
}

void DilateDeluxeDirections(int passes) {
	if (!deluxeFloats || !lightFloats) return;

	int width = g_game->lightmapSize;
	int totalPixels = numLightBytes / 3;
	int numLMs = totalPixels / (width * width);
	float *tempDir = malloc(numLightBytes * sizeof(float));
	int p, lm, x, y, i, j;

	_printf("DilateDeluxeDirections: Extrapolating light directions for shadow edges (%d passes)...\n", passes);

	for (p = 0; p < passes; p++) {
		memcpy(tempDir, deluxeFloats, numLightBytes * sizeof(float));

		for (lm = 0; lm < numLMs; lm++) {
			for (y = 0; y < width; y++) {
				for (x = 0; x < width; x++) {
					int idx = (lm * width * width) + y * width + x;
					
					// We only want to "fill in" directions for pixels that are mostly dark
					// (which would otherwise default to the surface normal or zero, causing rims)
					float intensity = lightFloats[idx * 3] + lightFloats[idx * 3 + 1] + lightFloats[idx * 3 + 2];
					if (intensity > 1.0f) continue; // Skip lit pixels

					float bestIntensity = -1.0f;
					int bestIdx = -1;

					// Look for the brightest neighbor to steal the direction from
					for (j = -1; j <= 1; j++) {
						for (i = -1; i <= 1; i++) {
							if (i == 0 && j == 0) continue;
							int nx = x + i;
							int ny = y + j;
							if (nx >= 0 && nx < width && ny >= 0 && ny < width) {
								int nidx = (lm * width * width) + ny * width + nx;
								// Neighbor must be significantly brighter than us
								float nIntensity = lightFloats[nidx * 3] + lightFloats[nidx * 3 + 1] + lightFloats[nidx * 3 + 2];
								if (nIntensity > intensity && nIntensity > bestIntensity) {
									bestIntensity = nIntensity;
									bestIdx = nidx;
								}
							}
						}
					}

					if (bestIdx != -1) {
						VectorCopy(&tempDir[bestIdx * 3], &deluxeFloats[idx * 3]);
					}
				}
			}
		}
	}
	free(tempDir);
}

static void DownConvertDeluxeMaps(void) {
	int i, lm;
	if (!deluxeFloats) return;

	int totalPixels = numLightBytes / 3;
	int numLMs = totalPixels / (g_game->lightmapSize * g_game->lightmapSize);
	int lmSize = g_game->lightmapSize * g_game->lightmapSize * 3;

	_printf("DownConvert: %d DeluxeMap pixels (interleaving as stride 2 for QFusion)\n", totalPixels);

	// Safety check: numLightBytes * 2 must not exceed MAX_MAP_LIGHTING
	if (numLightBytes * 2 > MAX_MAP_LIGHTING) {
		_printf("WARNING: DownConvertDeluxeMaps: Total lighting data exceeds MAX_MAP_LIGHTING! Skipping deluxeMap layer.\n");
		return;
	}

	// Step 1: Interleave. Move standard lightmaps to EVEN slots
	for (lm = numLMs - 1; lm >= 0; lm--) {
		memmove(&lightBytes[(lm * 2) * lmSize], &lightBytes[lm * lmSize], lmSize);
	}

	// Step 2: Encode and write deluxe maps into ODD slots
	for (lm = 0; lm < numLMs; lm++) {
		byte *dst = &lightBytes[(lm * 2 + 1) * lmSize];
		int basePixel = lm * (g_game->lightmapSize * g_game->lightmapSize);

		for (i = 0; i < (g_game->lightmapSize * g_game->lightmapSize); i++) {
			vec3_t dir;
			VectorCopy(&deluxeFloats[(basePixel + i) * 3], dir);

			byte *pixelDst = &dst[i * 3];

			if (VectorNormalize(dir, dir) > 0) {
				pixelDst[0] = (byte)(dir[0] * 127.5f + 127.5f);
				pixelDst[1] = (byte)(dir[1] * 127.5f + 127.5f);
				pixelDst[2] = (byte)(dir[2] * 127.5f + 127.5f);
			} else {
				// Default to Up (0,0,1)
				pixelDst[0] = 127;
				pixelDst[1] = 127;
				pixelDst[2] = 255;
			}
		}
	}

	// Step 3: Update surface lightmap indices. 
	// The engine requires the base lightmap to be on an EVEN index to detect deluxemaps.
	for (i = 0; i < numDrawSurfaces; i++) {
		if (drawSurfaces[i].lightmapNum[0] >= 0) {
			drawSurfaces[i].lightmapNum[0] *= 2;
			// The engine implicitly loads the deluxemap from base + 1.
			// Do NOT set lightmapNum[1], as MAX_LIGHTMAPS is for styles, not layers.

		}
	}

	// Step 4: Ensure UVs are duplicated for the deluxe layer in all vertices
	for (i = 0; i < numDrawVerts; i++) {
		drawVerts[i].lightmap[1][0] = drawVerts[i].lightmap[0][0];
		drawVerts[i].lightmap[1][1] = drawVerts[i].lightmap[0][1];
	}

	numLightBytes *= 2; // Double it!
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
	int i;

	_printf("--- DownConvertLightingData ---\n");
	
	if (g_game->deluxeMap) {
		ResolveIrradianceVectors();
		DilateDeluxeDirections(2); // Fix shadow-edge rims by extrapolating direction
	}

	// Always perform background dilation to prevent bilinear bleeding at shell edges
	DilateLightmapAtlas(g_game->lightmapSize, 2);

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
	DownConvertDeluxeMaps();
	DownConvertGrid(scale, lightmapRange);

/*
	// Export debug lightmaps for inspection
	if (g_game->deluxeMap) {
		int lmSize = g_game->lightmapSize * g_game->lightmapSize * 3;
		int totalLMs = numLightBytes / lmSize;
		_printf("--- Exporting Debug Lightmaps (%d layers) ---\n", totalLMs);
		for (i = 0; i < totalLMs; i++) {
			char debugName[256];
			if (i % 2 == 1) {
				sprintf(debugName, "debug_lightmap_%d.png", i / 2);
			} else {
				sprintf(debugName, "debug_deluxemap_%d.png", i / 2);
			}
			SavePNG(debugName, &lightBytes[i * lmSize], g_game->lightmapSize, g_game->lightmapSize, 3);
		}
	}
*/

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
	if (lambertianVecFloats) {
		free(lambertianVecFloats);
		lambertianVecFloats = NULL;
	}
	if (lightSurfaceIndex) {
		free(lightSurfaceIndex);
		lightSurfaceIndex = NULL;
	}
}
