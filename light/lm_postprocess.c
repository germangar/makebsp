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

int lightmapAA = 0;
float lightmapSmoothRadius = 0.0f;
int lightmapSmoothPasses = 0;

#define AA_ANGLE_MATCH_DEGREES 30.0f
static float aa_angle_match_cos = 0.85f;

#define MAX_KERNEL_RADIUS 16

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
		if (ds->surfaceType != MST_PLANAR || ds->lightmapNum[0] < 0) continue;

		planarInfo_t *p = &planarSurfaces[numPlanarSurfaces];
		planarSortIndex[numPlanarSurfaces] = numPlanarSurfaces;
		numPlanarSurfaces++;

		p->surfaceNum = i;
		VectorCopy(ds->lightmapOrigin, p->origin);
		VectorAdd(p->origin, surfaceOrigin[i], p->origin);
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

		// Bilinear sample
		int x0 = (int)floorf(u);
		int y0 = (int)floorf(v);
		float fx = u - x0;
		float fy = v - y0;

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

		if (lightAlphaMask[p00] == 0) w00 = 0.0f;
		if (lightAlphaMask[p10] == 0) w10 = 0.0f;
		if (lightAlphaMask[p01] == 0) w01 = 0.0f;
		if (lightAlphaMask[p11] == 0) w11 = 0.0f;

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
	
	int x0 = (int)floorf(px);
	int y0 = (int)floorf(py);
	float fx = px - (float)x0;
	float fy = py - (float)y0;
	
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

		if (lightAlphaMask[p00] == 0) w00 = 0.0f;
		if (lightAlphaMask[p10] == 0) w10 = 0.0f;
		if (lightAlphaMask[p01] == 0) w01 = 0.0f;
		if (lightAlphaMask[p11] == 0) w11 = 0.0f;

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
	VectorMA(ds->lightmapOrigin, px, ds->lightmapVecs[0], worldPos);
	VectorMA(worldPos, py, ds->lightmapVecs[1], worldPos);
	VectorAdd(worldPos, surfaceOrigin[pInfo->surfaceNum], worldPos);
	
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
	if (lightAlphaMask[p00] == 0) return qfalse;

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

extern int samplesize;
#define AA_ANGLE_MATCH_COS 0.85f

typedef struct aaTexel_s {
    vec3_t pos;
    vec3_t normal;
    vec3_t color;
    struct aaTexel_s *next;
} aaTexel_t;

// Cached world-space position/normal for a single lightmap pixel center.
// Avoids redundant TriSoupSamplePoint calls across the three passes of VPPS.
typedef struct {
    vec3_t pos;
    vec3_t normal;
    qboolean valid;
} pixelCache_t;

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

#define TRISOUP_SMOOTH_CHEAT 1.25f

