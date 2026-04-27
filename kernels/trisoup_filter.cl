/*
================
trisoup_filter.cl

3D Gaussian blur for Triangle Soup lightmap texels.

Uses a CSR (Compressed Sparse Row) voxel grid to limit the search
to a 3x3x3 neighbourhood in world space, exactly matching the CPU logic.
Each thread processes one valid texel and iterates over 1 (smooth) or
8 (AA) jitter sample positions.

All math is identical to ProcessTrisoupVolumetric in lm_postprocess.c.

lm_common.cl is prepended by BuildOpenCLProgramWithCommon() — GpuPlanarSurface
and gpu_sample_masked_bilinear are available but not used by this kernel
(it operates in 3D world space, not 2D image space).
================
*/

__kernel void trisoup_filter(
    __global const float *texelPos,      // [N*3]           centre world positions
    __global const float *texelNormal,   // [N*3]           centre normals
    __global const float *jitterPos,     // [N*numSamples*3] world pos per sample
    __global const float *jitterNormal,  // [N*numSamples*3] normal per sample
    __global const uchar *jitterValid,   // [N*numSamples]   1 = valid sample
    __global const float *texelColor,    // [N*3]            input colours
    __global const int   *bucketStart,   // [numBuckets]     CSR start index
    __global const int   *bucketCount,   // [numBuckets]     CSR entry count
    __global const int   *sortedTexels,  // [N]              texel IDs sorted by bucket
    __global       float *output,        // [totalAtlasPixels*3]
    __global const int   *validList,     // [N]              flat atlas index per texel
    float gridMinX, float gridMinY, float gridMinZ,
    float voxelSize,
    int   gridDimX,  int   gridDimY,  int   gridDimZ,
    float maxDistSq,
    float twoSigmaSq,
    float angleMatchCos,
    int   numSamples,
    int   N)
{
    int tid = get_global_id(0);
    if (tid >= N) return;

    int gStride1 = gridDimY * gridDimZ;
    int gStride2 = gridDimZ;

    float finalR = 0.0f, finalG = 0.0f, finalB = 0.0f;
    float finalWeight = 0.0f;

    for (int k = 0; k < numSamples; k++) {
        int sIdx = tid * numSamples + k;
        if (!jitterValid[sIdx]) continue;

        float ox = jitterPos[sIdx * 3 + 0];
        float oy = jitterPos[sIdx * 3 + 1];
        float oz = jitterPos[sIdx * 3 + 2];
        float nx = jitterNormal[sIdx * 3 + 0];
        float ny = jitterNormal[sIdx * 3 + 1];
        float nz = jitterNormal[sIdx * 3 + 2];

        int vx = (int)((ox - gridMinX) / voxelSize);
        int vy = (int)((oy - gridMinY) / voxelSize);
        int vz = (int)((oz - gridMinZ) / voxelSize);

        float totalR = 0.0f, totalG = 0.0f, totalB = 0.0f;
        float totalWeight = 0.0f;

        for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -1; dz <= 1; dz++) {
            int bx = vx + dx, by = vy + dy, bz = vz + dz;
            if (bx < 0 || bx >= gridDimX ||
                by < 0 || by >= gridDimY ||
                bz < 0 || bz >= gridDimZ) continue;

            int bucket = bx * gStride1 + by * gStride2 + bz;
            int start  = bucketStart[bucket];
            int count  = bucketCount[bucket];

            for (int j = 0; j < count; j++) {
                int other = sortedTexels[start + j];

                float cnx = texelNormal[other * 3 + 0];
                float cny = texelNormal[other * 3 + 1];
                float cnz = texelNormal[other * 3 + 2];
                float dot = nx*cnx + ny*cny + nz*cnz;
                if (dot <= angleMatchCos) continue;

                float ddx = ox - texelPos[other * 3 + 0];
                float ddy = oy - texelPos[other * 3 + 1];
                float ddz = oz - texelPos[other * 3 + 2];
                float distSq = ddx*ddx + ddy*ddy + ddz*ddz;
                if (distSq > maxDistSq) continue;

                float w = native_exp(-distSq / twoSigmaSq) * dot;
                totalR += texelColor[other * 3 + 0] * w;
                totalG += texelColor[other * 3 + 1] * w;
                totalB += texelColor[other * 3 + 2] * w;
                totalWeight += w;
            }
        }}}

        if (totalWeight > 0.0001f) {
            finalR += totalR / totalWeight;
            finalG += totalG / totalWeight;
            finalB += totalB / totalWeight;
            finalWeight += 1.0f;
        }
    }

    if (finalWeight > 0.0001f) {
        int atlasIdx = validList[tid] * 3;
        output[atlasIdx + 0] = finalR / finalWeight;
        output[atlasIdx + 1] = finalG / finalWeight;
        output[atlasIdx + 2] = finalB / finalWeight;
    }
}
