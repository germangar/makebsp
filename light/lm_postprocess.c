#include "light.h"
#include <omp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
===============================================================================

LIGHTMAP POST-PROCESSING (SMOOTHING & AA)

This module implements multi-threaded post-process filters for lightmaps.
It features cross-surface sampling to prevent seams between coplanar
world surfaces.

===============================================================================
*/

static int lightmapAA;
static float lightmapSmoothRadius;
static int lightmapSmoothPasses;

/*
 * FILTER_UPSCALE: 1 = perform all GPU filtering at 2x resolution (higher quality).
 * If enabled, the atlas is upscaled during upload and box-filtered down during download.
 */
#define FILTER_UPSCALE 1

#define TRISOUP_SMOOTH_CHEAT(A) ((A) + (useOpenCL ? 1.0f : 1.5f)) // offset smoothing radius for trisoups to get a closer result to world surfaces.

#define AA_ANGLE_MATCH_DEGREES 30.0f

#define MAX_KERNEL_RADIUS 16

typedef struct {
    vec3_t pos;
    vec3_t normal;
    qboolean valid;
} pixelCache_t;

// --- Planar Surface Indexing for Cross-Surface Filtering ---

typedef struct {
	int surfaceNum;
	vec3_t origin;
	vec3_t vecs[2];
	float invMagSq[2];
	int width, height;
	int lmNum;
	int lmOffset[2];
	vec3_t normal;
	float dist;
	int surfaceFlags;
	int contentFlags;
	
	int numPartners;
	int *partners; // Indices into planarSurfaces array
} planarInfo_t;

static planarInfo_t *planarSurfaces = NULL;
static int numPlanarSurfaces = 0;

typedef struct {
	vec3_t normal;
	float dist;
	int firstSurface;
	int numSurfaces;
} planeGroup_t;

static planeGroup_t *planeGroups = NULL;
static int numPlaneGroups = 0;
static int *planarSortIndex = NULL;

static int ComparePlanarInfo(const void *a, const void *b) {
	const planarInfo_t *pa = &planarSurfaces[*(const int *)a];
	const planarInfo_t *pb = &planarSurfaces[*(const int *)b];

	for (int i = 0; i < 3; i++) {
		if (pa->normal[i] < pb->normal[i] - 0.0001f) return -1;
		if (pa->normal[i] > pb->normal[i] + 0.0001f) return 1;
	}
	if (pa->dist < pb->dist - 0.01f) return -1;
	if (pa->dist > pb->dist + 0.01f) return 1;
	return 0;
}

#define POS_TO_INT(p) ((int)roundf((p) * 128.0f))

// Edge tracking for adjacency
typedef struct {
	int v[2][3]; // Snapped world-space coordinates
} edge_t;

typedef struct {
	edge_t edge;
	int surfaceIdx; // Index into planarSurfaces
} edgeRef_t;

static int CompareEdges(const void *a, const void *b) {
	const edgeRef_t *ea = (const edgeRef_t *)a;
	const edgeRef_t *eb = (const edgeRef_t *)b;
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 3; j++) {
			if (ea->edge.v[i][j] < eb->edge.v[i][j]) return -1;
			if (ea->edge.v[i][j] > eb->edge.v[i][j]) return 1;
		}
	}
	return 0;
}

/*
================
BuildPlanarSurfaceIndex

Builds a list of planar surfaces and determines adjacency via geometric shared edges.
================
*/
void BuildPlanarSurfaceIndex(void) {
	int i, j, k;
	if (planarSurfaces) {
		for (i = 0; i < numPlanarSurfaces; i++) {
			if (planarSurfaces[i].partners) free(planarSurfaces[i].partners);
		}
		free(planarSurfaces);
	}
	if (planarSortIndex) free(planarSortIndex);
	if (planeGroups) free(planeGroups);

	planarSurfaces = malloc(numDrawSurfaces * sizeof(planarInfo_t));
	planarSortIndex = malloc(numDrawSurfaces * sizeof(int));
	numPlanarSurfaces = 0;

	for (i = 0; i < numDrawSurfaces; i++) {
		dsurface_t *ds = &drawSurfaces[i];
		if (ds->lightmapNum[0] < 0) continue;
		/* Include all types that have lightmaps (Planar and Patches) */
		if (ds->surfaceType != MST_PLANAR && ds->surfaceType != MST_PATCH) continue;

		planarInfo_t *p = &planarSurfaces[numPlanarSurfaces];
		planarSortIndex[numPlanarSurfaces] = numPlanarSurfaces;
		numPlanarSurfaces++;

		p->surfaceNum = i;
		VectorCopy(ds->lightmapOrigin, p->origin);
		if (ds->surfaceType == MST_PLANAR) {
			VectorMA(p->origin, -0.5f, ds->lightmapVecs[0], p->origin);
			VectorMA(p->origin, -0.5f, ds->lightmapVecs[1], p->origin);
		}
		VectorAdd(p->origin, localSurfaces[i].entityOrigin, p->origin);
		VectorCopy(ds->lightmapVecs[0], p->vecs[0]);
		VectorCopy(ds->lightmapVecs[1], p->vecs[1]);
		p->invMagSq[0] = 1.0f / DotProduct(p->vecs[0], p->vecs[0]);
		p->invMagSq[1] = 1.0f / DotProduct(p->vecs[1], p->vecs[1]);
		p->width = ds->lightmapWidth;
		p->height = ds->lightmapHeight;
		p->lmNum = ds->lightmapNum[0];
		p->lmOffset[0] = ds->lightmapOffset[0][0];
		p->lmOffset[1] = ds->lightmapOffset[0][1];

		CrossProduct(p->vecs[0], p->vecs[1], p->normal);
		VectorNormalize(p->normal, p->normal);
		p->dist = DotProduct(p->origin, p->normal);

		p->surfaceFlags = dshaders[ds->shaderNum].surfaceFlags;
		p->contentFlags = dshaders[ds->shaderNum].contentFlags;
		
		p->numPartners = 0;
		p->partners = NULL;
	}

	if (numPlanarSurfaces == 0) return;

	// Adjacency Detection via Shared Geometric Edges
	int maxPossibleEdges = numDrawIndexes; 
	edgeRef_t *allEdges = malloc(maxPossibleEdges * sizeof(edgeRef_t));
	int numEdges = 0;

	for (i = 0; i < numPlanarSurfaces; i++) {
		dsurface_t *ds = &drawSurfaces[planarSurfaces[i].surfaceNum];
		for (j = 0; j < ds->numIndexes; j += 3) {
			// For each triangle in the planar surface
			for (k = 0; k < 3; k++) {
				int idx1 = ds->firstVert + drawIndexes[ds->firstIndex + j + k];
				int idx2 = ds->firstVert + drawIndexes[ds->firstIndex + j + ((k+1)%3)];
				
				vec3_t p1, p2;
				VectorCopy(drawVerts[idx1].xyz, p1);
				VectorCopy(drawVerts[idx2].xyz, p2);
				
				int ip1[3] = { POS_TO_INT(p1[0]), POS_TO_INT(p1[1]), POS_TO_INT(p1[2]) };
				int ip2[3] = { POS_TO_INT(p2[0]), POS_TO_INT(p2[1]), POS_TO_INT(p2[2]) };
				
				edgeRef_t *edge = &allEdges[numEdges++];
				edge->surfaceIdx = i;
				
				// Sort the two vertices to ensure consistent edge representation
				qboolean p1First = qtrue;
				if (ip1[0] > ip2[0]) p1First = qfalse;
				else if (ip1[0] == ip2[0] && ip1[1] > ip2[1]) p1First = qfalse;
				else if (ip1[0] == ip2[0] && ip1[1] == ip2[1] && ip1[2] > ip2[2]) p1First = qfalse;
				
				if (p1First) {
					edge->edge.v[0][0] = ip1[0]; edge->edge.v[0][1] = ip1[1]; edge->edge.v[0][2] = ip1[2];
					edge->edge.v[1][0] = ip2[0]; edge->edge.v[1][1] = ip2[1]; edge->edge.v[1][2] = ip2[2];
				} else {
					edge->edge.v[0][0] = ip2[0]; edge->edge.v[0][1] = ip2[1]; edge->edge.v[0][2] = ip2[2];
					edge->edge.v[1][0] = ip1[0]; edge->edge.v[1][1] = ip1[1]; edge->edge.v[1][2] = ip1[2];
				}
			}
		}
	}

	// Sort edges to find matches
	qsort(allEdges, numEdges, sizeof(edgeRef_t), CompareEdges);

	#define LIQUID_CONTENTS (CONTENTS_LAVA | CONTENTS_WATER | CONTENTS_SLIME)

	// Link surfaces that share a geometric edge AND are coplanar
	for (i = 0; i < numEdges; ) {
		int next = i + 1;
		while (next < numEdges && CompareEdges(&allEdges[i], &allEdges[next]) == 0) {
			next++;
		}

		if (next > i + 1) {
			// allEdges[i...next-1] all share the same edge
			for (j = i; j < next; j++) {
				for (k = j + 1; k < next; k++) {
					int s1 = allEdges[j].surfaceIdx;
					int s2 = allEdges[k].surfaceIdx;
					if (s1 == s2) continue;

					planarInfo_t *p1 = &planarSurfaces[s1];
					planarInfo_t *p2 = &planarSurfaces[s2];
					
					// Compatibility constraints:
					if (DotProduct(p1->normal, p2->normal) < 0.99f) continue;
					if (fabs(p1->dist - p2->dist) > 0.1f) continue;
					if (p1->surfaceFlags & SURF_SKY || p2->surfaceFlags & SURF_SKY) continue;
					if ((p1->contentFlags & LIQUID_CONTENTS) != (p2->contentFlags & LIQUID_CONTENTS)) continue;

					// Add s2 to s1's partners
					qboolean found = qfalse;
					for (int m = 0; m < p1->numPartners; m++) if (p1->partners[m] == s2) { found = qtrue; break; }
					if (!found) {
						p1->partners = realloc(p1->partners, (p1->numPartners + 1) * sizeof(int));
						p1->partners[p1->numPartners++] = s2;
					}

					// Add s1 to s2's partners
					found = qfalse;
					for (int m = 0; m < p2->numPartners; m++) if (p2->partners[m] == s1) { found = qtrue; break; }
					if (!found) {
						p2->partners = realloc(p2->partners, (p2->numPartners + 1) * sizeof(int));
						p2->partners[p2->numPartners++] = s1;
					}
				}
			}
		}
		i = next;
	}

	free(allEdges);

	// Sort surfaces by plane for remaining plane-group lookups (fallback)
	qsort(planarSortIndex, numPlanarSurfaces, sizeof(int), ComparePlanarInfo);

	planeGroups = malloc(numPlanarSurfaces * sizeof(planeGroup_t));
	numPlaneGroups = 0;

	for (i = 0; i < numPlanarSurfaces; i++) {
		planarInfo_t *p = &planarSurfaces[planarSortIndex[i]];
		if (i == 0 || ComparePlanarInfo(&planarSortIndex[i], &planarSortIndex[i-1]) != 0) {
			planeGroup_t *g = &planeGroups[numPlaneGroups++];
			VectorCopy(p->normal, g->normal);
			g->dist = p->dist;
			g->firstSurface = i;
			g->numSurfaces = 1;
		} else {
			planeGroups[numPlaneGroups-1].numSurfaces++;
		}
	}
}

void FreePlanarSurfaceIndex(void) {
	if (planarSurfaces) {
		for (int i = 0; i < numPlanarSurfaces; i++) {
			if (planarSurfaces[i].partners) free(planarSurfaces[i].partners);
		}
		free(planarSurfaces);
	}
	if (planarSortIndex) free(planarSortIndex);
	if (planeGroups) free(planeGroups);
	planarSurfaces = NULL;
	planarSortIndex = NULL;
	planeGroups = NULL;
	numPlanarSurfaces = 0;
	numPlaneGroups = 0;
}

