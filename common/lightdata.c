#include "lightdata.h"
#include "bspfile.h"
#include "cmdlib.h"
#include "globals.h"
#include "mesh.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

drawVert32_t *internalDrawVerts = NULL;
float *lightFloats = NULL;
float *radiosityFloats = NULL;
float *accumRadiosityFloats = NULL;
byte *lightAlphaMask = NULL;
bspGridPoint32_t *gridData32 = NULL;

float maxLightIntensity = 0.0f;
tonemap_t tonemapMode = TONEMAP_LINEAR;


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


void LerpDrawVert32(drawVert32_t *a, drawVert32_t *b, drawVert32_t *out) {
	int i, j;
	for (i = 0; i < 3; i++) {
		out->xyz[i] = 0.5f * (a->xyz[i] + b->xyz[i]);
		out->normal[i] = 0.5f * (a->normal[i] + b->normal[i]);
	}
	VectorNormalize(out->normal, out->normal);

	out->st[0] = 0.5f * (a->st[0] + b->st[0]);
	out->st[1] = 0.5f * (a->st[1] + b->st[1]);

	for (i = 0; i < 4; i++) {
		out->lightmap[i][0] = 0.5f * (a->lightmap[i][0] + b->lightmap[i][0]);
		out->lightmap[i][1] = 0.5f * (a->lightmap[i][1] + b->lightmap[i][1]);

		for (j = 0; j < 3; j++) {
			out->color[i][j] = 0.5f * (a->color[i][j] + b->color[i][j]);
		}
	}
}

void LerpDrawVertAmount32(drawVert32_t *a, drawVert32_t *b, float amount, drawVert32_t *out) {
	int i, j;
	for (i = 0; i < 3; i++) {
		out->xyz[i] = a->xyz[i] + amount * (b->xyz[i] - a->xyz[i]);
		out->normal[i] = a->normal[i] + amount * (b->normal[i] - a->normal[i]);
	}
	VectorNormalize(out->normal, out->normal);

	out->st[0] = a->st[0] + amount * (b->st[0] - a->st[0]);
	out->st[1] = a->st[1] + amount * (b->st[1] - a->st[1]);

	for (i = 0; i < 4; i++) {
		out->lightmap[i][0] = a->lightmap[i][0] + amount * (b->lightmap[i][0] - a->lightmap[i][0]);
		out->lightmap[i][1] = a->lightmap[i][1] + amount * (b->lightmap[i][1] - a->lightmap[i][1]);

		for (j = 0; j < 3; j++) {
			out->color[i][j] = a->color[i][j] + amount * (b->color[i][j] - a->color[i][j]);
		}
	}
}

void FreeMesh32(mesh32_t *m) {
	free(m->verts);
	free(m);
}

mesh32_t *CopyMesh32(mesh32_t *mesh) {
	mesh32_t *out = malloc(sizeof(*out));
	out->width = mesh->width;
	out->height = mesh->height;
	int size = out->width * out->height * sizeof(drawVert32_t);
	out->verts = malloc(size);
	memcpy(out->verts, mesh->verts, size);
	return out;
}

mesh32_t *TransposeMesh32(mesh32_t *in) {
	mesh32_t *out = malloc(sizeof(*out));
	out->width = in->height;
	out->height = in->width;
	out->verts = malloc(out->width * out->height * sizeof(drawVert32_t));
	for (int h = 0; h < in->height; h++) {
		for (int w = 0; w < in->width; w++) {
			out->verts[w * in->height + h] = in->verts[h * in->width + w];
		}
	}
	FreeMesh32(in);
	return out;
}