/*
================
ProcessTrisoupVolumetric

Creates a temporary, strictly local 3D spatial hash for a single Triangle Soup surface.
It voxelizes the surface's pixels, then immediately samples them using a true
3D Gaussian blur, guaranteeing perfect blending across UV islands and matching the
"softness" of world geometry regardless of editor scaling or _lightmapscale.
================
*/
static void ProcessTrisoupVolumetric(int surfIdx, float radius, float *tempFloats, qboolean isAA) {
    dsurface_t *ds = &drawSurfaces[surfIdx];
    if (ds->lightmapNum[0] < 0 || ds->surfaceType != MST_TRIANGLE_SOUP) return;

    int x, y, p, i, k;
    const int W = ds->lightmapWidth, H = ds->lightmapHeight;

    // 1. Calculate true density and set up local bounds
    float texelSize = GetSurfaceTexelSize(ds);
    float effectiveRadius = isAA ? radius : (radius * TRISOUP_SMOOTH_CHEAT);
    float searchRadius = effectiveRadius * texelSize;
    if (searchRadius < 0.1f) return;

    // The grid cells must be large enough that a 3x3x3 search guarantees we reach 'searchRadius'.
    // A cell size equal to the search radius means a 3x3x3 covers at minimum a sphere of radius R.
    float voxelSize = searchRadius;

    // OPT: Pre-cache the world-space position and normal for every pixel center.
    // This single flat array lets us skip redundant TriSoupSamplePoint calls in
    // the voxelization and (k==0) sampling passes below.
    pixelCache_t *pixCache = malloc(W * H * sizeof(pixelCache_t));
    if (!pixCache) return;

    // OPT: Merged Pass 1+2 (bounds + cache population).
    // Previously two separate W*H loops both calling TriSoupSamplePoint.
    // Now a single loop fills the cache, tracks bounds, and counts mapped pixels.
    vec3_t gridMins = {99999.0f, 99999.0f, 99999.0f};
    vec3_t gridMaxs = {-99999.0f, -99999.0f, -99999.0f};
    int mappedPixels = 0;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            pixelCache_t *pc = &pixCache[y * W + x];
            p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
            if (lightAlphaMask[p] == 0) { pc->valid = qfalse; continue; }

            float st[2];
            st[0] = (float)ds->lightmapOffset[0][0] + (float)x + 0.5f;
            st[1] = (float)ds->lightmapOffset[0][1] + (float)y + 0.5f;

            if (TriSoupSamplePoint(ds, st, pc->pos, pc->normal)) {
                pc->valid = qtrue;
                mappedPixels++;
                for (i = 0; i < 3; i++) {
                    if (pc->pos[i] < gridMins[i]) gridMins[i] = pc->pos[i];
                    if (pc->pos[i] > gridMaxs[i]) gridMaxs[i] = pc->pos[i];
                }
            } else {
                pc->valid = qfalse;
            }
        }
    }

    if (mappedPixels == 0) { free(pixCache); return; }

    // Expand bounds slightly to ensure edge cases fall neatly into buckets
    for (i = 0; i < 3; i++) {
        gridMins[i] -= voxelSize;
        gridMaxs[i] += voxelSize;
    }

    int gridDims[3];
    for (i = 0; i < 3; i++) {
        gridDims[i] = (int)ceilf((gridMaxs[i] - gridMins[i]) / voxelSize);
        if (gridDims[i] < 1) gridDims[i] = 1;
    }
    const int gStride1 = gridDims[1] * gridDims[2];
    const int gStride2 = gridDims[2];

    // OPT: Flat 1D grid array replaces the old jagged 4D pointer structure.
    // Single calloc + single free; contiguous memory = fewer cache misses during hash lookup.
    aaTexel_t **flatGrid = (aaTexel_t **)calloc(gridDims[0] * gridDims[1] * gridDims[2], sizeof(aaTexel_t *));
    if (!flatGrid) { free(pixCache); return; }

    // OPT: Flat pool for aaTexel_t nodes replaces one malloc(sizeof(aaTexel_t)) per pixel.
    // Single allocation; nodes are packed contiguously so walking linked-list chains
    // is friendlier to the CPU cache.
    aaTexel_t *pool = (aaTexel_t *)malloc(mappedPixels * sizeof(aaTexel_t));
    if (!pool) { free(flatGrid); free(pixCache); return; }
    int poolIdx = 0;

    // Pass 2 (Voxelize): reuse pixCache — no TriSoupSamplePoint calls needed.
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            const pixelCache_t *pc = &pixCache[y * W + x];
            if (!pc->valid) continue;

            p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;

            int v[3];
            qboolean valid = qtrue;
            for (i = 0; i < 3; i++) {
                v[i] = (int)((pc->pos[i] - gridMins[i]) / voxelSize);
                if (v[i] < 0 || v[i] >= gridDims[i]) { valid = qfalse; break; }
            }

            if (valid) {
                aaTexel_t *newT = &pool[poolIdx++];
                VectorCopy(pc->pos,    newT->pos);
                VectorCopy(pc->normal, newT->normal);
                // Read from tempFloats to allow multi-pass iteration
                newT->color[0] = tempFloats[p * 3 + 0];
                newT->color[1] = tempFloats[p * 3 + 1];
                newT->color[2] = tempFloats[p * 3 + 2];

                int cell = v[0] * gStride1 + v[1] * gStride2 + v[2];
                newT->next   = flatGrid[cell];
                flatGrid[cell] = newT;
            }
        }
    }

    // Pass 3 (Sample and Blur — 3D Gaussian)
    float maxDistSq  = searchRadius * searchRadius;
    float sigma      = searchRadius / 3.0f; // radius = 3 standard deviations
    if (sigma < 0.1f) sigma = 0.1f;
    float twoSigmaSq = 2.0f * sigma * sigma;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
            if (lightAlphaMask[p] == 0) continue;

            const int numSamples = isAA ? SS_PATTERN8_COUNT : 1;
            vec3_t finalColor = {0.0f, 0.0f, 0.0f};
            float  finalWeight = 0.0f;

            for (k = 0; k < numSamples; k++) {
                vec3_t origin, normal;

                if (isAA && k > 0) {
                    // Jittered AA samples still require a full UV->world lookup.
                    float st[2];
                    st[0] = (float)ds->lightmapOffset[0][0] + (float)x + 0.5f + ssPattern8[k][0] * radius;
                    st[1] = (float)ds->lightmapOffset[0][1] + (float)y + 0.5f + ssPattern8[k][1] * radius;
                    if (!TriSoupSamplePoint(ds, st, origin, normal)) continue;
                } else {
                    // OPT: k==0 (center sample, both AA and Smooth modes) reuses the
                    // already-computed cache entry — no TriSoupSamplePoint call needed.
                    const pixelCache_t *pc = &pixCache[y * W + x];
                    if (!pc->valid) continue;
                    VectorCopy(pc->pos,    origin);
                    VectorCopy(pc->normal, normal);
                }

                int v[3];
                for (i = 0; i < 3; i++) {
                    v[i] = (int)((origin[i] - gridMins[i]) / voxelSize);
                }

                vec3_t totalColor  = {0.0f, 0.0f, 0.0f};
                float  totalWeight = 0.0f;

                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dz = -1; dz <= 1; dz++) {
                            int nx = v[0] + dx, ny = v[1] + dy, nz = v[2] + dz;
                            if (nx < 0 || nx >= gridDims[0] ||
                                ny < 0 || ny >= gridDims[1] ||
                                nz < 0 || nz >= gridDims[2]) continue;

                            aaTexel_t *curr = flatGrid[nx * gStride1 + ny * gStride2 + nz];
                            while (curr) {
                                float dot = DotProduct(curr->normal, normal);
                                if (dot > aa_angle_match_cos) {
                                    vec3_t delta;
                                    VectorSubtract(origin, curr->pos, delta);
                                    float distSq = DotProduct(delta, delta);
                                    if (distSq <= maxDistSq) {
                                        // True 3D Gaussian Weight
                                        float w = expf(-distSq / twoSigmaSq) * dot;
                                        VectorMA(totalColor, w, curr->color, totalColor);
                                        totalWeight += w;
                                    }
                                }
                                curr = curr->next;
                            }
                        }
                    }
                }

                if (totalWeight > 0.0001f) {
                    VectorMA(finalColor, 1.0f / totalWeight, totalColor, finalColor);
                    finalWeight += 1.0f;
                }
            }

            if (finalWeight > 0.0001f) {
                VectorScale(finalColor, 1.0f / finalWeight, &lightFloats[p * 3]);
            }
        }
    }

    // Cleanup: three frees instead of O(N) individual frees + jagged pointer teardown
    free(pool);
    free(flatGrid);
    free(pixCache);
}