/*
================
SampleLightmapWorldBilinear

Finds and samples a lightmap pixel at a world position, crossing surfaces.
ONLY considers adjacent surfaces (sharing an edge) to prevent leaks.
================
*/
qboolean SampleLightmapWorldBilinear(int sourceSrfIdx, const vec3_t pos, const vec3_t normal, float *outColor, const float *buffer) {
	int i;
	if (sourceSrfIdx < 0 || sourceSrfIdx >= numPlanarSurfaces) return qfalse;

	planarInfo_t *srcP = &planarSurfaces[sourceSrfIdx];

	// Search ONLY the partners (shared edges)
	for (i = 0; i < srcP->numPartners; i++) {
		planarInfo_t *p = &planarSurfaces[srcP->partners[i]];
		
		vec3_t delta;
		VectorSubtract(pos, p->origin, delta);
		float u = DotProduct(delta, p->vecs[0]) * p->invMagSq[0];
		float v = DotProduct(delta, p->vecs[1]) * p->invMagSq[1];

		// The physical extent of the lightmap is [-0.5, width - 0.5].
		// Allow a tiny epsilon for float precision.
		if (u < -0.51f || u > (float)p->width - 0.49f || v < -0.51f || v > (float)p->height - 0.49f) continue;

		// Bilinear sample (shift to node-relative coordinates, centers at 0.5, 1.5...)
		float ux = u - 0.5f;
		float vy = v - 0.5f;

		int x0 = (int)floorf(ux);
		int y0 = (int)floorf(vy);
		float fx = ux - x0;
		float fy = vy - y0;

		int x1 = x0 + 1, y1 = y0 + 1;
		if (x0 < 0) x0 = 0; 
		if (x0 >= p->width) x0 = p->width - 1;
		if (x1 < 0) x1 = 0; 
		if (x1 >= p->width) x1 = p->width - 1;
		if (y0 < 0) y0 = 0; 
		if (y0 >= p->height) y0 = p->height - 1;
		if (y1 < 0) y1 = 0; 
		if (y1 >= p->height) y1 = p->height - 1;

		int p00 = (p->lmNum * LIGHTMAP_HEIGHT + p->lmOffset[1] + y0) * LIGHTMAP_WIDTH + p->lmOffset[0] + x0;
		int p10 = (p->lmNum * LIGHTMAP_HEIGHT + p->lmOffset[1] + y0) * LIGHTMAP_WIDTH + p->lmOffset[0] + x1;
		int p01 = (p->lmNum * LIGHTMAP_HEIGHT + p->lmOffset[1] + y1) * LIGHTMAP_WIDTH + p->lmOffset[0] + x0;
		int p11 = (p->lmNum * LIGHTMAP_HEIGHT + p->lmOffset[1] + y1) * LIGHTMAP_WIDTH + p->lmOffset[0] + x1;

		float w00 = (1.0f - fx) * (1.0f - fy);
		float w10 = fx * (1.0f - fy);
		float w01 = (1.0f - fx) * fy;
		float w11 = fx * fy;

		if (lightAlphaMask[p00] != ALPHA_SURF_WORLD) w00 = 0.0f;
		if (lightAlphaMask[p10] != ALPHA_SURF_WORLD) w10 = 0.0f;
		if (lightAlphaMask[p01] != ALPHA_SURF_WORLD) w01 = 0.0f;
		if (lightAlphaMask[p11] != ALPHA_SURF_WORLD) w11 = 0.0f;

		float sumW = w00 + w10 + w01 + w11;
		if (sumW > 0.01f) {
			for (int c = 0; c < 3; c++) {
				outColor[c] = (w00 * buffer[p00*3+c] + w10 * buffer[p10*3+c] + w01 * buffer[p01*3+c] + w11 * buffer[p11*3+c]) / sumW;
			}
			return qtrue;
		}
	}
	return qfalse;
}

/*
================
GetFilteredTexel

Universal helper to fetch a color at (px, py) relative to planar surface 'sIdx'.
'sIdx' is the index into the planarSurfaces array.
================
*/
static qboolean GetFilteredTexel(int sIdx, float px, float py, float *outColor, const float *buffer) {
	planarInfo_t *pInfo = &planarSurfaces[sIdx];
	dsurface_t *ds = &drawSurfaces[pInfo->surfaceNum];
	
	// Shift to node-relative coordinates (centers at 0.5, 1.5...)
	float ux = px - 0.5f;
	float vy = py - 0.5f;

	int x0 = (int)floorf(ux);
	int y0 = (int)floorf(vy);
	float fx = ux - (float)x0;
	float fy = vy - (float)y0;
	
	qboolean needsX1 = (fx > 0.001f);
	qboolean needsY1 = (fy > 0.001f);
	
	int maxX = needsX1 ? x0 + 1 : x0;
	int maxY = needsY1 ? y0 + 1 : y0;

	// Fast path: strictly inside local bounds (no chance of atlas leaking)
	if (x0 >= 0 && maxX < ds->lightmapWidth && 
	    y0 >= 0 && maxY < ds->lightmapHeight) {
		
		int x1 = x0 + 1, y1 = y0 + 1;
		if (x1 >= ds->lightmapWidth) x1 = ds->lightmapWidth - 1;
		if (y1 >= ds->lightmapHeight) y1 = ds->lightmapHeight - 1;

		int p00 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0;
		int p10 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y0) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1;
		int p01 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x0;
		int p11 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y1) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x1;

		float w00 = (1.0f - fx) * (1.0f - fy);
		float w10 = fx * (1.0f - fy);
		float w01 = (1.0f - fx) * fy;
		float w11 = fx * fy;

		if (lightAlphaMask[p00] != ALPHA_SURF_WORLD) w00 = 0.0f;
		if (lightAlphaMask[p10] != ALPHA_SURF_WORLD) w10 = 0.0f;
		if (lightAlphaMask[p01] != ALPHA_SURF_WORLD) w01 = 0.0f;
		if (lightAlphaMask[p11] != ALPHA_SURF_WORLD) w11 = 0.0f;

		float sumW = w00 + w10 + w01 + w11;
		if (sumW > 0.01f) {
			for (int c = 0; c < 3; c++) {
				outColor[c] = (w00 * buffer[p00*3+c] + w10 * buffer[p10*3+c] + w01 * buffer[p01*3+c] + w11 * buffer[p11*3+c]) / sumW;
			}
			return qtrue;
		}
	}

	// Slow path: cross-surface lookup (strictly via shared edges)
	vec3_t worldPos;
	VectorMA(pInfo->origin, px, pInfo->vecs[0], worldPos);
	VectorMA(worldPos, py, pInfo->vecs[1], worldPos);
	
	if (SampleLightmapWorldBilinear(sIdx, worldPos, ds->lightmapVecs[2], outColor, buffer)) {
		return qtrue;
	}

	// Fallback: if no neighbor is found, clamp to our own surface edge (standard texture clamping)
	int cx = x0;
	if (cx < 0) cx = 0;
	if (cx >= ds->lightmapWidth) cx = ds->lightmapWidth - 1;

	int cy = y0;
	if (cy < 0) cy = 0;
	if (cy >= ds->lightmapHeight) cy = ds->lightmapHeight - 1;

	int p00 = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + cy) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + cx;
	if (lightAlphaMask[p00] != ALPHA_SURF_WORLD) return qfalse;

	outColor[0] = buffer[p00*3+0];
	outColor[1] = buffer[p00*3+1];
	outColor[2] = buffer[p00*3+2];
	return qtrue;
}

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

// ---------------------------------------------------------------------------
// Per-Surface Volumetric Post-Process Smoothing (VPPS) for Triangle Soups
// ---------------------------------------------------------------------------

#define AA_ANGLE_MATCH_COS 0.85f

typedef struct aaTexel_s {
    vec3_t pos;
    vec3_t normal;
    vec3_t color;
    struct aaTexel_s *next;
} aaTexel_t;

// Cached world-space position/normal for a single lightmap pixel center.
// Avoids redundant TriSoupSamplePoint calls across the three passes of VPPS.

/*
================
GetSurfaceTexelSize

Calculates the actual physical size (in world units) of one lightmap texel
on the given surface by measuring the ratio of world distance to UV distance
across all its triangles.
================
*/
static float GetSurfaceTexelSize(dsurface_t *ds) {
    if (ds->numIndexes == 0) return (float)samplesize;

    float totalWorldDist = 0.0f;
    float totalUVDist = 0.0f;

    for (int j = 0; j < ds->numIndexes; j += 3) {
        for (int k = 0; k < 3; k++) {
            int i0 = drawIndexes[ds->firstIndex + j + k];
            int i1 = drawIndexes[ds->firstIndex + j + ((k + 1) % 3)];

            drawVert_t *v0 = &drawVerts[ds->firstVert + i0];
            drawVert_t *v1 = &drawVerts[ds->firstVert + i1];

            vec3_t deltaWorld;
            VectorSubtract(v0->xyz, v1->xyz, deltaWorld);
            float worldDist = VectorLength(deltaWorld);

            float deltaU = (v0->lightmap[0][0] - v1->lightmap[0][0]) * (float)LIGHTMAP_WIDTH;
            float deltaV = (v0->lightmap[0][1] - v1->lightmap[0][1]) * (float)LIGHTMAP_HEIGHT;
            float uvDist = sqrtf(deltaU * deltaU + deltaV * deltaV);

            if (uvDist > 0.001f) {
                totalWorldDist += worldDist;
                totalUVDist += uvDist;
            }
        }
    }

    if (totalUVDist > 0.001f) {
        float ratio = totalWorldDist / totalUVDist;
        if (ratio < 0.1f) return 0.1f; // Sanity clamp
        if (ratio > 256.0f) return 256.0f;
        return ratio;
    }

    return (float)samplesize;
}