void MakeMeshNormals32(mesh32_t in) {
	int i, j, k, dist, count, x, y;
	vec3_t normal, sum, base, delta, around[8], temp;
	qboolean good[8], wrapWidth = qfalse, wrapHeight = qfalse;
	static int neighbors[8][2] = {{0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1}};

	for (i = 0; i < in.height; i++) {
		VectorSubtract(in.verts[i*in.width].xyz, in.verts[i*in.width+in.width-1].xyz, delta);
		if (VectorLength(delta) <= 1.0) { wrapWidth = qtrue; break; }
	}
	for (i = 0; i < in.width; i++) {
		VectorSubtract(in.verts[i].xyz, in.verts[i+(in.height-1)*in.width].xyz, delta);
		if (VectorLength(delta) <= 1.0) { wrapHeight = qtrue; break; }
	}

	for (i = 0; i < in.width; i++) {
		for (j = 0; j < in.height; j++) {
			count = 0;
			drawVert32_t *dv = &in.verts[j*in.width+i];
			VectorCopy(dv->xyz, base);
			for (k = 0; k < 8; k++) {
				good[k] = qfalse;
				for (dist = 1; dist <= 3; dist++) {
					x = i + neighbors[k][0] * dist;
					y = j + neighbors[k][1] * dist;
					if (wrapWidth) { if (x < 0) x = in.width - 1 + x; else if (x >= in.width) x = 1 + x - in.width; }
					if (wrapHeight) { if (y < 0) y = in.height - 1 + y; else if (y >= in.height) y = 1 + y - in.height; }
					if (x < 0 || x >= in.width || y < 0 || y >= in.height) break;
					VectorSubtract(in.verts[y*in.width+x].xyz, base, temp);
					if (VectorNormalize(temp, temp) != 0) { good[k] = qtrue; VectorCopy(temp, around[k]); break; }
				}
			}
			VectorClear(sum);
			for (k = 0; k < 8; k++) {
				if (!good[k] || !good[(k+1)&7]) continue;
				CrossProduct(around[(k+1)&7], around[k], normal);
				if (VectorNormalize(normal, normal) != 0) { VectorAdd(normal, sum, sum); count++; }
			}
			if (count == 0) count = 1;
			VectorNormalize(sum, dv->normal);
		}
	}
}

void PutMeshOnCurve32(mesh32_t in) {
	int i, j, l;
	float prev, next;
	for (i = 0; i < in.width; i++) {
		for (j = 1; j < in.height; j += 2) {
			for (l = 0; l < 3; l++) {
				prev = (in.verts[j*in.width+i].xyz[l] + in.verts[(j+1)*in.width+i].xyz[l]) * 0.5f;
				next = (in.verts[j*in.width+i].xyz[l] + in.verts[(j-1)*in.width+i].xyz[l]) * 0.5f;
				in.verts[j*in.width+i].xyz[l] = (prev + next) * 0.5f;
			}
		}
	}
	for (j = 0; j < in.height; j++) {
		for (i = 1; i < in.width; i += 2) {
			for (l = 0; l < 3; l++) {
				prev = (in.verts[j*in.width+i].xyz[l] + in.verts[j*in.width+i+1].xyz[l]) * 0.5f;
				next = (in.verts[j*in.width+i].xyz[l] + in.verts[j*in.width+i-1].xyz[l]) * 0.5f;
				in.verts[j*in.width+i].xyz[l] = (prev + next) * 0.5f;
			}
		}
	}
}

