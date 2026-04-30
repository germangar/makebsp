#include "lightdata.h"
#include "../common/mathlib.h"
#include "../common/cmdlib.h"
#include "../shared/mesh.h"
#include <stdlib.h>
#include <string.h>


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