static void RunGpuTrisoupFilter(
    dsurface_t *ds,
    int         W, int H,
    pixelCache_t *pixCache,
    int         mappedPixels,
    float       radius,
    qboolean    isAA,
    const vec3_t gridMins,
    float       voxelSize,
    const int   gridDims[3],
    float       maxDistSq,
    float       twoSigmaSq,
    float      *tempFloats)
{
    int i, k;
    int numSamples = isAA ? SS_PATTERN8_COUNT : 1;
    int N = mappedPixels;

    ThreadLock();
    _printf(".");
    fflush(stdout);
    ThreadUnlock();

    float *texelPos    = malloc(N * 3 * sizeof(float));
    float *texelNormal = malloc(N * 3 * sizeof(float));
    float *texelColor  = malloc(N * 3 * sizeof(float));
    int   *validList   = malloc(N * sizeof(int));
    int   *tidX        = malloc(N * sizeof(int));
    int   *tidY        = malloc(N * sizeof(int));
    float *jitterPos    = malloc(N * numSamples * 3 * sizeof(float));
    float *jitterNormal = malloc(N * numSamples * 3 * sizeof(float));
    byte  *jitterValid  = malloc(N * numSamples * sizeof(byte));

    if (!texelPos || !texelNormal || !texelColor || !validList ||
        !tidX || !tidY || !jitterPos || !jitterNormal || !jitterValid) {
        free(texelPos); free(texelNormal); free(texelColor); free(validList);
        free(tidX); free(tidY); free(jitterPos); free(jitterNormal); free(jitterValid);
        return;
    }

    memset(jitterValid, 0, N * numSamples * sizeof(byte));

    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            pixelCache_t *pc = &pixCache[y * W + x];
            if (!pc->valid) continue;

            int p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
            if (lightAlphaMask && lightAlphaMask[p] != ALPHA_TRISOUP) continue;
            validList[n] = p;
            tidX[n] = x;
            tidY[n] = y;

            VectorCopy(pc->pos,    &texelPos[n*3]);
            VectorCopy(pc->normal, &texelNormal[n*3]);
            VectorCopy(&lightFloats[p*3], &texelColor[n*3]);

            // k=0 is always the center
            VectorCopy(pc->pos,    &jitterPos[n*numSamples*3]);
            VectorCopy(pc->normal, &jitterNormal[n*numSamples*3]);
            jitterValid[n*numSamples] = 1;
            n++;
        }
    }

    if (isAA && numSamples > 1) {
        #pragma omp parallel for schedule(dynamic, 64) private(k)
        for (int tid = 0; tid < N; tid++) {
            for (k = 1; k < numSamples; k++) {
                float st[2];
                st[0] = (float)ds->lightmapOffset[0][0] + (float)tidX[tid] + 0.5f + ssPattern8[k][0] * radius;
                st[1] = (float)ds->lightmapOffset[0][1] + (float)tidY[tid] + 0.5f + ssPattern8[k][1] * radius;
                int base = tid * numSamples + k;
                vec3_t jpos, jnorm;
                if (TriSoupSamplePoint(ds, st, jpos, jnorm)) {
                    VectorCopy(jpos,  &jitterPos[base*3]);
                    VectorCopy(jnorm, &jitterNormal[base*3]);
                    jitterValid[base] = 1;
                }
            }
        }
    }

    int numBuckets = gridDims[0] * gridDims[1] * gridDims[2];
    int gStride1 = gridDims[1] * gridDims[2];
    int gStride2 = gridDims[2];

    int *bucketCount  = calloc(numBuckets, sizeof(int));
    int *bucketStart  = malloc(numBuckets * sizeof(int));
    int *sortedTexels = malloc(N * sizeof(int));
    int *writePos     = malloc(numBuckets * sizeof(int));

    if (!bucketCount || !bucketStart || !sortedTexels || !writePos) {
        free(bucketCount); free(bucketStart); free(sortedTexels); free(writePos);
        goto cleanup;
    }

    for (int tid = 0; tid < N; tid++) {
        int vx = (int)((texelPos[tid*3+0] - gridMins[0]) / voxelSize);
        int vy = (int)((texelPos[tid*3+1] - gridMins[1]) / voxelSize);
        int vz = (int)((texelPos[tid*3+2] - gridMins[2]) / voxelSize);
        if (vx >= 0 && vx < gridDims[0] &&
            vy >= 0 && vy < gridDims[1] &&
            vz >= 0 && vz < gridDims[2]) {
            bucketCount[vx * gStride1 + vy * gStride2 + vz]++;
        }
    }

    bucketStart[0] = 0;
    for (i = 1; i < numBuckets; i++) {
        bucketStart[i] = bucketStart[i-1] + bucketCount[i-1];
    }
    memcpy(writePos, bucketStart, numBuckets * sizeof(int));

    for (int tid = 0; tid < N; tid++) {
        int vx = (int)((texelPos[tid*3+0] - gridMins[0]) / voxelSize);
        int vy = (int)((texelPos[tid*3+1] - gridMins[1]) / voxelSize);
        int vz = (int)((texelPos[tid*3+2] - gridMins[2]) / voxelSize);
        if (vx >= 0 && vx < gridDims[0] &&
            vy >= 0 && vy < gridDims[1] &&
            vz >= 0 && vz < gridDims[2]) {
            int bucket = vx * gStride1 + vy * gStride2 + vz;
            sortedTexels[writePos[bucket]++] = tid;
        }
    }
    free(writePos);

    {
        cl_int    err;
        cl_program prog   = BuildOpenCLProgram("trisoup_filter.cl", "");
        if (prog) {
            cl_kernel kernel = clCreateKernel(prog, "trisoup_filter", &err);
            if (err == CL_SUCCESS) {
                size_t atlasBytes    = (size_t)(numLightBytes / 3) * 3 * sizeof(float);
                size_t nf3           = (size_t)N * 3 * sizeof(float);
                size_t njf3          = (size_t)N * numSamples * 3 * sizeof(float);
                size_t njv           = (size_t)N * numSamples * sizeof(byte);
                size_t bucketBytes   = (size_t)numBuckets * sizeof(int);

                cl_mem bTexelPos     = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, nf3,  texelPos,    &err);
                cl_mem bTexelNormal  = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, nf3,  texelNormal, &err);
                cl_mem bJitterPos    = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, njf3, jitterPos,   &err);
                cl_mem bJitterNormal = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, njf3, jitterNormal,&err);
                cl_mem bJitterValid  = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, njv,  jitterValid, &err);
                cl_mem bTexelColor   = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, nf3,  texelColor,  &err);
                cl_mem bBucketStart  = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, bucketBytes, bucketStart,  &err);
                cl_mem bBucketCount  = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, bucketBytes, bucketCount,  &err);
                cl_mem bSortedTexels = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (size_t)N*sizeof(int), sortedTexels, &err);
                cl_mem bOutput       = clCreateBuffer(g_clContext, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, atlasBytes, lightFloats,  &err);
                cl_mem bValidList    = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (size_t)N*sizeof(int), validList, &err);

                int arg = 0;
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bTexelPos);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bTexelNormal);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bJitterPos);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bJitterNormal);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bJitterValid);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bTexelColor);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bBucketStart);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bBucketCount);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bSortedTexels);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bOutput);
                clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bValidList);
                clSetKernelArg(kernel, arg++, sizeof(float),  &gridMins[0]);
                clSetKernelArg(kernel, arg++, sizeof(float),  &gridMins[1]);
                clSetKernelArg(kernel, arg++, sizeof(float),  &gridMins[2]);
                clSetKernelArg(kernel, arg++, sizeof(float),  &voxelSize);
                clSetKernelArg(kernel, arg++, sizeof(int),    &gridDims[0]);
                clSetKernelArg(kernel, arg++, sizeof(int),    &gridDims[1]);
                clSetKernelArg(kernel, arg++, sizeof(int),    &gridDims[2]);
                clSetKernelArg(kernel, arg++, sizeof(float),  &maxDistSq);
                clSetKernelArg(kernel, arg++, sizeof(float),  &twoSigmaSq);
                float aa_match = AA_ANGLE_MATCH_COS;
                clSetKernelArg(kernel, arg++, sizeof(float),  &aa_match);
                clSetKernelArg(kernel, arg++, sizeof(int),    &numSamples);
                clSetKernelArg(kernel, arg++, sizeof(int),    &N);

                size_t globalSize = (size_t)N;
                err = clEnqueueNDRangeKernel(g_clQueue, kernel, 1, NULL, &globalSize, NULL, 0, NULL, NULL);
                if (err == CL_SUCCESS) {
                    clFinish(g_clQueue);
                    clEnqueueReadBuffer(g_clQueue, bOutput, CL_TRUE, 0, atlasBytes, lightFloats, 0, NULL, NULL);
                } else {
                    _printf("RunGpuTrisoupFilter: kernel enqueue failed (%d).\n", err);
                }

                clReleaseMemObject(bTexelPos);    clReleaseMemObject(bTexelNormal);
                clReleaseMemObject(bJitterPos);   clReleaseMemObject(bJitterNormal);
                clReleaseMemObject(bJitterValid); clReleaseMemObject(bTexelColor);
                clReleaseMemObject(bBucketStart); clReleaseMemObject(bBucketCount);
                clReleaseMemObject(bSortedTexels);clReleaseMemObject(bOutput);
                clReleaseMemObject(bValidList);
                clReleaseKernel(kernel);
            }
            clReleaseProgram(prog);
        }
    }

cleanup:
    free(bucketCount); free(bucketStart); free(sortedTexels);
    free(texelPos);    free(texelNormal); free(texelColor);
    free(validList);   free(tidX);        free(tidY);
    free(jitterPos);   free(jitterNormal);free(jitterValid);
}

