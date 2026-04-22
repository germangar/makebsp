#include "light.h"
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
// Voxel Grid Structure for Anti-Aliasing Triangle Soups (VPPS Spatial Hash)
// ---------------------------------------------------------------------------

extern int samplesize;
#define AA_ANGLE_MATCH_COS 0.85f

typedef struct aaTexel_s {
    vec3_t pos;
    vec3_t normal;
    vec3_t color;
    struct aaTexel_s *next;
} aaTexel_t;

static aaTexel_t ***g_aaVoxels = NULL;
static vec3_t g_aaVoxelMins;
static int g_aaVoxelDims[3];
static float g_aaVoxelSize = 16.0f;

static void AAVoxelReset(void) {
    int i;
    if (g_aaVoxels) {
        for (int x = 0; x < g_aaVoxelDims[0]; x++) {
            for (int y = 0; y < g_aaVoxelDims[1]; y++) {
                for (int z = 0; z < g_aaVoxelDims[2]; z++) {
                    aaTexel_t *v = g_aaVoxels[x][y][z].next;
                    while(v) {
                        aaTexel_t *next = v->next;
                        free(v);
                        v = next;
                    }
                }
                free(g_aaVoxels[x][y]);
            }
            free(g_aaVoxels[x]);
        }
        free(g_aaVoxels);
        g_aaVoxels = NULL;
    }

    g_aaVoxelSize = (float)samplesize * 1.5f;
    if (g_aaVoxelSize < 4.0f) g_aaVoxelSize = 4.0f;

    aa_angle_match_cos = cosf(DEG2RAD(AA_ANGLE_MATCH_DEGREES));

    // Initialize dimensions based on map bounds
    for (i = 0; i < 3; i++) {
        g_aaVoxelMins[i] = dmodels[0].mins[i] - g_aaVoxelSize;
        float size = (dmodels[0].maxs[i] + g_aaVoxelSize) - g_aaVoxelMins[i];
        g_aaVoxelDims[i] = (int)ceil(size / g_aaVoxelSize);
    }

    g_aaVoxels = malloc(sizeof(aaTexel_t**) * g_aaVoxelDims[0]);
    for (int x = 0; x < g_aaVoxelDims[0]; x++) {
        g_aaVoxels[x] = malloc(sizeof(aaTexel_t*) * g_aaVoxelDims[1]);
        for (int y = 0; y < g_aaVoxelDims[1]; y++) {
            g_aaVoxels[x][y] = calloc(g_aaVoxelDims[2], sizeof(aaTexel_t));
        }
    }
}

static void AAVoxelAdd(const vec3_t pos, const vec3_t normal, const float *color) {
    int v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = (int)((pos[i] - g_aaVoxelMins[i]) / g_aaVoxelSize);
        if (v[i] < 0 || v[i] >= g_aaVoxelDims[i]) return;
    }

    aaTexel_t *newT = malloc(sizeof(aaTexel_t));
    VectorCopy(pos, newT->pos);
    VectorCopy(normal, newT->normal);
    newT->color[0] = color[0];
    newT->color[1] = color[1];
    newT->color[2] = color[2];
    
    // Prepend to list
    newT->next = g_aaVoxels[v[0]][v[1]][v[2]].next;
    g_aaVoxels[v[0]][v[1]][v[2]].next = newT;
}

static qboolean AAVoxelSample(const vec3_t pos, const vec3_t normal, float searchRadius, vec3_t outColor) {
    int v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = (int)((pos[i] - g_aaVoxelMins[i]) / g_aaVoxelSize);
        if (v[i] < 0 || v[i] >= g_aaVoxelDims[i]) return qfalse;
    }

    vec3_t totalColor = {0,0,0};
    float totalWeight = 0.0f;
    float maxDistSq = searchRadius * searchRadius;

    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                int nx = v[0] + dx, ny = v[1] + dy, nz = v[2] + dz;
                if (nx < 0 || nx >= g_aaVoxelDims[0] || ny < 0 || ny >= g_aaVoxelDims[1] || nz < 0 || nz >= g_aaVoxelDims[2]) continue;
                
                aaTexel_t *curr = g_aaVoxels[nx][ny][nz].next;
                while (curr) {
                    float dot = DotProduct(curr->normal, normal);
                    if (dot > aa_angle_match_cos) {
                        vec3_t delta;
                        VectorSubtract(pos, curr->pos, delta);
                        float distSq = DotProduct(delta, delta);
                        if (distSq <= maxDistSq) {
                            float w = (1.0f / (sqrtf(distSq) + 0.1f)) * dot;
                            VectorMA(totalColor, w, curr->color, totalColor);
                            totalWeight += w;
                        }
                    }
                    curr = curr->next;
                }
            }
        }
    }

    if (totalWeight < 0.0001f) return qfalse;

    VectorScale(totalColor, 1.0f / totalWeight, outColor);
    return qtrue;
}

