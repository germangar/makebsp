#pragma once
#include "MRMeshFwd.h"
#include "MRVector3.h"

MR_EXTERN_C_BEGIN

typedef struct MRPlane3f {
    MRVector3f n;
    float d;
} MRPlane3f;

// Trims a mesh with a plane. Leaves only the part in the positive normal direction.
MRMESHC_API void mrTrimMeshWithPlane( MRMesh* mesh, const MRPlane3f* plane );

// Performs iterative trimming, UV/Color interpolation, and packing for a miscModelMesh.
// Automatically duplicates non-manifold vertices, slices them, and returns packed arrays.
MRMESHC_API void mrTrimMiscModelMesh(
    const float* inPositions, int numVerts,
    const float* inNormals,  // Expected 3 floats per vert, can be NULL
    const float* inUVs,      // Expected 2 floats per vert, can be NULL
    const unsigned char* inColors, // Expected 4 bytes per vert, can be NULL
    const int* inIndices, int numIndices,
    
    const MRPlane3f* planes, int numPlanes,
    
    float** outPositions,
    float** outNormals,
    float** outUVs,
    unsigned char** outColors,
    int** outIndices,
    int* outNumVerts,
    int* outNumIndices
);

MR_EXTERN_C_END