static void ProcessTrisoupVolumetricGPU(int surfIdx, float radius, float *tempFloats, int aaPasses, int smoothPasses) {
    dsurface_t *ds = &drawSurfaces[surfIdx];
    if (ds->lightmapNum[0] < 0 || ds->surfaceType != MST_TRIANGLE_SOUP) return;
    if (aaPasses <= 0 && smoothPasses <= 0) return;

    float texelSize = GetSurfaceTexelSize(ds);
    float effectiveRadius = (smoothPasses > 0) ? TRISOUP_SMOOTH_CHEAT(radius) : radius;
    float searchRadius = effectiveRadius * texelSize;
    if (searchRadius < 0.1f) return;

    float voxelSize = searchRadius;
    float maxDistSq  = searchRadius * searchRadius;
    float sigma      = searchRadius / 3.0f;
    if (sigma < 0.1f) sigma = 0.1f;
    float twoSigmaSq = 2.0f * sigma * sigma;

    if (g_fast) {
        // --- Optimized Path (HEAD) ---
        int numPoints = 0;
        voxelPoint_t *cachedPoints = VoxelCache_Load(surfIdx, &numPoints);
        if (!cachedPoints || numPoints == 0) { if (cachedPoints) free(cachedPoints); return; }

        int N = numPoints;
        int numSamples = (aaPasses > 0) ? SS_PATTERN8_COUNT : 1;

        ThreadLock();
        _printf(".");
        fflush(stdout);
        ThreadUnlock();

        float *texelPos    = malloc(N * 3 * sizeof(float));
        float *texelNormal = malloc(N * 3 * sizeof(float));
        float *texelColor  = malloc(N * 3 * sizeof(float));
        int   *validList   = malloc(N * sizeof(int));
        int   *tidX        = malloc(N * sizeof(int));
        int   *tidY        = malloc(N * sizeof(int));
        float *jitterPos    = malloc(N * numSamples * 3 * sizeof(float));
        float *jitterNormal = malloc(N * numSamples * 3 * sizeof(float));
        byte  *jitterValid  = malloc(N * numSamples * sizeof(byte));

        if (!texelPos || !texelNormal || !texelColor || !validList || !tidX || !tidY || !jitterPos || !jitterNormal || !jitterValid) {
            free(texelPos); free(texelNormal); free(texelColor); free(validList); free(tidX); free(tidY);
            free(jitterPos); free(jitterNormal); free(jitterValid); free(cachedPoints); return;
        }

        memset(jitterValid, 0, N * numSamples * sizeof(byte));
        vec3_t gridMins = {99999.0f, 99999.0f, 99999.0f};
        vec3_t gridMaxs = {-99999.0f, -99999.0f, -99999.0f};

        for (int i = 0; i < N; i++) {
            int p = cachedPoints[i].pixelIndex;

            validList[i] = p;
            int lmLocal = p % (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT);
            tidX[i] = lmLocal % LIGHTMAP_WIDTH - ds->lightmapOffset[0][0];
            tidY[i] = lmLocal / LIGHTMAP_WIDTH - ds->lightmapOffset[0][1];
            VectorCopy(cachedPoints[i].pos, &texelPos[i*3]);
            VectorCopy(cachedPoints[i].normal, &texelNormal[i*3]);
            for (int k = 0; k < 3; k++) {
                if (texelPos[i*3+k] < gridMins[k]) gridMins[k] = texelPos[i*3+k];
                if (texelPos[i*3+k] > gridMaxs[k]) gridMaxs[k] = texelPos[i*3+k];
            }
            VectorCopy(cachedPoints[i].pos, &jitterPos[i*numSamples*3]);
            VectorCopy(cachedPoints[i].normal, &jitterNormal[i*numSamples*3]);
            jitterValid[i*numSamples] = 1;
        }

        if (aaPasses > 0) {
            #pragma omp parallel for schedule(dynamic, 64)
            for (int i = 0; i < N; i++) {
                for (int k = 1; k < numSamples; k++) {
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)tidX[i] + 0.5f + ssPattern8[k][0] * radius;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)tidY[i] + 0.5f + ssPattern8[k][1] * radius;
                    int base = i * numSamples + k;
                    vec3_t jpos, jnorm;
                    if (TriSoupSamplePoint(ds, st, jpos, jnorm)) {
                        VectorCopy(jpos, &jitterPos[base*3]); VectorCopy(jnorm, &jitterNormal[base*3]); jitterValid[base] = 1;
                    }
                }
            }
        }

        for (int k = 0; k < 3; k++) { gridMins[k] -= voxelSize; gridMaxs[k] += voxelSize; }
        int gridDims[3];
        for (int k = 0; k < 3; k++) {
            gridDims[k] = (int)ceilf((gridMaxs[k] - gridMins[k]) / voxelSize);
            if (gridDims[k] < 1) gridDims[k] = 1;
        }

        int numBuckets = gridDims[0] * gridDims[1] * gridDims[2];
        int gStride1 = gridDims[1] * gridDims[2], gStride2 = gridDims[2];
        int *bucketCount = calloc(numBuckets, sizeof(int));
        int *bucketStart = malloc(numBuckets * sizeof(int));
        int *sortedTexels = malloc(N * sizeof(int));
        int *writePos = malloc(numBuckets * sizeof(int));

        for (int i = 0; i < N; i++) {
            int v[3];
            for (int k = 0; k < 3; k++) v[k] = (int)((texelPos[i*3+k] - gridMins[k]) / voxelSize);
            if (v[0] >= 0 && v[0] < gridDims[0] && v[1] >= 0 && v[1] < gridDims[1] && v[2] >= 0 && v[2] < gridDims[2])
                bucketCount[v[0] * gStride1 + v[1] * gStride2 + v[2]]++;
        }
        bucketStart[0] = 0;
        for (int i = 1; i < numBuckets; i++) bucketStart[i] = bucketStart[i-1] + bucketCount[i-1];
        memcpy(writePos, bucketStart, numBuckets * sizeof(int));
        for (int i = 0; i < N; i++) {
            int v[3];
            for (int k = 0; k < 3; k++) v[k] = (int)((texelPos[i*3+k] - gridMins[k]) / voxelSize);
            if (v[0] >= 0 && v[0] < gridDims[0] && v[1] >= 0 && v[1] < gridDims[1] && v[2] >= 0 && v[2] < gridDims[2])
                sortedTexels[writePos[v[0] * gStride1 + v[1] * gStride2 + v[2]]++] = i;
        }

        cl_int err;
        cl_program prog = BuildOpenCLProgram("trisoup_filter.cl", "");
        if (prog) {
            cl_kernel kernel = clCreateKernel(prog, "trisoup_filter", &err);
            if (err == CL_SUCCESS) {
                size_t atlasBytes = (size_t)(numLightBytes / 3) * 3 * sizeof(float);
                size_t nf3 = (size_t)N * 3 * sizeof(float);
                size_t njf3 = (size_t)N * numSamples * 3 * sizeof(float);
                size_t njv = (size_t)N * numSamples * sizeof(byte);
                size_t bucketBytes = (size_t)numBuckets * sizeof(int);

                cl_mem bTexelPos = NULL, bTexelNormal = NULL, bJitterPos = NULL, bJitterNormal = NULL, bJitterValid = NULL;
                cl_mem bBucketStart = NULL, bBucketCount = NULL, bSortedTexels = NULL, bOutput = NULL, bValidList = NULL;
                cl_mem bTexelColor = NULL;

                err = CL_SUCCESS;
                bTexelPos = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, nf3, texelPos, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bTexelPos (%d)\n", err); goto cleanup_gpu_trisoup; }
                bTexelNormal = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, nf3, texelNormal, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bTexelNormal (%d)\n", err); goto cleanup_gpu_trisoup; }
                bJitterPos = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, njf3, jitterPos, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bJitterPos (%d)\n", err); goto cleanup_gpu_trisoup; }
                bJitterNormal = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, njf3, jitterNormal, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bJitterNormal (%d)\n", err); goto cleanup_gpu_trisoup; }
                bJitterValid = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, njv, jitterValid, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bJitterValid (%d)\n", err); goto cleanup_gpu_trisoup; }
                bBucketStart = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, bucketBytes, bucketStart, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bBucketStart (%d)\n", err); goto cleanup_gpu_trisoup; }
                bBucketCount = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, bucketBytes, bucketCount, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bBucketCount (%d)\n", err); goto cleanup_gpu_trisoup; }
                bSortedTexels = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (size_t)N*sizeof(int), sortedTexels, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bSortedTexels (%d)\n", err); goto cleanup_gpu_trisoup; }
                bOutput = clCreateBuffer(g_clContext, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR, atlasBytes, lightFloats, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bOutput (%d)\n", err); goto cleanup_gpu_trisoup; }
                bValidList = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (size_t)N*sizeof(int), validList, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bValidList (%d)\n", err); goto cleanup_gpu_trisoup; }
                bTexelColor = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY, nf3, NULL, &err);
                if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate bTexelColor (%d)\n", err); goto cleanup_gpu_trisoup; }

                int totalPasses = aaPasses + smoothPasses;
                for (int currentPass = 0; currentPass < totalPasses; currentPass++) {
                    int samples = (currentPass < aaPasses) ? SS_PATTERN8_COUNT : 1;
                    for (int i = 0; i < N; i++) VectorCopy(&lightFloats[validList[i] * 3], &texelColor[i * 3]);
                    clEnqueueWriteBuffer(g_clQueue, bTexelColor, CL_TRUE, 0, nf3, texelColor, 0, NULL, NULL);
                    int arg = 0;
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bTexelPos);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bTexelNormal);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bJitterPos);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bJitterNormal);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bJitterValid);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bTexelColor);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bBucketStart);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bBucketCount);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bSortedTexels);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bOutput);
                    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bValidList);
                    clSetKernelArg(kernel, arg++, sizeof(float), &gridMins[0]); clSetKernelArg(kernel, arg++, sizeof(float), &gridMins[1]); clSetKernelArg(kernel, arg++, sizeof(float), &gridMins[2]);
                    clSetKernelArg(kernel, arg++, sizeof(float), &voxelSize);
                    clSetKernelArg(kernel, arg++, sizeof(int), &gridDims[0]); clSetKernelArg(kernel, arg++, sizeof(int), &gridDims[1]); clSetKernelArg(kernel, arg++, sizeof(int), &gridDims[2]);
                    clSetKernelArg(kernel, arg++, sizeof(float), &maxDistSq); clSetKernelArg(kernel, arg++, sizeof(float), &twoSigmaSq);
                    float aa_match = AA_ANGLE_MATCH_COS; clSetKernelArg(kernel, arg++, sizeof(float), &aa_match);
                    clSetKernelArg(kernel, arg++, sizeof(int), &samples); clSetKernelArg(kernel, arg++, sizeof(int), &N);
                    size_t globalSize = (size_t)N; clEnqueueNDRangeKernel(g_clQueue, kernel, 1, NULL, &globalSize, NULL, 0, NULL, NULL); clFinish(g_clQueue);
                    if (currentPass < totalPasses - 1) {
                        clEnqueueReadBuffer(g_clQueue, bOutput, CL_TRUE, 0, atlasBytes, lightFloats, 0, NULL, NULL);
                    }
                }
                clEnqueueReadBuffer(g_clQueue, bOutput, CL_TRUE, 0, atlasBytes, lightFloats, 0, NULL, NULL);
                
            cleanup_gpu_trisoup:
                if (bTexelPos) { clReleaseMemObject(bTexelPos); } if (bTexelNormal) { clReleaseMemObject(bTexelNormal); }
                if (bJitterPos) { clReleaseMemObject(bJitterPos); } if (bJitterNormal) { clReleaseMemObject(bJitterNormal); }
                if (bJitterValid) { clReleaseMemObject(bJitterValid); }
                if (bTexelColor) { clReleaseMemObject(bTexelColor); } if (bBucketStart) { clReleaseMemObject(bBucketStart); }
                if (bBucketCount) { clReleaseMemObject(bBucketCount); } if (bSortedTexels) { clReleaseMemObject(bSortedTexels); }
                if (bOutput) { clReleaseMemObject(bOutput); } if (bValidList) { clReleaseMemObject(bValidList); }
                clReleaseKernel(kernel);
            }
            clReleaseProgram(prog);
        }
        free(texelPos); free(texelNormal); free(texelColor); free(validList); free(tidX); free(tidY); free(jitterPos); free(jitterNormal); free(jitterValid);
        free(bucketCount); free(bucketStart); free(sortedTexels); free(writePos); free(cachedPoints);
    } else {
        // --- High-Fidelity Path (Legacy) ---
        int numPoints = 0;
        voxelPoint_t *cachedPoints = VoxelCache_Load(surfIdx, &numPoints);
        if (!cachedPoints || numPoints == 0) { if (cachedPoints) free(cachedPoints); return; }

        int W = ds->lightmapWidth, H = ds->lightmapHeight;
        pixelCache_t *pixCache = calloc(W * H, sizeof(pixelCache_t));
        for (int i = 0; i < numPoints; i++) {
            int p = cachedPoints[i].pixelIndex;
            int lmLocal = p % (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT);
            int lx = lmLocal % LIGHTMAP_WIDTH - ds->lightmapOffset[0][0];
            int ly = lmLocal / LIGHTMAP_WIDTH - ds->lightmapOffset[0][1];
            if (lx >= 0 && lx < W && ly >= 0 && ly < H) {
                pixelCache_t *pc = &pixCache[ly * W + lx];
                VectorCopy(cachedPoints[i].pos, pc->pos); VectorCopy(cachedPoints[i].normal, pc->normal); pc->valid = qtrue;
            }
        }
        vec3_t gridMins = {99999.0f, 99999.0f, 99999.0f}, gridMaxs = {-99999.0f, -99999.0f, -99999.0f};
        for (int i = 0; i < numPoints; i++) {
            for (int k = 0; k < 3; k++) {
                if (cachedPoints[i].pos[k] < gridMins[k]) gridMins[k] = cachedPoints[i].pos[k];
                if (cachedPoints[i].pos[k] > gridMaxs[k]) gridMaxs[k] = cachedPoints[i].pos[k];
            }
        }
        for (int i = 0; i < 3; i++) { gridMins[i] -= voxelSize; gridMaxs[i] += voxelSize; }
        int gridDims[3]; for (int i = 0; i < 3; i++) {
            gridDims[i] = (int)ceilf((gridMaxs[i] - gridMins[i]) / voxelSize); if (gridDims[i] < 1) gridDims[i] = 1;
        }
        if (aaPasses > 0) { for (int pnum = 0; pnum < aaPasses; pnum++) RunGpuTrisoupFilter(ds, W, H, pixCache, numPoints, radius, qtrue, gridMins, voxelSize, gridDims, maxDistSq, twoSigmaSq, tempFloats); }
        if (smoothPasses > 0) { for (int pnum = 0; pnum < smoothPasses; pnum++) RunGpuTrisoupFilter(ds, W, H, pixCache, numPoints, radius, qfalse, gridMins, voxelSize, gridDims, maxDistSq, twoSigmaSq, tempFloats); }
        free(pixCache); free(cachedPoints);
    }
}