mesh32_t *SubdivideMesh32(mesh32_t in, float maxError, float minLength) {
	int i, j, k, l;
	drawVert32_t prev, next, mid;
	vec3_t prevxyz, nextxyz, midxyz, delta;
	mesh32_t out;
	drawVert32_t (*expand)[MAX_EXPANDED_AXIS] = malloc(sizeof(drawVert32_t) * MAX_EXPANDED_AXIS * MAX_EXPANDED_AXIS);

	if (!expand) Error("SubdivideMesh32: malloc failed");
	out.width = in.width; out.height = in.height;
	for (i = 0; i < in.width; i++) for (j = 0; j < in.height; j++) expand[j][i] = in.verts[j * in.width + i];

	for (j = 0; j + 2 < out.width; j += 2) {
		for (i = 0; i < out.height; i++) {
			for (l = 0; l < 3; l++) {
				prevxyz[l] = expand[i][j + 1].xyz[l] - expand[i][j].xyz[l];
				nextxyz[l] = expand[i][j + 2].xyz[l] - expand[i][j + 1].xyz[l];
				midxyz[l] = (expand[i][j].xyz[l] + expand[i][j + 1].xyz[l] * 2 + expand[i][j + 2].xyz[l]) * 0.25f;
			}
			if (VectorLength(prevxyz) > minLength || VectorLength(nextxyz) > minLength) break;
			VectorSubtract(expand[i][j + 1].xyz, midxyz, delta);
			if (VectorLength(delta) > maxError) break;
		}
		if (out.width + 2 >= MAX_EXPANDED_AXIS || i == out.height) { if (i == out.height) continue; break; }
		out.width += 2;
		for (i = 0; i < out.height; i++) {
			LerpDrawVert32(&expand[i][j], &expand[i][j + 1], &prev);
			LerpDrawVert32(&expand[i][j + 1], &expand[i][j + 2], &next);
			LerpDrawVert32(&prev, &next, &mid);
			for (k = out.width - 1; k > j + 3; k--) expand[i][k] = expand[i][k - 2];
			expand[i][j + 1] = prev; expand[i][j + 2] = mid; expand[i][j + 3] = next;
		}
		j -= 2;
	}

	for (j = 0; j + 2 < out.height; j += 2) {
		for (i = 0; i < out.width; i++) {
			for (l = 0; l < 3; l++) {
				prevxyz[l] = expand[j + 1][i].xyz[l] - expand[j][i].xyz[l];
				nextxyz[l] = expand[j + 2][i].xyz[l] - expand[j + 1][i].xyz[l];
				midxyz[l] = (expand[j][i].xyz[l] + expand[j + 1][i].xyz[l] * 2 + expand[j + 2][i].xyz[l]) * 0.25f;
			}
			if (VectorLength(prevxyz) > minLength || VectorLength(nextxyz) > minLength) break;
			VectorSubtract(expand[j + 1][i].xyz, midxyz, delta);
			if (VectorLength(delta) > maxError) break;
		}
		if (out.height + 2 >= MAX_EXPANDED_AXIS || i == out.width) { if (i == out.width) continue; break; }
		out.height += 2;
		for (i = 0; i < out.width; i++) {
			LerpDrawVert32(&expand[j][i], &expand[j + 1][i], &prev);
			LerpDrawVert32(&expand[j + 1][i], &expand[j + 2][i], &next);
			LerpDrawVert32(&prev, &next, &mid);
			for (k = out.height - 1; k > j + 3; k--) expand[k][i] = expand[k - 2][i];
			expand[j + 1][i] = prev; expand[j + 2][i] = mid; expand[j + 3][i] = next;
		}
		j -= 2;
	}

	out.verts = &expand[0][0];
	for (i = 1; i < out.height; i++) memmove(&out.verts[i * out.width], expand[i], out.width * sizeof(drawVert32_t));
	mesh32_t *result = CopyMesh32(&out);
	free(expand);
	return result;
}

mesh32_t *SubdivideMeshQuads32(mesh32_t *in, float minLength, int maxsize, int widthtable[], int heighttable[]) {
	int i, j, k, w, h, maxsubdivisions, subdivisions;
	vec3_t dir;
	float maxLength, amount;
	mesh32_t out;
	drawVert32_t (*expand)[MAX_EXPANDED_AXIS] = malloc(sizeof(drawVert32_t) * MAX_EXPANDED_AXIS * MAX_EXPANDED_AXIS);

	if (!expand) Error("SubdivideMeshQuads32: malloc failed");
	out.width = in->width; out.height = in->height;
	for (i = 0; i < in->width; i++) for (j = 0; j < in->height; j++) expand[j][i] = in->verts[j * in->width + i];

	maxsubdivisions = (maxsize - in->width) / (in->width - 1);
	for (w = 0, j = 0; w < in->width - 1; w++, j += subdivisions + 1) {
		maxLength = 0;
		for (i = 0; i < out.height; i++) {
			VectorSubtract(expand[i][j + 1].xyz, expand[i][j].xyz, dir);
			if (VectorLength(dir) > maxLength) maxLength = VectorLength(dir);
		}
		subdivisions = (int)(maxLength / minLength);
		if (subdivisions > maxsubdivisions) subdivisions = maxsubdivisions;
		widthtable[w] = subdivisions + 1;
		if (subdivisions <= 0) continue;
		out.width += subdivisions;
		for (i = 0; i < out.height; i++) {
			for (k = out.width - 1; k > j + subdivisions; k--) expand[i][k] = expand[i][k - subdivisions];
			for (k = 1; k <= subdivisions; k++) {
				amount = (float)k / (subdivisions + 1);
				LerpDrawVertAmount32(&expand[i][j], &expand[i][j + subdivisions + 1], amount, &expand[i][j + k]);
			}
		}
	}

	maxsubdivisions = (maxsize - in->height) / (in->height - 1);
	for (h = 0, j = 0; h < in->height - 1; h++, j += subdivisions + 1) {
		maxLength = 0;
		for (i = 0; i < out.width; i++) {
			VectorSubtract(expand[j + 1][i].xyz, expand[j][i].xyz, dir);
			if (VectorLength(dir) > maxLength) maxLength = VectorLength(dir);
		}
		subdivisions = (int)(maxLength / minLength);
		if (subdivisions > maxsubdivisions) subdivisions = maxsubdivisions;
		heighttable[h] = subdivisions + 1;
		if (subdivisions <= 0) continue;
		out.height += subdivisions;
		for (i = 0; i < out.width; i++) {
			for (k = out.height - 1; k > j + subdivisions; k--) expand[k][i] = expand[k - subdivisions][i];
			for (k = 1; k <= subdivisions; k++) {
				amount = (float)k / (subdivisions + 1);
				LerpDrawVertAmount32(&expand[j][i], &expand[j + subdivisions + 1][i], amount, &expand[j + k][i]);
			}
		}
	}

	out.verts = &expand[0][0];
	for (i = 1; i < out.height; i++) memmove(&out.verts[i * out.width], expand[i], out.width * sizeof(drawVert32_t));
	mesh32_t *result = CopyMesh32(&out);
	free(expand);
	return result;
}

