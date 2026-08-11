#ifndef __LIGHTDATA_H__
#define __LIGHTDATA_H__

#include "mathlib.h"
#include "qfiles.h"

typedef struct {
  vec3_t dir;
  vec3_t irradiance;
  float angle;
  float ambientClampScale;
  qboolean isGlow;
} contribution_t;

// High-precision vertex for internal lighting
typedef struct {
	vec3_t		xyz;
	float		st[2];
	float		lightmap[4][2];
	vec3_t		normal;
	float		color[4][3]; // RGB-only for internal lighting
} drawVert32_t;

// High-precision grid point for internal lighting
typedef struct {
    vec3_t      ambient[4];
    vec3_t      directed[4];
    vec3_t      dir;
    byte        styles[4];
} bspGridPoint32_t;

// High-precision mesh for internal lighting
typedef struct {
	int				width, height;
	drawVert32_t	*verts;
} mesh32_t;

// 32-bit floating-point lighting data
extern drawVert32_t *internalDrawVerts;
extern float *lightFloats;
extern float *deluxeFloats;
extern float *energyFloats;
extern float *normalFloats;
extern float *radiosityFloats;
extern float *accumRadiosityFloats;
extern float *radiosityVertexFloats;
extern float *radiosityGridColors;
extern float *radiosityDeluxeFloats;
extern float *radiosityEnergyFloats;
extern float *accumRadiosityDeluxeSum;
extern float *accumRadiosityEnergyFloats;
extern byte *lightAlphaMask;
extern byte *unreachableMask;

#define BITMAP_SET(map, i)   ((map)[(i) >> 3] |=  (1 << ((i) & 7)))
#define BITMAP_CLEAR(map, i) ((map)[(i) >> 3] &= ~(1 << ((i) & 7)))
#define BITMAP_TEST(map, i)  ((map)[(i) >> 3] &   (1 << ((i) & 7)))


typedef struct {
    vec3_t pos;
    vec3_t normal;
    int    pixelIndex;
} voxelPoint_t;

// Voxel Cache Service
void VoxelCache_BakeAll(void);
voxelPoint_t *VoxelCache_Load(int surfIdx, int *outNumPoints);
void MergeAccumulatedState(vec3_t color, vec3_t dir, vec3_t energy,
                           const vec3_t addColor, const vec3_t addDir,
                           const vec3_t addEnergy, const vec3_t normal, float deluxeMinAngle);
void AccumulateContribution(vec3_t color, vec3_t dir, vec3_t energy, const contribution_t *cont, const vec3_t normal, float deluxeMinAngle);
extern bspGridPoint32_t *gridData32;

extern float maxLightIntensity;

// Up-conversion: Standard 8-bit data -> Internal 32-bit float buffers
void UpConvertLightingData(void);
void CheckGridData32(void);

// Down-conversion: Internal 32-bit float buffers -> Standard 8-bit data
void DownConvertLightingData(void);
void AllocateRadiosityFloats(void);
void FreeRadiosityFloats(void);

// Scaling logic
void ScanLightmapIntensity(void);
void InternalColorToBytesScaled(const float *color, byte *colorBytes, float scale, qboolean sRGB);

// Canonical Color -> Byte conversion
void InternalColorToBytes(const float *color, byte *colorBytes, qboolean sRGB);

// High-precision lerp functions
void LerpDrawVert32(drawVert32_t *a, drawVert32_t *b, drawVert32_t *out);
void LerpDrawVertAmount32(drawVert32_t *a, drawVert32_t *b, float amount, drawVert32_t *out);

// High-precision mesh functions (cloned from shared/mesh.c)
void FreeMesh32(mesh32_t *m);
mesh32_t *CopyMesh32(mesh32_t *mesh);
mesh32_t *TransposeMesh32(mesh32_t *in);
void MakeMeshNormals32(mesh32_t in);
void PutMeshOnCurve32(mesh32_t in);
mesh32_t *SubdivideMesh32(mesh32_t in, float maxError, float minLength);
mesh32_t *SubdivideMeshQuads32(mesh32_t *in, float minLength, int maxsize, int widthtable[], int heighttable[]);
mesh32_t *RemoveLinearMeshColumnsRows32(mesh32_t *in);

#endif