static void ProcessTrisoupVolumetricCPU(int surfIdx, float radius, float *tempFloats, int aaPasses, int smoothPasses) {
    dsurface_t *ds = &drawSurfaces[surfIdx];
    if (ds->lightmapNum[0] < 0 || ds->surfaceType != MST_TRIANGLE_SOUP) return;
    if (aaPasses <= 0 && smoothPasses <= 0) return;

    float texelSize = GetSurfaceTexelSize(ds);
    float effectiveRadius = (smoothPasses > 0) ? TRISOUP_SMOOTH_CHEAT(radius) : radius;
    float searchRadius = effectiveRadius * texelSize;
    if (searchRadius < 0.1f) return;

    float voxelSize = searchRadius;
    float maxDistSq  = searchRadius * searchRadius;
    float sigma      = searchRadius / 3.0f;
    if (sigma < 0.1f) sigma = 0.1f;
    float twoSigmaSq = 2.0f * sigma * sigma;

    int i, k, p;

    if (g_fast) {
        // --- Optimized Path (HEAD) ---
        int numPoints = 0;
        voxelPoint_t *cachedPoints = VoxelCache_Load(surfIdx, &numPoints);
        if (!cachedPoints || numPoints == 0) { if (cachedPoints) free(cachedPoints); return; }

        vec3_t gridMins = {99999.0f, 99999.0f, 99999.0f}, gridMaxs = {-99999.0f, -99999.0f, -99999.0f};
        for (i = 0; i < numPoints; i++) {
            for (k = 0; k < 3; k++) {
                if (cachedPoints[i].pos[k] < gridMins[k]) gridMins[k] = cachedPoints[i].pos[k];
                if (cachedPoints[i].pos[k] > gridMaxs[k]) gridMaxs[k] = cachedPoints[i].pos[k];
            }
        }
        for (i = 0; i < 3; i++) { gridMins[i] -= voxelSize; gridMaxs[i] += voxelSize; }
        int gridDims[3];
        for (i = 0; i < 3; i++) {
            gridDims[i] = (int)ceilf((gridMaxs[i] - gridMins[i]) / voxelSize);
            if (gridDims[i] < 1) gridDims[i] = 1;
        }

        const size_t gStride1 = (size_t)gridDims[1] * gridDims[2], gStride2 = (size_t)gridDims[2], numBuckets = (size_t)gridDims[0] * gridDims[1] * gridDims[2];
        aaTexel_t **flatGrid = (aaTexel_t **)calloc(numBuckets, sizeof(aaTexel_t *));
        aaTexel_t *pool = (aaTexel_t *)malloc(numPoints * sizeof(aaTexel_t));
        if (!flatGrid || !pool) { if (flatGrid) free(flatGrid); if (pool) free(pool); free(cachedPoints); return; }

        for (i = 0; i < numPoints; i++) {
            int v[3]; qboolean gridInBounds = qtrue;
            for (k = 0; k < 3; k++) {
                v[k] = (int)((cachedPoints[i].pos[k] - gridMins[k]) / voxelSize);
                if (v[k] < 0 || v[k] >= gridDims[k]) { gridInBounds = qfalse; break; }
            }
            if (gridInBounds) {
                aaTexel_t *newT = &pool[i]; VectorCopy(cachedPoints[i].pos, newT->pos); VectorCopy(cachedPoints[i].normal, newT->normal);
                p = cachedPoints[i].pixelIndex;
                if (lightAlphaMask && lightAlphaMask[p] != ALPHA_TRISOUP) continue;
                VectorCopy(&tempFloats[p * 3], newT->color);
                size_t cell = (size_t)v[0] * gStride1 + (size_t)v[1] * gStride2 + (size_t)v[2];
                newT->next = flatGrid[cell]; flatGrid[cell] = newT;
            }
        }

        vec3_t *aaJittersPos = NULL; vec3_t *aaJittersNormal = NULL; byte *aaJittersValid = NULL;
        if (aaPasses > 0 && numPoints > 0) {
            aaJittersPos = malloc((size_t)numPoints * SS_PATTERN8_COUNT * sizeof(vec3_t));
            aaJittersNormal = malloc((size_t)numPoints * SS_PATTERN8_COUNT * sizeof(vec3_t));
            aaJittersValid = malloc((size_t)numPoints * SS_PATTERN8_COUNT * sizeof(byte));
            if (aaJittersPos && aaJittersNormal && aaJittersValid) {
                for (i = 0; i < numPoints; i++) {
                    p = cachedPoints[i].pixelIndex; int lmLocal = p % (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT), lx = lmLocal % LIGHTMAP_WIDTH, ly = lmLocal / LIGHTMAP_WIDTH;
                    int x = lx - ds->lightmapOffset[0][0], y = ly - ds->lightmapOffset[0][1];
                    for (k = 0; k < SS_PATTERN8_COUNT; k++) {
                        size_t idx = (size_t)i * SS_PATTERN8_COUNT + k;
                        if (k == 0) { VectorCopy(cachedPoints[i].pos, aaJittersPos[idx]); VectorCopy(cachedPoints[i].normal, aaJittersNormal[idx]); aaJittersValid[idx] = 1; }
                        else {
                            float st[2]; st[0] = (float)ds->lightmapOffset[0][0] + (float)x + 0.5f + ssPattern8[k][0] * radius;
                            st[1] = (float)ds->lightmapOffset[0][1] + (float)y + 0.5f + ssPattern8[k][1] * radius;
                            if (TriSoupSamplePoint(ds, st, aaJittersPos[idx], aaJittersNormal[idx])) aaJittersValid[idx] = 1; else aaJittersValid[idx] = 0;
                        }
                    }
                }
            }
        }

        int totalPasses = aaPasses + smoothPasses;
        for (int currentPass = 0; currentPass < totalPasses; currentPass++) {
            qboolean isAA = (currentPass < aaPasses);
            for (int pIdx = 0; pIdx < numPoints; pIdx++) {
                p = cachedPoints[pIdx].pixelIndex;
                if (lightAlphaMask && lightAlphaMask[p] != ALPHA_TRISOUP) continue;
                const int numSamples = isAA ? SS_PATTERN8_COUNT : 1;
                vec3_t finalColor = {0,0,0}; float finalWeight = 0;
                for (k = 0; k < numSamples; k++) {
                    vec3_t origin, normal;
                    if (isAA && aaJittersPos) {
                        size_t idx = (size_t)pIdx * SS_PATTERN8_COUNT + k; if (!aaJittersValid[idx]) continue;
                        VectorCopy(aaJittersPos[idx], origin); VectorCopy(aaJittersNormal[idx], normal);
                    } else { VectorCopy(cachedPoints[pIdx].pos, origin); VectorCopy(cachedPoints[pIdx].normal, normal); }
                    int v[3]; for (int i0 = 0; i0 < 3; i0++) v[i0] = (int)((origin[i0] - gridMins[i0]) / voxelSize);
                    vec3_t totalColor = {0,0,0}; float totalWeight = 0;
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = v[0] + dx; if (nx < 0 || nx >= gridDims[0]) continue;
                        for (int dy = -1; dy <= 1; dy++) {
                            int ny = v[1] + dy; if (ny < 0 || ny >= gridDims[1]) continue;
                            for (int dz = -1; dz <= 1; dz++) {
                                int nz = v[2] + dz; if (nz < 0 || nz >= gridDims[2]) continue;
                                size_t cell = (size_t)nx * gStride1 + (size_t)ny * gStride2 + (size_t)nz;
                                aaTexel_t *curr = flatGrid[cell];
                                while (curr) {
                                    vec3_t delta; VectorSubtract(origin, curr->pos, delta);
                                    float distSq = DotProduct(delta, delta); if (distSq >= maxDistSq) { curr = curr->next; continue; }
                                    float dot = DotProduct(normal, curr->normal);
                                    if (dot > AA_ANGLE_MATCH_COS) {
                                        float w = expf(-distSq / twoSigmaSq) * dot; VectorMA(totalColor, w, curr->color, totalColor); totalWeight += w;
                                    }
                                    curr = curr->next;
                                }
                            }
                        }
                    }
                    if (totalWeight > 0.0001f) { VectorMA(finalColor, 1.0f / totalWeight, totalColor, finalColor); finalWeight += 1.0f; }
                }
                if (finalWeight > 0.0001f) VectorScale(finalColor, 1.0f / finalWeight, &lightFloats[p * 3]);
            }
            if (currentPass < totalPasses - 1) { for (i = 0; i < numPoints; i++) { p = cachedPoints[i].pixelIndex; VectorCopy(&lightFloats[p * 3], pool[i].color); } }
        }
        if (aaJittersPos) free(aaJittersPos);
        if (aaJittersNormal) free(aaJittersNormal);
        if (aaJittersValid) free(aaJittersValid);
        free(pool); free(flatGrid); free(cachedPoints);
    } else {
        // --- High-Fidelity Path (Legacy) ---
        int numPoints = 0;
        voxelPoint_t *cachedPoints = VoxelCache_Load(surfIdx, &numPoints);
        if (!cachedPoints || numPoints == 0) { if (cachedPoints) free(cachedPoints); return; }

        int W = ds->lightmapWidth, H = ds->lightmapHeight;
        int i, k, p, x, y;
        pixelCache_t *pixCache = malloc(W * H * sizeof(pixelCache_t));
        memset(pixCache, 0, W * H * sizeof(pixelCache_t));

        for (i = 0; i < numPoints; i++) {
            int pIdx = cachedPoints[i].pixelIndex;
            int lmLocal = pIdx % (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT);
            int lx = lmLocal % LIGHTMAP_WIDTH - ds->lightmapOffset[0][0];
            int ly = lmLocal / LIGHTMAP_WIDTH - ds->lightmapOffset[0][1];
            if (lx >= 0 && lx < W && ly >= 0 && ly < H) {
                pixelCache_t *pc = &pixCache[ly * W + lx];
                VectorCopy(cachedPoints[i].pos, pc->pos); VectorCopy(cachedPoints[i].normal, pc->normal); pc->valid = qtrue;
            }
        }

        vec3_t gridMins = {99999, 99999, 99999}, gridMaxs = {-99999, -99999, -99999};
        for (i = 0; i < numPoints; i++) {
            for (k = 0; k < 3; k++) {
                if (cachedPoints[i].pos[k] < gridMins[k]) gridMins[k] = cachedPoints[i].pos[k];
                if (cachedPoints[i].pos[k] > gridMaxs[k]) gridMaxs[k] = cachedPoints[i].pos[k];
            }
        }
        for (i = 0; i < 3; i++) { gridMins[i] -= voxelSize; gridMaxs[i] += voxelSize; }
        int gridDims[3]; for (i = 0; i < 3; i++) { gridDims[i] = (int)ceilf((gridMaxs[i] - gridMins[i]) / voxelSize); if (gridDims[i] < 1) gridDims[i] = 1; }

        const size_t gStride1 = (size_t)gridDims[1] * gridDims[2], gStride2 = (size_t)gridDims[2], numBuckets = (size_t)gridDims[0] * gridDims[1] * gridDims[2];
        aaTexel_t **flatGrid = calloc(numBuckets, sizeof(aaTexel_t *));
        aaTexel_t *pool = malloc(numPoints * sizeof(aaTexel_t));

        int totalPasses = aaPasses + smoothPasses;
        for (int currentPass = 0; currentPass < totalPasses; currentPass++) {
            qboolean isAA = (currentPass < aaPasses);
            memset(flatGrid, 0, numBuckets * sizeof(aaTexel_t *));
            for (i = 0; i < numPoints; i++) {
                int v[3]; qboolean gridInBounds = qtrue;
                for (k = 0; k < 3; k++) {
                    v[k] = (int)((cachedPoints[i].pos[k] - gridMins[k]) / voxelSize);
                    if (v[k] < 0 || v[k] >= gridDims[k]) { gridInBounds = qfalse; break; }
                }
                if (gridInBounds) {
                    aaTexel_t *newT = &pool[i]; VectorCopy(cachedPoints[i].pos, newT->pos); VectorCopy(cachedPoints[i].normal, newT->normal);
                    p = cachedPoints[i].pixelIndex; 
                    if (lightAlphaMask && lightAlphaMask[p] != ALPHA_TRISOUP) continue;
                    VectorCopy(&tempFloats[p * 3], newT->color);
                    size_t cell = (size_t)v[0] * gStride1 + (size_t)v[1] * gStride2 + (size_t)v[2];
                    newT->next = flatGrid[cell]; flatGrid[cell] = newT;
                }
            }

            for (y = 0; y < H; y++) {
                for (x = 0; x < W; x++) {
                    int pixIdx = y * W + x; if (!pixCache[pixIdx].valid) continue;
                    p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
                    if (lightAlphaMask && lightAlphaMask[p] != ALPHA_TRISOUP) continue;
                    const int numSamples = isAA ? SS_PATTERN8_COUNT : 1;
                    vec3_t finalColor = {0,0,0}; float finalWeight = 0;
                    for (k = 0; k < numSamples; k++) {
                        vec3_t origin, normal;
                        if (isAA) {
                            float st[2]; st[0] = (float)ds->lightmapOffset[0][0] + (float)x + 0.5f + ssPattern8[k][0] * radius;
                            st[1] = (float)ds->lightmapOffset[0][1] + (float)y + 0.5f + ssPattern8[k][1] * radius;
                            if (!TriSoupSamplePoint(ds, st, origin, normal)) continue;
                        } else { VectorCopy(pixCache[pixIdx].pos, origin); VectorCopy(pixCache[pixIdx].normal, normal); }
                        int v[3]; for (int i0 = 0; i0 < 3; i0++) v[i0] = (int)((origin[i0] - gridMins[i0]) / voxelSize);
                        vec3_t totalColor = {0,0,0}; float totalWeight = 0;
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = v[0] + dx; if (nx < 0 || nx >= gridDims[0]) continue;
                            for (int dy = -1; dy <= 1; dy++) {
                                int ny = v[1] + dy; if (ny < 0 || ny >= gridDims[1]) continue;
                                for (int dz = -1; dz <= 1; dz++) {
                                    int nz = v[2] + dz; if (nz < 0 || nz >= gridDims[2]) continue;
                                    size_t cell = (size_t)nx * gStride1 + (size_t)ny * gStride2 + (size_t)nz;
                                    aaTexel_t *curr = flatGrid[cell];
                                    while (curr) {
                                        vec3_t delta; VectorSubtract(origin, curr->pos, delta);
                                        float distSq = DotProduct(delta, delta); if (distSq >= maxDistSq) { curr = curr->next; continue; }
                                        float dot = DotProduct(normal, curr->normal);
                                        if (dot > AA_ANGLE_MATCH_COS) {
                                            float w = expf(-distSq / twoSigmaSq) * dot; VectorMA(totalColor, w, curr->color, totalColor); totalWeight += w;
                                        }
                                        curr = curr->next;
                                    }
                                }
                            }
                        }
                        if (totalWeight > 0.0001f) { VectorMA(finalColor, 1.0f / totalWeight, totalColor, finalColor); finalWeight += 1.0f; }
                    }
                    if (finalWeight > 0.0001f) VectorScale(finalColor, 1.0f / finalWeight, &lightFloats[p * 3]);
                }
            }
            if (currentPass < totalPasses - 1) {
                for (i = 0; i < numPoints; i++) {
                    p = cachedPoints[i].pixelIndex;
                    VectorCopy(&lightFloats[p * 3], &tempFloats[p * 3]);
                }
            }
        }
        free(pool); free(flatGrid); free(pixCache); free(cachedPoints);
    }
}