static void AAVoxelize(void) {
    int s, x, y, p;
    dsurface_t *ds;
    
    AAVoxelReset();

    for (s = 0; s < numDrawSurfaces; s++) {
        ds = &drawSurfaces[s];
        if (ds->lightmapNum[0] < 0 || ds->surfaceType != MST_TRIANGLE_SOUP) continue;

        for (y = 0; y < ds->lightmapHeight; y++) {
            for (x = 0; x < ds->lightmapWidth; x++) {
                p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
                if (lightAlphaMask[p] == 0) continue;

                float st[2];
                vec3_t origin, normal;
                st[0] = (float)ds->lightmapOffset[0][0] + (float)x + 0.5f;
                st[1] = (float)ds->lightmapOffset[0][1] + (float)y + 0.5f;

                if (TriSoupSamplePoint(ds, st, origin, normal)) {
                    AAVoxelAdd(origin, normal, &lightFloats[p * 3]);
                }
            }
        }
    }
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

    // AA for Triangle Soups
    #pragma omp parallel for private(s, ds, x, y, p)
    for (s = 0; s < numDrawSurfaces; s++) {
        ds = &drawSurfaces[s];
        if (ds->lightmapNum[0] < 0 || ds->surfaceType != MST_TRIANGLE_SOUP) continue;

        for (y = 0; y < ds->lightmapHeight; y++) {
            for (x = 0; x < ds->lightmapWidth; x++) {
                p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
                if (lightAlphaMask[p] == 0) continue;

                float st[2];
                vec3_t origin, normal;
                st[0] = (float)ds->lightmapOffset[0][0] + (float)x + 0.5f;
                st[1] = (float)ds->lightmapOffset[0][1] + (float)y + 0.5f;

                if (TriSoupSamplePoint(ds, st, origin, normal)) {
                    vec3_t sampleColor;
                    float searchRadius = radius * (float)samplesize * 1.5f;
                    if (searchRadius < 4.0f) searchRadius = 4.0f;
                    if (AAVoxelSample(origin, normal, searchRadius, sampleColor)) {
                        VectorCopy(sampleColor, &lightFloats[p * 3]);
                    }
                }
            }
        }
    }

	if (lightmapAA == 1) {
		#pragma omp parallel for private(s, ds, x, y, p, k)
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
		}
	} else if (lightmapAA == 2) {
		#pragma omp parallel for private(s, ds, x, y, p, k)
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
						mask2x[Y * W2 + X] = ALPHA_SMOOTH;
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
						blurMask2x[Y * W2 + X] = ALPHA_SMOOTH;
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
		}
	}
	free(tempFloats);
	AAVoxelReset();
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

	// Ensure the voxel grid is up to date with current lightFloats for Trisoups
	AAVoxelize();

	// --- Single Pass Blur (2D for Planar/Patch, 3D for Trisoups) ---
	#pragma omp parallel for private(s, ds, y, x, i, j, p)
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
	}

	// 3D VPPS Blur for Triangle Soups
	#pragma omp parallel for private(s, ds, x, y, p)
    for (s = 0; s < numDrawSurfaces; s++) {
        ds = &drawSurfaces[s];
        if (ds->lightmapNum[0] < 0 || ds->surfaceType != MST_TRIANGLE_SOUP) continue;

        for (y = 0; y < ds->lightmapHeight; y++) {
            for (x = 0; x < ds->lightmapWidth; x++) {
                p = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + y) * LIGHTMAP_WIDTH + ds->lightmapOffset[0][0] + x;
                if (lightAlphaMask[p] == 0) continue;

                float st[2];
                vec3_t origin, normal;
                st[0] = (float)ds->lightmapOffset[0][0] + (float)x + 0.5f;
                st[1] = (float)ds->lightmapOffset[0][1] + (float)y + 0.5f;

                if (TriSoupSamplePoint(ds, st, origin, normal)) {
                    vec3_t sampleColor;
                    float searchRadius = radius * (float)samplesize;
                    if (searchRadius < g_aaVoxelSize) searchRadius = g_aaVoxelSize;
                    if (AAVoxelSample(origin, normal, searchRadius, sampleColor)) {
                        VectorCopy(sampleColor, &lightFloats[p * 3]);
                    }
                }
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
		// VPPS voxelization is now handled inside AAVoxelize, called explicitly here for AA pass
		_printf("  [vpps] Voxelizing Trisoups...\n");
		AAVoxelize();
		AntiAliasLightmaps();
		AAVoxelReset(); // Free grid after AA if Smoothing doesn't need it immediately
	}

	if (lightmapSmoothPasses > 0 && lightmapSmoothRadius > 0.0f) {
		_printf("Smoothing (%d passes, radius %.2f): ", lightmapSmoothPasses, lightmapSmoothRadius);
		for (int pnum = 1; pnum <= lightmapSmoothPasses; pnum++) {
			_printf("%d...", pnum);
			SmoothLightmaps(lightmapSmoothRadius);
		}
		AAVoxelReset(); // Final free after all smooth passes
		_printf(" Done\n");
	}

	FreePlanarSurfaceIndex();
}