void AntiAliasLightmaps(void) {
	int x, y, p, s, k;
	int numPixels;
	float *tempFloats;
	dsurface_t *ds;

	if (!lightFloats || !lightAlphaMask) return;

	numPixels = numLightBytes / 3;
	tempFloats = malloc(numPixels * sizeof(float) * 3);
	if (!tempFloats) return;
	memcpy(tempFloats, lightFloats, numPixels * sizeof(float) * 3);

	float radius = lightmapSmoothRadius > 0.0f ? lightmapSmoothRadius : 1.0f;

    // AA for Triangle Soups (VPPS Spatial Hash)
    _printf("  Volumetric AA (Trisoups): ");
    int progress = 0;
    #pragma omp parallel for schedule(dynamic, 1) private(s)
    for (s = 0; s < numDrawSurfaces; s++) {
        ProcessTrisoupVolumetric(s, radius, tempFloats, qtrue);
        
        int currentProgress;
        #pragma omp atomic capture
        currentProgress = ++progress;

        if (numDrawSurfaces >= 10) {
            int oldPercent = ((currentProgress - 1) * 10) / numDrawSurfaces;
            int newPercent = (currentProgress * 10) / numDrawSurfaces;
            if (newPercent > oldPercent) {
                ThreadLock();
                _printf("%d...", newPercent);
                ThreadUnlock();
            }
        }    }
    _printf("Done\n");

	if (lightmapAA == 1) {
        _printf("  Image-space AA (Mode 1): ");
        progress = 0;
		#pragma omp parallel for schedule(dynamic, 1) private(s, ds, x, y, p, k)
		for (s = 0; s < numPlanarSurfaces; s++) {
			ds = &drawSurfaces[planarSurfaces[s].surfaceNum];
			
			for (y = 0; y < ds->lightmapHeight; y++) {
				for (x = 0; x < ds->lightmapWidth; x++) {
					p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
					if (lightAlphaMask[p] == 0) continue;

					float sumColor[3] = {0, 0, 0};
					float sumWeight = 0.0f;

					for (k = 0; k < SS_PATTERN8_COUNT; k++) {
						float px = (float)x + ssPattern8[k][0] * radius;
						float py = (float)y + ssPattern8[k][1] * radius;

						float sampleColor[3];
						if (GetFilteredTexel(s, px, py, sampleColor, tempFloats)) {
							VectorAdd(sumColor, sampleColor, sumColor);
							sumWeight += 1.0f;
						}
					}

					if (sumWeight > 0.0001f) {
						VectorScale(sumColor, 1.0f / sumWeight, &lightFloats[p * 3]);
					}
				}
			}
            int currentProgress;
            #pragma omp atomic capture
            currentProgress = ++progress;

            if (numPlanarSurfaces >= 10) {
                int oldPercent = ((currentProgress - 1) * 10) / numPlanarSurfaces;
                int newPercent = (currentProgress * 10) / numPlanarSurfaces;
                if (newPercent > oldPercent) {
                    ThreadLock();
                    _printf("%d...", newPercent);
                    ThreadUnlock();
                }
            }
		}
        _printf("Done\n");
	} else if (lightmapAA == 2) {
        _printf("  Image-space AA (Mode 2 - High Fidelity): ");
        progress = 0;
		#pragma omp parallel for schedule(dynamic, 1) private(s, ds, x, y, p, k)
		for (s = 0; s < numPlanarSurfaces; s++) {
			ds = &drawSurfaces[planarSurfaces[s].surfaceNum];

			int W = ds->lightmapWidth, H = ds->lightmapHeight;
			if (W <= 0 || H <= 0) continue;

			int W2 = W * 2, H2 = H * 2;
			float *temp2x = malloc(W2 * H2 * 3 * sizeof(float));
			byte *mask2x = malloc(W2 * H2 * sizeof(byte));
			float *blur2x = malloc(W2 * H2 * 3 * sizeof(float));
			byte *blurMask2x = malloc(W2 * H2 * sizeof(byte));

			// 1. Upscale
			for (int Y = 0; Y < H2; Y++) {
				for (int X = 0; X < W2; X++) {
					float px = (float)X * 0.5f - 0.25f;
					float py = (float)Y * 0.5f - 0.25f;

					float sampleColor[3];
					if (GetFilteredTexel(s, px, py, sampleColor, tempFloats)) {
						mask2x[Y * W2 + X] = ALPHA_SURF_WORLD;
						VectorCopy(sampleColor, &temp2x[(Y * W2 + X) * 3]);
					} else {
						mask2x[Y * W2 + X] = 0;
						VectorClear(&temp2x[(Y * W2 + X) * 3]);
					}
				}
			}

			// 2. Blur (Pattern)
			for (int Y = 0; Y < H2; Y++) {
				for (int X = 0; X < W2; X++) {
					if (mask2x[Y * W2 + X] == 0) { blurMask2x[Y * W2 + X] = 0; continue; }
					
					float sumColor[3] = {0,0,0}, sumWeight = 0.0f;
					for (k = 0; k < SS_PATTERN8_COUNT; k++) {
						float px = (float)X + ssPattern8[k][0] * radius * 2.0f;
						float py = (float)Y + ssPattern8[k][1] * radius * 2.0f;
						int ix = (int)roundf(px), iy = (int)roundf(py);
						
						float sampleColor[3];
						if (ix >= 0 && ix < W2 && iy >= 0 && iy < H2) {
							if (mask2x[iy * W2 + ix] != 0) {
								VectorAdd(sumColor, &temp2x[(iy * W2 + ix) * 3], sumColor);
								sumWeight += 1.0f;
							}
						} else {
							// Kernel reached beyond the upscaled grid, reach out to world space
							float srcX = px * 0.5f - 0.25f;
							float srcY = py * 0.5f - 0.25f;
							if (GetFilteredTexel(s, srcX, srcY, sampleColor, tempFloats)) {
								VectorAdd(sumColor, sampleColor, sumColor);
								sumWeight += 1.0f;
							}
						}
					}
					if (sumWeight > 0.0001f) {
						blurMask2x[Y * W2 + X] = ALPHA_SURF_WORLD;
						VectorScale(sumColor, 1.0f / sumWeight, &blur2x[(Y * W2 + X) * 3]);
					} else blurMask2x[Y * W2 + X] = 0;
				}
			}

			// 3. Reduce back
			for (y = 0; y < H; y++) {
				for (x = 0; x < W; x++) {
					p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
					if (lightAlphaMask[p] == 0) continue;
					float sumColor[3] = {0,0,0}, sumWeight = 0.0f;
					for (int dy = 0; dy < 2; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							int X = x * 2 + dx, Y = y * 2 + dy;
							if (blurMask2x[Y * W2 + X] != 0) {
								VectorAdd(sumColor, &blur2x[(Y * W2 + X) * 3], sumColor);
								sumWeight += 1.0f;
							}
						}
					}
					if (sumWeight > 0.0001f) VectorScale(sumColor, 1.0f / sumWeight, &lightFloats[p * 3]);
				}
			}
			free(temp2x); free(mask2x); free(blur2x); free(blurMask2x);

            int currentProgress;
            #pragma omp atomic capture
            currentProgress = ++progress;

            if (numPlanarSurfaces >= 10) {
                int oldPercent = ((currentProgress - 1) * 10) / numPlanarSurfaces;
                int newPercent = (currentProgress * 10) / numPlanarSurfaces;
                if (newPercent > oldPercent) {
                    ThreadLock();
                    _printf("%d...", newPercent);
                    ThreadUnlock();
                }
            }
		}
        _printf("Done\n");
	}
	free(tempFloats);
}