/*
================
GpuLightmapState_Upload

Builds and uploads all persistent GPU metadata needed by every
post-processing filter kernel:
  - Ping-pong atlas buffers (atlasA and atlasB both seeded with lightFloats)
  - Alpha mask
  - GpuPlanarSurface array (flattened from planarSurfaces[])
  - Partner adjacency in CSR layout (partnerData + partnerOffsets)
  - Per-pixel lookup tables: validList, pixelToSurface, pixelToX, pixelToY

Called once from PostProcessLightmaps, before any kernel dispatch.
================
*/
void GpuLightmapState_Upload(void) {
    int s, x, y;
    GpuLightmapState *st = &g_gpuLM;
    cl_int err;

    int scale = FILTER_UPSCALE ? 2 : 1;
    st->upscale = scale;

    int totalPixels1x = numLightBytes / 3;
    int totalPixels   = totalPixels1x * scale * scale;
    st->totalAtlasPixels = totalPixels;
    st->numPlanarSurfaces = numPlanarSurfaces;
    st->pingIsA           = 1;

    size_t atlasBytes = (size_t)totalPixels * 3 * sizeof(float);
    size_t maskBytes  = (size_t)totalPixels * sizeof(byte);

    float *tempAtlas = NULL;
    byte  *tempMask  = NULL;

    if (scale == 1) {
        tempAtlas = lightFloats;
        tempMask  = lightAlphaMask;
    } else {
        /* Bilinear upscale 1x -> 2x */
        if (verbose) _printf("  Upscaling atlas to 2x for high-fidelity filtering...\n");
        tempAtlas = malloc(atlasBytes);
        tempMask  = malloc(maskBytes);
        if (!tempAtlas || !tempMask) { Error("Out of memory for GPU upscale"); }

        int numLms = totalPixels1x / (LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT);
        int W = LIGHTMAP_WIDTH, H = LIGHTMAP_HEIGHT;
        int Ws = W * scale, Hs = H * scale;

        #pragma omp parallel for schedule(static)
        for (int m = 0; m < numLms; m++) {
            for (int ys = 0; ys < Hs; ys++) {
                for (int xs = 0; xs < Ws; xs++) {
                    int ps = (m * Hs + ys) * Ws + xs;
                    
                    /* Weighted bilinear sample from 1x */
                    float fx = ((float)xs + 0.5f) / (float)scale - 0.5f;
                    float fy = ((float)ys + 0.5f) / (float)scale - 0.5f;
                    int ix0 = (int)floorf(fx), iy0 = (int)floorf(fy);
                    float tx = fx - (float)ix0, ty = fy - (float)iy0;
                    
                    int ix1 = ix0 + 1, iy1 = iy0 + 1;
                    if (ix0 < 0) ix0 = 0;
                    if (ix1 >= W) ix1 = W - 1;
                    if (iy0 < 0) iy0 = 0;
                    if (iy1 >= H) iy1 = H - 1;

                    int i00 = (m * H + iy0) * W + ix0, i10 = (m * H + iy0) * W + ix1;                    int i01 = (m * H + iy1) * W + ix0, i11 = (m * H + iy1) * W + ix1;

                    float w00 = (1.0f - tx) * (1.0f - ty);
                    float w10 = tx * (1.0f - ty);
                    float w01 = (1.0f - tx) * ty;
                    float w11 = tx * ty;

                    if (lightAlphaMask[i00] == 0) w00 = 0.0f;
                    if (lightAlphaMask[i10] == 0) w10 = 0.0f;
                    if (lightAlphaMask[i01] == 0) w01 = 0.0f;
                    if (lightAlphaMask[i11] == 0) w11 = 0.0f;

                    float *dst = &tempAtlas[ps * 3];
                    float sumW = w00 + w10 + w01 + w11;
                    if (sumW > 0.01f) {
                        float invW = 1.0f / sumW;
                        dst[0] = (w00 * lightFloats[i00*3+0] + w10 * lightFloats[i10*3+0] + w01 * lightFloats[i01*3+0] + w11 * lightFloats[i11*3+0]) * invW;
                        dst[1] = (w00 * lightFloats[i00*3+1] + w10 * lightFloats[i10*3+1] + w01 * lightFloats[i01*3+1] + w11 * lightFloats[i11*3+1]) * invW;
                        dst[2] = (w00 * lightFloats[i00*3+2] + w10 * lightFloats[i10*3+2] + w01 * lightFloats[i01*3+2] + w11 * lightFloats[i11*3+2]) * invW;
                    } else {
                        /* Fallback to nearest valid neighbor */
                        int in = (lightAlphaMask[i00] != 0) ? i00 : (lightAlphaMask[i10] != 0 ? i10 : (lightAlphaMask[i01] != 0 ? i01 : i11));
                        VectorCopy(&lightFloats[in*3], dst);
                    }

                    /* Initial mask replication */
                    tempMask[ps] = lightAlphaMask[(m * H + (ys/scale)) * W + (xs/scale)];
                }
            }
        }

        /* High-fidelity refinement pass: re-calculate all indexed pixels using seam logic */
        if (verbose) _printf("  Refining %dx surface edges with adjacency logic...\n", scale);
        #pragma omp parallel for schedule(dynamic, 1)
        for (int sidx = 0; sidx < numPlanarSurfaces; sidx++) {
            planarInfo_t *p = &planarSurfaces[sidx];
            dsurface_t *ds = &drawSurfaces[p->surfaceNum];
            int sWs = ds->lightmapWidth * scale, sHs = ds->lightmapHeight * scale;
            int offXs = ds->lightmapOffset[0][0] * scale, offYs = ds->lightmapOffset[0][1] * scale;
            int lm = ds->lightmapNum[0];

            for (int y_s = 0; y_s < sHs; y_s++) {
                for (int x_s = 0; x_s < sWs; x_s++) {
                    /* Convert scaled index to native coordinate (e.g. 0.25, 0.75 for 2x) */
                    float px = ((float)x_s + 0.5f) / (float)scale;
                    float py = ((float)y_s + 0.5f) / (float)scale;
                    float col[3];
                    int pa = (lm * Hs + offYs + y_s) * Ws + offXs + x_s;
                    if (GetFilteredTexel(sidx, px, py, col, lightFloats)) {
                        VectorCopy(col, &tempAtlas[pa * 3]);
                        tempMask[pa] = ALPHA_SURF_WORLD; 
                    } else {
                        tempMask[pa] = 0; // Strictly mask out pixels rejected by CPU logic
                    }
                }
            }
        }
    }

    st->atlasA  = clCreateBuffer(g_clContext, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                  atlasBytes, tempAtlas, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU atlasA (%d)\n", err); useOpenCL = qfalse; goto cleanup_upload; }
    st->atlasB  = clCreateBuffer(g_clContext, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                  atlasBytes, tempAtlas, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU atlasB (%d)\n", err); useOpenCL = qfalse; goto cleanup_upload; }
    st->maskBuf = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  maskBytes, tempMask, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU maskBuf (%d)\n", err); useOpenCL = qfalse; goto cleanup_upload; }
    
    if (scale > 1) { free(tempAtlas); tempAtlas = NULL; free(tempMask); tempMask = NULL; }

    /* --- Flatten GpuPlanarSurface[] (SCALED for GPU-oblivious path) --- */
    GpuPlanarSurface_t *cpuSurfaces = malloc(numPlanarSurfaces * sizeof(GpuPlanarSurface_t));
    if (!cpuSurfaces) { _printf("ERROR: Out of memory for cpuSurfaces\n"); useOpenCL = qfalse; goto cleanup_upload; }
    int totalLinks = 0;
    float fScale = (float)scale;
    float shift  = (0.5f / fScale) - 0.5f;

    for (s = 0; s < numPlanarSurfaces; s++) {
        planarInfo_t      *p = &planarSurfaces[s];
        GpuPlanarSurface_t *g = &cpuSurfaces[s];
        
        /* Adjust origin to the center of the first sub-pixel */
        g->originX   = p->origin[0] + shift * p->vecs[0][0] + shift * p->vecs[1][0];
        g->originY   = p->origin[1] + shift * p->vecs[0][1] + shift * p->vecs[1][1];
        g->originZ   = p->origin[2] + shift * p->vecs[0][2] + shift * p->vecs[1][2];

        /* Scale vectors down (one atlas unit = 1/scale native units) */
        g->vecs0X    = p->vecs[0][0] / fScale; 
        g->vecs0Y    = p->vecs[0][1] / fScale; 
        g->vecs0Z    = p->vecs[0][2] / fScale;
        g->vecs1X    = p->vecs[1][0] / fScale; 
        g->vecs1Y    = p->vecs[1][1] / fScale; 
        g->vecs1Z    = p->vecs[1][2] / fScale;

        /* invMagSq scales by scale^2 */
        g->invMagSq0 = p->invMagSq[0] * (fScale * fScale);
        g->invMagSq1 = p->invMagSq[1] * (fScale * fScale);

        g->width     = p->width * scale;   
        g->height    = p->height * scale;
        g->lmNum     = p->lmNum;
        g->lmOffX    = p->lmOffset[0] * scale; 
        g->lmOffY    = p->lmOffset[1] * scale;
        
        totalLinks  += p->numPartners;
    }
    st->surfacesBuf = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                      numPlanarSurfaces * sizeof(GpuPlanarSurface_t),
                                      cpuSurfaces, &err);
    free(cpuSurfaces);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU surfacesBuf (%d)\n", err); useOpenCL = qfalse; goto cleanup_upload; }

    /* --- Flatten partner CSR --- */
    int *offsets = malloc((numPlanarSurfaces + 1) * sizeof(int));
    int *data    = totalLinks > 0 ? malloc(totalLinks * sizeof(int)) : malloc(sizeof(int));
    if (!offsets || !data) { _printf("ERROR: Out of memory for partner lists\n"); free(offsets); free(data); useOpenCL = qfalse; goto cleanup_upload; }
    offsets[0] = 0;
    for (s = 0; s < numPlanarSurfaces; s++) {
        int base = offsets[s];
        for (int pi = 0; pi < planarSurfaces[s].numPartners; pi++)
            data[base + pi] = planarSurfaces[s].partners[pi];
        offsets[s + 1] = base + planarSurfaces[s].numPartners;
    }
    st->partnerOffsets = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         (numPlanarSurfaces + 1) * sizeof(int), offsets, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU partnerOffsets (%d)\n", err); free(offsets); free(data); useOpenCL = qfalse; goto cleanup_upload; }
    st->partnerData    = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         (totalLinks > 0 ? totalLinks : 1) * sizeof(int), data, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU partnerData (%d)\n", err); free(offsets); free(data); useOpenCL = qfalse; goto cleanup_upload; }
    free(offsets); free(data);

    /* --- Per-pixel lookup tables (SCALED) --- */
    int *pixelToSurface = malloc(totalPixels * sizeof(int));
    int *pixelToX       = malloc(totalPixels * sizeof(int));
    int *pixelToY       = malloc(totalPixels * sizeof(int));
    int *validList      = malloc(totalPixels * sizeof(int));
    if (!pixelToSurface || !pixelToX || !pixelToY || !validList) {
        _printf("ERROR: Out of memory for GPU lookup tables\n");
        free(pixelToSurface); free(pixelToX); free(pixelToY); free(validList);
        useOpenCL = qfalse; goto cleanup_upload;
    }
    memset(pixelToSurface, -1, totalPixels * sizeof(int));

    int numValid = 0;
    int W = LIGHTMAP_WIDTH * scale, H = LIGHTMAP_HEIGHT * scale;
    
    /* Rewriting the pixel mapping loop for clarity with scale */
    memset(pixelToSurface, -1, totalPixels * sizeof(int));
    numValid = 0;
    for (s = 0; s < numPlanarSurfaces; s++) {
        dsurface_t *ds = &drawSurfaces[planarSurfaces[s].surfaceNum];
        int lm = ds->lightmapNum[0];
        int sW = ds->lightmapWidth * scale;
        int sH = ds->lightmapHeight * scale;
        int offX = ds->lightmapOffset[0][0] * scale;
        int offY = ds->lightmapOffset[0][1] * scale;

        for (y = 0; y < sH; y++) {
            for (x = 0; x < sW; x++) {
                int p = (lm * H + offY + y) * W + offX + x;
                /* Mask check logic: we used tempMask for maskBuf, but it's easier to just re-sample lightAlphaMask */
                int p1x = (lm * LIGHTMAP_HEIGHT + (offY + y)/scale) * LIGHTMAP_WIDTH + (offX + x)/scale;
                if (lightAlphaMask[p1x] == 0) continue;

                pixelToSurface[p] = s;
                pixelToX[p]       = x;
                pixelToY[p]       = y;
                validList[numValid++] = p;
            }
        }
    }

    st->numValid = numValid;
    st->validList      = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         numValid * sizeof(int), validList, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU validList (%d)\n", err); useOpenCL = qfalse; }
    st->pixelToSurface = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         totalPixels * sizeof(int), pixelToSurface, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU pixelToSurface (%d)\n", err); useOpenCL = qfalse; }
    st->pixelToX       = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         totalPixels * sizeof(int), pixelToX, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU pixelToX (%d)\n", err); useOpenCL = qfalse; }
    st->pixelToY       = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         totalPixels * sizeof(int), pixelToY, &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to allocate GPU pixelToY (%d)\n", err); useOpenCL = qfalse; }
    
    free(pixelToSurface); free(pixelToX); free(pixelToY); free(validList);

    if (verbose) _printf("  GPU state uploaded (%dx): %d planar surfaces, %d valid texels, %d partner links\n",
            scale, numPlanarSurfaces, numValid, totalLinks);
    return;

cleanup_upload:
    if (scale > 1) { if (tempAtlas) free(tempAtlas); if (tempMask) free(tempMask); }
    GpuLightmapState_Free();
    _printf("  GPU upload failed. Falling back to CPU filtering.\n");
}

/*
================
RunGpuAAKernel

Dispatches the aa_filter kernel for one AA pass.
Reads from current ping buffer, writes to pong, then swaps.
pattern is a flat float[numSamples*2] array of (x,y) jitter offsets.
================
*/
static void RunGpuAAKernel(float *pattern, int numSamples, float radius) {
    GpuLightmapState *st = &g_gpuLM;
    cl_int err;

    static cl_program prog = NULL;
    if (!prog) {
        prog = BuildOpenCLProgramWithCommon("aa_filter.cl", "");
        if (!prog) { _printf("ERROR: Failed to build aa_filter.cl\n"); return; }
    }

    cl_kernel kernel = clCreateKernel(prog, "aa_filter", &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to create aa_filter kernel (%d)\n", err); return; }

    cl_mem src = st->pingIsA ? st->atlasA : st->atlasB;
    cl_mem dst = st->pingIsA ? st->atlasB : st->atlasA;

    size_t patBytes = (size_t)numSamples * 2 * sizeof(float);
    cl_mem patBuf   = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                      patBytes, pattern, &err);

    int arg = 0;
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &src);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &dst);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->maskBuf);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->surfacesBuf);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->partnerData);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->partnerOffsets);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->validList);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->pixelToSurface);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->pixelToX);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->pixelToY);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &patBuf);
    float scaledRadius = radius * (float)st->upscale;
    clSetKernelArg(kernel, arg++, sizeof(int),    &numSamples);
    clSetKernelArg(kernel, arg++, sizeof(float),  &scaledRadius);

    size_t globalSize = (size_t)st->numValid;
    err = clEnqueueNDRangeKernel(g_clQueue, kernel, 1, NULL, &globalSize, NULL, 0, NULL, NULL);
    if (err == CL_SUCCESS) {
        clFinish(g_clQueue);
        st->pingIsA ^= 1; /* swap ping-pong */
    } else {
        _printf("RunGpuAAKernel: dispatch failed (%d)\n", err);
    }

    clReleaseMemObject(patBuf);
    clReleaseKernel(kernel);
}