void ProjectPointOntoVector32(vec3_t point, vec3_t vStart, vec3_t vEnd, vec3_t vProj) {
	vec3_t pVec, vec;
	VectorSubtract(point, vStart, pVec);
	VectorSubtract(vEnd, vStart, vec);
	VectorNormalize(vec, vec);
	VectorMA(vStart, DotProduct(pVec, vec), vec, vProj);
}

mesh32_t *RemoveLinearMeshColumnsRows32(mesh32_t *in) {
	int i, j, k;
	float maxLength;
	vec3_t proj, dir;
	mesh32_t out;
	drawVert32_t (*expand)[MAX_EXPANDED_AXIS] = malloc(sizeof(drawVert32_t) * MAX_EXPANDED_AXIS * MAX_EXPANDED_AXIS);

	if (!expand) Error("RemoveLinearMeshColumnsRows32: malloc failed");
	out.width = in->width; out.height = in->height;
	for (i = 0; i < in->width; i++) for (j = 0; j < in->height; j++) expand[j][i] = in->verts[j * in->width + i];

	for (j = 1; j < out.width - 1; j++) {
		maxLength = 0;
		for (i = 0; i < out.height; i++) {
			ProjectPointOntoVector32(expand[i][j].xyz, expand[i][j - 1].xyz, expand[i][j + 1].xyz, proj);
			VectorSubtract(expand[i][j].xyz, proj, dir);
			if (VectorLength(dir) > maxLength) maxLength = VectorLength(dir);
		}
		if (maxLength < 0.1f) {
			out.width--;
			for (i = 0; i < out.height; i++) for (k = j; k < out.width; k++) expand[i][k] = expand[i][k + 1];
			j--;
		}
	}
	for (j = 1; j < out.height - 1; j++) {
		maxLength = 0;
		for (i = 0; i < out.width; i++) {
			ProjectPointOntoVector32(expand[j][i].xyz, expand[j - 1][i].xyz, expand[j + 1][i].xyz, proj);
			VectorSubtract(expand[j][i].xyz, proj, dir);
			if (VectorLength(dir) > maxLength) maxLength = VectorLength(dir);
		}
		if (maxLength < 0.1f) {
			out.height--;
			for (i = 0; i < out.width; i++) for (k = j; k < out.height; k++) expand[k][i] = expand[k + 1][i];
			j--;
		}
	}
	out.verts = &expand[0][0];
	for (i = 1; i < out.height; i++) memmove(&out.verts[i * out.width], expand[i], out.width * sizeof(drawVert32_t));
	mesh32_t *result = CopyMesh32(&out);
	free(expand);
	return result;
}

void CheckGridData32(void) {
	if (gridData32) free(gridData32);
	gridData32 = malloc(numGridPoints * sizeof(bspGridPoint32_t));
	if (!gridData32) Error("CheckGridData32: malloc failed");
	memset(gridData32, 0, numGridPoints * sizeof(bspGridPoint32_t));
}

