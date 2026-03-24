#ifndef XATLAS_C_H
#define XATLAS_C_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    xatlasChartType_Planar,
    xatlasChartType_Ortho,
    xatlasChartType_LSCM,
    xatlasChartType_Piecewise,
    xatlasChartType_Invalid
} xatlasChartType;

typedef struct
{
    uint32_t *faceArray;
    uint32_t atlasIndex;
    uint32_t faceCount;
    xatlasChartType type;
    uint32_t material;
} xatlasChart;

typedef struct
{
    int32_t atlasIndex;
    int32_t chartIndex;
    float uv[2];
    uint32_t xref;
} xatlasVertex;

typedef struct
{
    xatlasChart *chartArray;
    uint32_t *indexArray;
    xatlasVertex *vertexArray;
    uint32_t chartCount;
    uint32_t indexCount;
    uint32_t vertexCount;
} xatlasMesh;

typedef struct
{
    uint32_t *image;
    xatlasMesh *meshes;
    float *utilization;
    uint32_t width;
    uint32_t height;
    uint32_t atlasCount;
    uint32_t chartCount;
    uint32_t meshCount;
    float texelsPerUnit;
} xatlasAtlas;

typedef enum
{
    xatlasIndexFormat_UInt16,
    xatlasIndexFormat_UInt32
} xatlasIndexFormat;

typedef struct
{
    const void *vertexPositionData;
    const void *vertexNormalData;
    const void *vertexUvData;
    const void *indexData;
    const bool *faceIgnoreData;
    const uint32_t *faceMaterialData;
    const uint8_t *faceVertexCount;
    uint32_t vertexCount;
    uint32_t vertexPositionStride;
    uint32_t vertexNormalStride;
    uint32_t vertexUvStride;
    uint32_t indexCount;
    int32_t indexOffset;
    uint32_t faceCount;
    xatlasIndexFormat indexFormat;
    float epsilon;
} xatlasMeshDecl;

typedef struct
{
    const void *vertexUvData;
    const void *indexData;
    const uint32_t *faceMaterialData;
    uint32_t vertexCount;
    uint32_t vertexStride;
    uint32_t indexCount;
    int32_t indexOffset;
    xatlasIndexFormat indexFormat;
} xatlasUvMeshDecl;

typedef struct
{
    void (*paramFunc)(const float *, float *, uint32_t, const uint32_t *, uint32_t);
    float maxChartArea;
    float maxBoundaryLength;
    float normalDeviationWeight;
    float roundnessWeight;
    float straightnessWeight;
    float normalSeamWeight;
    float textureSeamWeight;
    float maxCost;
    uint32_t maxIterations;
    bool useInputMeshUvs;
    bool fixWinding;
} xatlasChartOptions;

typedef struct
{
    uint32_t maxChartSize;
    uint32_t padding;
    float texelsPerUnit;
    uint32_t resolution;
    bool bilinear;
    bool blockAlign;
    bool bruteForce;
    bool createImage;
    bool rotateChartsToAxis;
    bool rotateCharts;
} xatlasPackOptions;

typedef enum
{
    xatlasAddMeshError_Success,
    xatlasAddMeshError_Error,
    xatlasAddMeshError_IndexOutOfRange,
    xatlasAddMeshError_InvalidFaceVertexCount,
    xatlasAddMeshError_InvalidIndexCount
} xatlasAddMeshError;

typedef enum
{
    xatlasProgressCategory_AddMesh,
    xatlasProgressCategory_ComputeCharts,
    xatlasProgressCategory_PackCharts,
    xatlasProgressCategory_BuildOutputMeshes
} xatlasProgressCategory;

typedef bool (*xatlasProgressFunc)(xatlasProgressCategory category, int progress, void *userData);
typedef void *(*xatlasReallocFunc)(void *, size_t);
typedef void (*xatlasFreeFunc)(void *);
typedef int (*xatlasPrintFunc)(const char *, ...);

xatlasAtlas *xatlasCreate();
void xatlasDestroy(xatlasAtlas *atlas);
xatlasAddMeshError xatlasAddMesh(xatlasAtlas *atlas, const xatlasMeshDecl *meshDecl, uint32_t meshCountHint);
void xatlasAddMeshJoin(xatlasAtlas *atlas);
xatlasAddMeshError xatlasAddUvMesh(xatlasAtlas *atlas, const xatlasUvMeshDecl *decl);
void xatlasComputeCharts(xatlasAtlas *atlas, const xatlasChartOptions *chartOptions);
void xatlasPackCharts(xatlasAtlas *atlas, const xatlasPackOptions *packOptions);
void xatlasGenerate(xatlasAtlas *atlas, const xatlasChartOptions *chartOptions, const xatlasPackOptions *packOptions);
void xatlasSetProgressCallback(xatlasAtlas *atlas, xatlasProgressFunc progressFunc, void *progressUserData);
void xatlasSetAlloc(xatlasReallocFunc reallocFunc, xatlasFreeFunc freeFunc);
void xatlasSetPrint(xatlasPrintFunc print, bool verbose);
const char *xatlasAddMeshErrorString(xatlasAddMeshError error);
const char *xatlasProgressCategoryString(xatlasProgressCategory category);
void xatlasMeshDeclInit(xatlasMeshDecl *meshDecl);
void xatlasUvMeshDeclInit(xatlasUvMeshDecl *uvMeshDecl);
void xatlasChartOptionsInit(xatlasChartOptions *chartOptions);
void xatlasPackOptionsInit(xatlasPackOptions *packOptions);

#ifdef __cplusplus
}
#endif

#endif