/*
================
RunGpuSmoothKernel

Dispatches the smooth_filter kernel for one smoothing pass.
Reads from current ping buffer, writes to pong, then swaps.
All N passes share the same persistent GPU metadata — no re-upload.
================
*/
static void RunGpuSmoothKernel(int kernelRadius, float sigma) {
    GpuLightmapState *st = &g_gpuLM;
    cl_int err;

    /* Build Gaussian weights on CPU (tiny: (2R+1)^2 floats) */
    int   diam    = 2 * kernelRadius + 1;
    int   wCount  = diam * diam;
    float *weights = malloc(wCount * sizeof(float));
    float  twoSigSq = 2.0f * sigma * sigma;
    float  wSum = 0.0f;
    for (int j = -kernelRadius; j <= kernelRadius; j++) {
        for (int i = -kernelRadius; i <= kernelRadius; i++) {
            float w = expf(-(float)(i*i + j*j) / twoSigSq);
            weights[(j + kernelRadius) * diam + (i + kernelRadius)] = w;
            wSum += w;
        }
    }
    for (int wi = 0; wi < wCount; wi++) weights[wi] /= wSum;

    static cl_program prog = NULL;
    if (!prog) {
        prog = BuildOpenCLProgramWithCommon("smooth_filter.cl", "");
        if (!prog) { _printf("ERROR: Failed to build smooth_filter.cl\n"); free(weights); return; }
    }

    cl_kernel kernel = clCreateKernel(prog, "smooth_filter", &err);
    if (err != CL_SUCCESS) { _printf("ERROR: Failed to create smooth_filter kernel (%d)\n", err); free(weights); return; }

    cl_mem src     = st->pingIsA ? st->atlasA : st->atlasB;
    cl_mem dst     = st->pingIsA ? st->atlasB : st->atlasA;
    cl_mem wBuf    = clCreateBuffer(g_clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                     wCount * sizeof(float), weights, &err);
    free(weights);

    int arg = 0;
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &src);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &dst);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->maskBuf);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->surfacesBuf);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->partnerData);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->partnerOffsets);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->validList);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->pixelToSurface);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->pixelToX);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &st->pixelToY);
    clSetKernelArg(kernel, arg++, sizeof(cl_mem), &wBuf);
    clSetKernelArg(kernel, arg++, sizeof(int),    &kernelRadius);

    size_t globalSize = (size_t)st->numValid;
    err = clEnqueueNDRangeKernel(g_clQueue, kernel, 1, NULL, &globalSize, NULL, 0, NULL, NULL);
    if (err == CL_SUCCESS) {
        clFinish(g_clQueue);
        st->pingIsA ^= 1; /* swap ping-pong */
    } else {
        _printf("RunGpuSmoothKernel: dispatch failed (%d)\n", err);
    }

    clReleaseMemObject(wBuf);
    clReleaseKernel(kernel);
}

/*
================
AntiAliasLightmapsGPU
================
*/
void AntiAliasLightmapsGPU(int passes) {
	if (passes <= 0 || !lightFloats || !lightAlphaMask) return;

	float radius = lightmapSmoothRadius > 0.0f ? lightmapSmoothRadius : 1.0f;
    int scale = g_gpuLM.upscale;
    _printf("  Image-space AA (%d passes, GPU planar %dx): dispatching kernel...\n", passes, scale);
    
    /* Use the unscaled pattern; RunGpuAAKernel scales the radius param by st->upscale */
    float pat[SS_PATTERN8_COUNT * 2];
    for (int ki = 0; ki < SS_PATTERN8_COUNT; ki++) {
        pat[ki*2+0] = ssPattern8[ki][0];
        pat[ki*2+1] = ssPattern8[ki][1];
    }
    
    for (int pnum = 0; pnum < passes; pnum++) {
        RunGpuAAKernel(pat, SS_PATTERN8_COUNT, radius); 
    }
}

/*
================
AntiAliasLightmapsCPU
================
*/
void AntiAliasLightmapsCPU(int passes) {
	int x, y, p, s, k;
	int numPixels;
	float *tempFloats;
	dsurface_t *ds;
    int progress = 0;

	if (passes <= 0 || !lightFloats || !lightAlphaMask) return;

	numPixels = numLightBytes / 3;
	tempFloats = malloc(numPixels * sizeof(float) * 3);
	if (!tempFloats) return;
	memcpy(tempFloats, lightFloats, numPixels * sizeof(float) * 3);

	float radius = lightmapSmoothRadius > 0.0f ? lightmapSmoothRadius : 1.0f;

    _printf("  Anti-aliasing surfaces (%d passes): ", passes);
    for (int pnum = 0; pnum < passes; pnum++) {
        if (passes > 1) _printf("%d ", pnum + 1);
        progress = 0;
        #pragma omp parallel for schedule(dynamic, 1) private(s, ds, x, y, p, k)
        for (s = 0; s < numPlanarSurfaces; s++) {
            ds = &drawSurfaces[planarSurfaces[s].surfaceNum];
            for (y = 0; y < ds->lightmapHeight; y++) {
                for (x = 0; x < ds->lightmapWidth; x++) {
                    p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
                    if (lightAlphaMask[p] != ALPHA_SURF_WORLD) continue;

                    float r = 0, g = 0, b = 0;
                    int cnt = 0;
                    for (k = 0; k < SS_PATTERN8_COUNT; k++) {
                        float col[3];
                        float px = (float)x + 0.5f + ssPattern8[k][0] * radius;
                        float py = (float)y + 0.5f + ssPattern8[k][1] * radius;
                        if (GetFilteredTexel(s, px, py, col, tempFloats)) {
                            r += col[0]; g += col[1]; b += col[2];
                            cnt++;
                        }
                    }
                    if (cnt > 0) {
                        lightFloats[p * 3 + 0] = r / cnt;
                        lightFloats[p * 3 + 1] = g / cnt;
                        lightFloats[p * 3 + 2] = b / cnt;
                    }
                }
            }
            int currentProgress;
            #pragma omp atomic capture
            currentProgress = ++progress;
            if (numPlanarSurfaces >= 10) {
                int oldPercent = ((currentProgress - 1) * 10) / numPlanarSurfaces;
                int newPercent = (currentProgress * 10) / numPlanarSurfaces;
                if (newPercent > oldPercent) { ThreadLock(); _printf("."); ThreadUnlock(); }
            }
        }
        if (pnum < passes - 1) memcpy(tempFloats, lightFloats, numPixels * sizeof(float) * 3);
    }
    _printf("Done\n");
	free(tempFloats);
}

/*
================
FilterPlanarSurfaceHighFidelityCPU

Unified high-fidelity filtering for a single planar surface.
Upscales to 2x, runs all AA and Smooth passes, then downscales.
================
*/
static void FilterPlanarSurfaceHighFidelityCPU(int sIdx, float radius, const float *tempFloats, int aaPasses, int smoothPasses) {
	int x, y, k, p;
	dsurface_t *ds = &drawSurfaces[planarSurfaces[sIdx].surfaceNum];

	int W = ds->lightmapWidth, H = ds->lightmapHeight;
	if (W <= 0 || H <= 0) return;

	int W2 = W * 2, H2 = H * 2;
	float *grid2x = malloc(W2 * H2 * 3 * sizeof(float));
	byte *mask2x = malloc(W2 * H2 * sizeof(byte));
	float *blur2x = malloc(W2 * H2 * 3 * sizeof(float));
	byte *blurMask2x = malloc(W2 * H2 * sizeof(byte));

	if (!grid2x || !mask2x || !blur2x || !blurMask2x) {
		if (grid2x) free(grid2x);
		if (mask2x) free(mask2x);
		if (blur2x) free(blur2x);
		if (blurMask2x) free(blurMask2x);
		return;
	}

	// 1. Upscale to 2x
	for (int Y = 0; Y < H2; Y++) {
		for (int X = 0; X < W2; X++) {
			float px = ((float)X + 0.5f) * 0.5f;
			float py = ((float)Y + 0.5f) * 0.5f;
			float col[3];
			if (GetFilteredTexel(sIdx, px, py, col, tempFloats)) {
				mask2x[Y * W2 + X] = ALPHA_SURF_WORLD;
				VectorCopy(col, &grid2x[(Y * W2 + X) * 3]);
			} else {
				mask2x[Y * W2 + X] = 0;
				VectorClear(&grid2x[(Y * W2 + X) * 3]);
			}
		}
	}

	int totalPasses = aaPasses + smoothPasses;
	float sigma = (radius * 2.0f) / 3.0f;
	if (sigma < 1.0f) sigma = 1.0f;
	int kernelRadius = (int)ceil(radius * 2.0f);
	if (kernelRadius > MAX_KERNEL_RADIUS * 2) kernelRadius = MAX_KERNEL_RADIUS * 2;

	// Build Smoothing kernel (scale-aware for 2x grid)
	float gKernel[MAX_KERNEL_RADIUS * 4 + 1][MAX_KERNEL_RADIUS * 4 + 1];
	float gKernelSum = 0.0f;
	for (int j = -kernelRadius; j <= kernelRadius; j++)
		for (int i = -kernelRadius; i <= kernelRadius; i++) {
			gKernel[j+kernelRadius][i+kernelRadius] = expf(-(float)(i*i+j*j) / (2.0f*sigma*sigma));
			gKernelSum += gKernel[j+kernelRadius][i+kernelRadius];
		}
	for (int j = 0; j <= kernelRadius*2; j++)
		for (int i = 0; i <= kernelRadius*2; i++)
			gKernel[j][i] /= gKernelSum;

	for (int currentPass = 0; currentPass < totalPasses; currentPass++) {
		qboolean isAA = (currentPass < aaPasses);

		for (int Y = 0; Y < H2; Y++) {
			for (int X = 0; X < W2; X++) {
				if (mask2x[Y * W2 + X] == 0) { blurMask2x[Y * W2 + X] = 0; continue; }

				float sumColor[3] = {0, 0, 0}, sumWeight = 0.0f;

				if (isAA) {
					// AA Pass (8-tap grid)
					for (k = 0; k < SS_PATTERN8_COUNT; k++) {
						float px = (float)X + ssPattern8[k][0] * radius * 2.0f;
						float py = (float)Y + ssPattern8[k][1] * radius * 2.0f;
						int ix = (int)roundf(px), iy = (int)roundf(py);

						if (ix >= 0 && ix < W2 && iy >= 0 && iy < H2) {
							if (mask2x[iy * W2 + ix] != 0) {
								VectorAdd(sumColor, &grid2x[(iy * W2 + ix) * 3], sumColor);
								sumWeight += 1.0f;
							}
						} else {
							float srcX = ((float)px + 0.5f) * 0.5f;
							float srcY = ((float)py + 0.5f) * 0.5f;
							float col[3];
							if (GetFilteredTexel(sIdx, srcX, srcY, col, tempFloats)) {
								VectorAdd(sumColor, col, sumColor);
								sumWeight += 1.0f;
							}
						}
					}
				} else {
					// Smoothing Pass (Gaussian)
					for (int j = -kernelRadius; j <= kernelRadius; j++) {
						for (int i = -kernelRadius; i <= kernelRadius; i++) {
							float weight = gKernel[j+kernelRadius][i+kernelRadius];
							int ix = X + i, iy = Y + j;
							if (ix >= 0 && ix < W2 && iy >= 0 && iy < H2) {
								if (mask2x[iy * W2 + ix] != 0) {
									VectorMA(sumColor, weight, &grid2x[(iy * W2 + ix) * 3], sumColor);
									sumWeight += weight;
								}
							} else {
								float srcX = ((float)ix + 0.5f) * 0.5f;
								float srcY = ((float)iy + 0.5f) * 0.5f;
								float col[3];
								if (GetFilteredTexel(sIdx, srcX, srcY, col, tempFloats)) {
									VectorMA(sumColor, weight, col, sumColor);
									sumWeight += weight;
								}
							}
						}
					}
				}

				if (sumWeight > 0.0001f) {
					blurMask2x[Y * W2 + X] = ALPHA_SURF_WORLD;
					VectorScale(sumColor, 1.0f / sumWeight, &blur2x[(Y * W2 + X) * 3]);
				} else {
					blurMask2x[Y * W2 + X] = 0;
				}
			}
		}
		// Update grid for next pass
		memcpy(grid2x, blur2x, W2 * H2 * 3 * sizeof(float));
		memcpy(mask2x, blurMask2x, W2 * H2 * sizeof(byte));
	}

	// 3. Downscale back to 1x
	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
			if (lightAlphaMask[p] != ALPHA_SURF_WORLD) continue;

			float sumColor[3] = {0,0,0}, sumWeight = 0.0f;
			for (int dy = 0; dy < 2; dy++) {
				for (int dx = 0; dx < 2; dx++) {
					int X = x * 2 + dx, Y = y * 2 + dy;
					if (mask2x[Y * W2 + X] != 0) {
						VectorAdd(sumColor, &grid2x[(Y * W2 + X) * 3], sumColor);
						sumWeight += 1.0f;
					}
				}
			}
			if (sumWeight > 0.01f) {
				VectorScale(sumColor, 1.0f / sumWeight, &lightFloats[p * 3]);
			}
		}
	}

	free(grid2x); free(mask2x); free(blur2x); free(blurMask2x);
}