void UpConvertLightingData(void) {
	int i, j, k;

	_printf("--- UpConvertLightingData ---\n");

	// 1. Draw Verts
	if (internalDrawVerts) free(internalDrawVerts);
	internalDrawVerts = malloc(MAX_MAP_DRAW_VERTS * sizeof(drawVert32_t));
	if (!internalDrawVerts) Error("UpConvertLightingData: malloc internalDrawVerts failed");
	memset(internalDrawVerts, 0, MAX_MAP_DRAW_VERTS * sizeof(drawVert32_t));

	for (i = 0; i < numDrawVerts; i++) {
		for (k = 0; k < 3; k++) internalDrawVerts[i].xyz[k] = drawVerts[i].xyz[k];
		internalDrawVerts[i].st[0] = drawVerts[i].st[0];
		internalDrawVerts[i].st[1] = drawVerts[i].st[1];
		for (j = 0; j < 4; j++) {
			internalDrawVerts[i].lightmap[j][0] = drawVerts[i].lightmap[j][0];
			internalDrawVerts[i].lightmap[j][1] = drawVerts[i].lightmap[j][1];
			// surgically zero out internal colors to ensure a clean additive start
			for (k = 0; k < 3; k++) {
				internalDrawVerts[i].color[j][k] = 0.0f;
			}
		}
		for (k = 0; k < 3; k++) internalDrawVerts[i].normal[k] = drawVerts[i].normal[k];
	}

	// 2. Lightmaps - Start from zero (total blackness) for the new passes
	if (lightFloats) free(lightFloats);
	_printf("UpConvertLightingData: Allocating %d pixel buffers for lightmaps...\n", numLightBytes / 3);
	lightFloats = malloc((numLightBytes / 3) * sizeof(vec3_t));
	if (!lightFloats) Error("UpConvertLightingData: malloc lightFloats failed");
	memset(lightFloats, 0, (numLightBytes / 3) * sizeof(vec3_t));
	// Note: We skip loading from lightBytes to ensure a clean additive start

	// 2.5 Alpha Mask - Persistent across multiple passes
	// We only allocate it the first time it's needed
	if (!lightAlphaMask) {
		lightAlphaMask = malloc((numLightBytes / 3) * sizeof(byte));
		if (!lightAlphaMask) Error("UpConvertLightingData: malloc lightAlphaMask failed");
		memset(lightAlphaMask, 0, (numLightBytes / 3) * sizeof(byte));
	}

	// 3. Light Grid - Start from zero (total blackness) for the new passes
	CheckGridData32();
	// Note: CheckGridData32 already memsets the whole gridData32 array to 0, 
	// which is appropriate since it is entirely comprised of lighting data.
}

void DownConvertLightingData(void) {
	int i, j;
	float scale = 1.0f;
    qboolean lightmapRange = (g_game->hdr == HDR_8BIT);

	_printf("--- DownConvertLightingData ---\n");

	if (lightmapRange) {
		ScanLightmapIntensity();
		if (maxLightIntensity > 255.0f) {
			scale = 255.0f / maxLightIntensity;
			float engineIntensity = maxLightIntensity / 255.0f;
			_printf("Normalization active: Scale factor %f (_lightingIntensity %f)\n", scale, engineIntensity);
			SetKeyValue(&entities[0], "_lightingIntensity", va("%f", engineIntensity));
		} else {
			_printf("Normalization: Peak value %.3f <= 255.0, scaling skipped.\n", maxLightIntensity);
			lightmapRange = qfalse; // Disable scaling for this pass
		}
	}

	// 1. Draw Verts
	if (!internalDrawVerts) { _printf("DownConvert: internalDrawVerts is NULL\n"); return; }
	_printf("DownConvert: Converting %d DrawVerts\n", numDrawVerts);
	for (i = 0; i < numDrawVerts; i++) {
		for (j = 0; j < 4; j++) {
			if (lightmapRange) {
				InternalColorToBytesScaled(internalDrawVerts[i].color[j], (byte *)drawVerts[i].color[j], scale, g_game->colorsRGB);
			} else {
				InternalColorToBytes(internalDrawVerts[i].color[j], (byte *)drawVerts[i].color[j], g_game->colorsRGB);
			}
		}
	}

	// 2. Lightmaps
	if (!lightFloats) { _printf("DownConvert: lightFloats is NULL\n"); return; }
	_printf("DownConvert: Converting %d Lightmap pixels\n", numLightBytes / 3);
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

	// 3. Light Grid
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
}