void SmoothLightmaps(float radius) {
	int i, j, x, y, p, s;
	float *tempFloats;
	float kernel[MAX_KERNEL_RADIUS * 2 + 1][MAX_KERNEL_RADIUS * 2 + 1];
	int kernelRadius;
	float sigma;
	dsurface_t *ds;

	if (radius <= 0.0f || !lightFloats || !lightAlphaMask) return;

	sigma = radius / 3.0f;
	if (sigma < 0.5f) sigma = 0.5f;
	kernelRadius = (int)ceil(radius);
	if (kernelRadius > MAX_KERNEL_RADIUS) kernelRadius = MAX_KERNEL_RADIUS;

	// Prepare 2D Gaussian Kernel
	float kernelSum = 0.0f;
	for (j = -kernelRadius; j <= kernelRadius; j++) {
		for (i = -kernelRadius; i <= kernelRadius; i++) {
			float distSq = (float)(i * i + j * j);
			kernel[j + kernelRadius][i + kernelRadius] = expf(-distSq / (2.0f * sigma * sigma));
			kernelSum += kernel[j + kernelRadius][i + kernelRadius];
		}
	}
	for (j = 0; j <= kernelRadius * 2; j++) {
		for (i = 0; i <= kernelRadius * 2; i++) {
			kernel[j][i] /= kernelSum;
		}
	}

	int numPixels = numLightBytes / 3;
	tempFloats = malloc(numPixels * sizeof(float) * 3);
	if (!tempFloats) return;
	memcpy(tempFloats, lightFloats, numPixels * sizeof(float) * 3);

	// --- Single Pass Blur (2D for Planar/Patch, 3D for Trisoups) ---
    int progress = 0;
	#pragma omp parallel for schedule(dynamic, 1) private(s, ds, y, x, i, j, p)
	for (s = 0; s < numPlanarSurfaces; s++) {
		ds = &drawSurfaces[planarSurfaces[s].surfaceNum];

		for (y = 0; y < ds->lightmapHeight; y++) {
			for (x = 0; x < ds->lightmapWidth; x++) {
				p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
				if (lightAlphaMask[p] == 0) continue;
				
				float sumColor[3] = {0,0,0}, sumWeight = 0.0f;

				for (j = -kernelRadius; j <= kernelRadius; j++) {
					for (i = -kernelRadius; i <= kernelRadius; i++) {
						float weight = kernel[j + kernelRadius][i + kernelRadius];
						float sampleColor[3];
						
						if (GetFilteredTexel(s, (float)(x + i), (float)(y + j), sampleColor, tempFloats)) {
							VectorMA(sumColor, weight, sampleColor, sumColor);
							sumWeight += weight;
						}
					}
				}
				
				if (sumWeight > 0.0001f) {
					VectorScale(sumColor, 1.0f / sumWeight, &lightFloats[p * 3]);
				}
			}
		}
        int currentProgress;
        #pragma omp atomic capture
        currentProgress = ++progress;

        if (numPlanarSurfaces >= 10) {
            int oldPercent = ((currentProgress - 1) * 10) / numPlanarSurfaces;
            int newPercent = (currentProgress * 10) / numPlanarSurfaces;
            if (newPercent > oldPercent) {
                ThreadLock();
                _printf(".");
                ThreadUnlock();
            }
        }
	}

	// 3D VPPS Blur for Triangle Soups
    progress = 0;
	#pragma omp parallel for schedule(dynamic, 1) private(s)
    for (s = 0; s < numDrawSurfaces; s++) {
        ProcessTrisoupVolumetric(s, radius, tempFloats, qfalse);

        int currentProgress;
        #pragma omp atomic capture
        currentProgress = ++progress;

        if (numDrawSurfaces >= 10) {
            int oldPercent = ((currentProgress - 1) * 10) / numDrawSurfaces;
            int newPercent = (currentProgress * 10) / numDrawSurfaces;
            if (newPercent > oldPercent) {
                ThreadLock();
                _printf(".");
                ThreadUnlock();
            }
        }
    }

	free(tempFloats);
}

void PostProcessLightmaps(void) {
	_printf("--- Post Processing ---\n");
	ScanLightmapIntensity();

	BuildPlanarSurfaceIndex();

	if (lightmapAA) {
		_printf("Applying Anti-Aliasing pass (Mode %d)...\n", lightmapAA);
		AntiAliasLightmaps();
	}

	if (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) {
		_printf("Smoothing (%d passes, radius %.2f):", lightmapSmoothPasses, lightmapSmoothRadius);
		for (int pnum = 1; pnum <= lightmapSmoothPasses; pnum++) {
			_printf(" %d", pnum);
			SmoothLightmaps(lightmapSmoothRadius);
		}
		_printf(" Done\n");
	}

	FreePlanarSurfaceIndex();
}