/*
================
SmoothLightmapsGPU
================
*/
void SmoothLightmapsGPU(float radius) {
	if (radius <= 0.0f || !lightFloats || !lightAlphaMask) return;

    int scale = g_gpuLM.upscale;
	float sigma = (radius * scale) / 3.0f;
	if (sigma < 0.5f * scale) sigma = 0.5f * scale;
	int kernelRadius = (int)ceil(radius * scale);
	if (kernelRadius > MAX_KERNEL_RADIUS * scale) kernelRadius = MAX_KERNEL_RADIUS * scale;

    RunGpuSmoothKernel(kernelRadius, sigma);
}

/*
================
SmoothLightmapsCPU
================
*/
void SmoothLightmapsCPU(float radius) {
	int i, j, x, y, p, s;
	dsurface_t *ds;
	int progress = 0;

	if (radius <= 0.0f || !lightFloats || !lightAlphaMask) return;

	float sigma = radius / 3.0f;
	if (sigma < 0.5f) sigma = 0.5f;
	int kernelRadius = (int)ceil(radius);
	if (kernelRadius > MAX_KERNEL_RADIUS) kernelRadius = MAX_KERNEL_RADIUS;

    /* CPU: build normalized 2D Gaussian kernel */
    float kernel[MAX_KERNEL_RADIUS * 2 + 1][MAX_KERNEL_RADIUS * 2 + 1];
    float kernelSum = 0.0f;
    for (j = -kernelRadius; j <= kernelRadius; j++)
        for (i = -kernelRadius; i <= kernelRadius; i++) {
            kernel[j+kernelRadius][i+kernelRadius] = expf(-(float)(i*i+j*j) / (2.0f*sigma*sigma));
            kernelSum += kernel[j+kernelRadius][i+kernelRadius];
        }
    for (j = 0; j <= kernelRadius*2; j++)
        for (i = 0; i <= kernelRadius*2; i++)
            kernel[j][i] /= kernelSum;

    int numPixels = numLightBytes / 3;
    float *tempFloats = malloc(numPixels * sizeof(float) * 3);
    if (!tempFloats) return;
    memcpy(tempFloats, lightFloats, numPixels * sizeof(float) * 3);

    progress = 0;
    #pragma omp parallel for schedule(dynamic, 1) private(s, ds, y, x, i, j, p)
    for (s = 0; s < numPlanarSurfaces; s++) {
        ds = &drawSurfaces[planarSurfaces[s].surfaceNum];
        for (y = 0; y < ds->lightmapHeight; y++) {
            for (x = 0; x < ds->lightmapWidth; x++) {
                p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
                if (lightAlphaMask[p] != ALPHA_SURF_WORLD) continue;
                float sumColor[3] = {0,0,0}, sumWeight = 0.0f;
                for (j = -kernelRadius; j <= kernelRadius; j++) {
                    for (i = -kernelRadius; i <= kernelRadius; i++) {
                        float weight = kernel[j+kernelRadius][i+kernelRadius];
                        float sampleColor[3];
                        if (GetFilteredTexel(s, (float)(x+i) + 0.5f, (float)(y+j) + 0.5f, sampleColor, tempFloats)) {
                            VectorMA(sumColor, weight, sampleColor, sumColor);
                            sumWeight += weight;
                        }
                    }
                }
                if (sumWeight > 0.0001f)
                    VectorScale(sumColor, 1.0f / sumWeight, &lightFloats[p * 3]);
            }
        }
        int currentProgress;
        #pragma omp atomic capture
        currentProgress = ++progress;
        if (numPlanarSurfaces >= 10) {
            int oldPercent = ((currentProgress - 1) * 10) / numPlanarSurfaces;
            int newPercent = (currentProgress * 10) / numPlanarSurfaces;
            if (newPercent > oldPercent) { ThreadLock(); _printf("."); ThreadUnlock(); }
        }
    }
    free(tempFloats);
}

void PostProcessLightmaps(void) {
    lightmapAA = game->antialiasingPasses;
    lightmapSmoothRadius = game->defaultSmoothRadius;
    lightmapSmoothPasses = game->defaultSmoothPasses;

	_printf("--- Post Processing ---\n");
	BuildPlanarSurfaceIndex();
    double startFiltering = I_FloatTime();

    /* Step 3 — Planar Filtering (AA and Smoothing) */
    if (useOpenCL) {
        /* ==== GPU PATH ==== */
        if (verbose) _printf("  Uploading GPU lightmap state...\n");
        GpuLightmapState_Upload();
        if (!useOpenCL) {
            _printf("  GPU initialization failed. Falling back to CPU path.\n");
            goto fallback_cpu;
        }
        if (lightmapAA) AntiAliasLightmapsGPU(lightmapAA);
        if (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) {
            _printf("  Smoothing surfaces (%d passes, radius %.2f): ", lightmapSmoothPasses, lightmapSmoothRadius);
            for (int pnum = 1; pnum <= lightmapSmoothPasses; pnum++) {
                _printf("%d ", pnum);
                SmoothLightmapsGPU(lightmapSmoothRadius);
            }
            _printf("Done\n");
        }
        if (verbose) _printf("  Downloading GPU lightmap result...\n");
        GpuLightmapState_Download();
        GpuLightmapState_Free();
    } else {
    fallback_cpu:
        /* ==== CPU PATH ==== */
        if (FILTER_UPSCALE) {
            _printf("  High-Fidelity Filtering (Planar - 2x): ");
            int progress = 0;
            int s;
            int numPixels = numLightBytes / 3;
            float *tempFloats = malloc(numPixels * sizeof(float) * 3);
            if (tempFloats) {
                memcpy(tempFloats, lightFloats, numPixels * sizeof(float) * 3);
                int aaPasses = lightmapAA;
                int smoothPasses = (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) ? lightmapSmoothPasses : 0;

                #pragma omp parallel for schedule(dynamic, 1) private(s)
                for (s = 0; s < numPlanarSurfaces; s++) {
                    FilterPlanarSurfaceHighFidelityCPU(s, lightmapSmoothRadius > 0.0f ? lightmapSmoothRadius : 1.0f, tempFloats, aaPasses, smoothPasses);
                    
                    int currentProgress;
                    #pragma omp atomic capture
                    currentProgress = ++progress;
                    if (numPlanarSurfaces >= 10) {
                        int oldPercent = ((currentProgress - 1) * 10) / numPlanarSurfaces;
                        int newPercent = (currentProgress * 10) / numPlanarSurfaces;
                        if (newPercent > oldPercent) { ThreadLock(); _printf("."); ThreadUnlock(); }
                    }
                }
                free(tempFloats);
                _printf("Done\n");
            }
        } else {
            /* Note: AntiAliasLightmaps(passes) already loops internally on CPU. */
            if (lightmapAA) AntiAliasLightmapsCPU(lightmapAA);
            if (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) {
                _printf("  Smoothing surfaces (%d passes, radius %.2f): ", lightmapSmoothPasses, lightmapSmoothRadius);
                for (int pnum = 1; pnum <= lightmapSmoothPasses; pnum++) {
                    _printf("%d ", pnum);
                    SmoothLightmapsCPU(lightmapSmoothRadius);
                }
                _printf("Done\n");
            }
        }
    }

    /* Step 4 — Volumetric (Trisoup) Filtering */
    int aaPasses = lightmapAA;
    int smoothPasses = (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) ? lightmapSmoothPasses : 0;
    
    if (aaPasses > 0 || smoothPasses > 0) {
        float radius = lightmapSmoothRadius > 0.0f ? lightmapSmoothRadius : 1.0f;
        _printf("  Volumetric Filtering (Trisoups): ");
        
        int numPx = numLightBytes / 3;
        size_t bufferSize = (size_t)numPx * 3 * sizeof(float);
        float *tempFloats = malloc(bufferSize);
        if (tempFloats) {
            memcpy(tempFloats, lightFloats, bufferSize);
            
            int progress = 0;
            if (useOpenCL) {
                /* GPU path for trisoups: MUST be serial to avoid race conditions on lightFloats/g_clQueue and out-of-memory errors */
                for (int s = 0; s < numDrawSurfaces; s++) {
                    ProcessTrisoupVolumetricGPU(s, radius, tempFloats, aaPasses, smoothPasses);
                    int cur = ++progress;
                    if (numDrawSurfaces >= 10) {
                        int op = ((cur-1)*10)/numDrawSurfaces, np = (cur*10)/numDrawSurfaces;
                        if (np > op) { ThreadLock(); _printf("."); ThreadUnlock(); }
                    }
                }
            } else {
                #pragma omp parallel for schedule(dynamic, 1)
                for (int s = 0; s < numDrawSurfaces; s++) {
                    ProcessTrisoupVolumetricCPU(s, radius, tempFloats, aaPasses, smoothPasses);
                    int cur;
                    #pragma omp atomic capture
                    cur = ++progress;
                    if (numDrawSurfaces >= 10) {
                        int op = ((cur-1)*10)/numDrawSurfaces, np = (cur*10)/numDrawSurfaces;
                        if (np > op) { ThreadLock(); _printf("."); ThreadUnlock(); }
                    }
                }
            }
            free(tempFloats);
            _printf("Done\n");
        }
    }

    double endFiltering = I_FloatTime();
    _printf("  Total 2D filtering time: %.2f seconds\n", endFiltering - startFiltering);
    FreePlanarSurfaceIndex();
}
