/*
================
trisoup_filter.cl

3D Gaussian blur for Triangle Soup lightmap texels, using Density Normalization
to prevent shadow displacement caused by uneven UV packing (XAtlas).

Pass 1 (trisoup_density): Estimates the local texel density for each point.
Pass 2 (trisoup_filter): Gathers colors, dividing each neighbor's weight by its density.

Uses a CSR (Compressed Sparse Row) voxel grid to limit the search.
Replaces the hard angular cutoff with a smooth angular taper to prevent corner artifacts.
================
*/

__kernel void trisoup_density(
    __global const float *texelPos,
    __global const float *texelNormal,
    __global const int   *bucketStart,
    __global const int   *bucketCount,
    __global const int   *sortedTexels,
    __global       float *density,
    float gridMinX, float gridMinY, float gridMinZ,
    float voxelSize,
    int   gridDimX,  int   gridDimY,  int   gridDimZ,
    float maxDistSq,
    float twoSigmaSq,
    float angleMatchCos,
    int   N)
{
    int tid = get_global_id(0);
    if (tid >= N) return;

    int gStride1 = gridDimY * gridDimZ;
    int gStride2 = gridDimZ;

    float ox = texelPos[tid * 3 + 0];
    float oy = texelPos[tid * 3 + 1];
    float oz = texelPos[tid * 3 + 2];
    float nx = texelNormal[tid * 3 + 0];
    float ny = texelNormal[tid * 3 + 1];
    float nz = texelNormal[tid * 3 + 2];

    int vx = (int)((ox - gridMinX) / voxelSize);
    int vy = (int)((oy - gridMinY) / voxelSize);
    int vz = (int)((oz - gridMinZ) / voxelSize);

    float localDensity = 0.0f;

    for (int dx = -1; dx <= 1; dx++) {
    for (int dy = -1; dy <= 1; dy++) {
    for (int dz = -1; dz <= 1; dz++) {
        int bx = vx + dx, by = vy + dy, bz = vz + dz;
        if (bx < 0 || bx >= gridDimX || by < 0 || by >= gridDimY || bz < 0 || bz >= gridDimZ) continue;

        int bucket = bx * gStride1 + by * gStride2 + bz;
        int start  = bucketStart[bucket];
        int count  = bucketCount[bucket];

        for (int j = 0; j < count; j++) {
            int other = sortedTexels[start + j];

            float cnx = texelNormal[other * 3 + 0];
            float cny = texelNormal[other * 3 + 1];
            float cnz = texelNormal[other * 3 + 2];
            float dot = nx*cnx + ny*cny + nz*cnz;

            // Soft angular falloff instead of hard cutoff
            float angleWeight = clamp((dot - angleMatchCos) / (1.0f - angleMatchCos), 0.0f, 1.0f);
            if (angleWeight <= 0.0f) continue;

            float ddx = ox - texelPos[other * 3 + 0];
            float ddy = oy - texelPos[other * 3 + 1];
            float ddz = oz - texelPos[other * 3 + 2];
            float distSq = ddx*ddx + ddy*ddy + ddz*ddz;
            if (distSq > maxDistSq) continue;

            float w = native_exp(-distSq / twoSigmaSq) * angleWeight;
            localDensity += w;
        }
    }}}

    // Prevent division by zero
    density[tid] = (localDensity > 0.0001f) ? localDensity : 1.0f;
}

__kernel void trisoup_filter(
    __global const float *texelPos,
    __global const float *texelNormal,
    __global const float *jitterPos,
    __global const float *jitterNormal,
    __global const uchar *jitterValid,
    __global const float *texelColor,
    __global const float *density,
    __global const int   *bucketStart,
    __global const int   *bucketCount,
    __global const int   *sortedTexels,
    __global       float *output,
    __global const int   *validList,
    float gridMinX, float gridMinY, float gridMinZ,
    float voxelSize,
    int   gridDimX,  int   gridDimY,  int   gridDimZ,
    float maxDistSq,
    float twoSigmaSq,
    float angleMatchCos,
    int   numSamples,
    int   N,
    __global const float *texelDir,
    __global       float *outputDir,
    __global const float *texelNrm,
    __global       float *outputNrm)
{
    int tid = get_global_id(0);
    if (tid >= N) return;

    int gStride1 = gridDimY * gridDimZ;
    int gStride2 = gridDimZ;

    float finalR = 0.0f, finalG = 0.0f, finalB = 0.0f;
    float finalDx = 0.0f, finalDy = 0.0f, finalDz = 0.0f;
    float finalNx = 0.0f, finalNy = 0.0f, finalNz = 0.0f;
    float finalWeight = 0.0f, finalDWeight = 0.0f;

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
        float totalDx = 0.0f, totalDy = 0.0f, totalDz = 0.0f;
        float totalNx = 0.0f, totalNy = 0.0f, totalNz = 0.0f;
        float totalWeight = 0.0f, totalDWeight = 0.0f;

        for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -1; dz <= 1; dz++) {
            int bx = vx + dx, by = vy + dy, bz = vz + dz;
            if (bx < 0 || bx >= gridDimX || by < 0 || by >= gridDimY || bz < 0 || bz >= gridDimZ) continue;

            int bucket = bx * gStride1 + by * gStride2 + bz;
            int start  = bucketStart[bucket];
            int count  = bucketCount[bucket];

            for (int j = 0; j < count; j++) {
                int other = sortedTexels[start + j];

                float cnx = texelNormal[other * 3 + 0];
                float cny = texelNormal[other * 3 + 1];
                float cnz = texelNormal[other * 3 + 2];
                float dot = nx*cnx + ny*cny + nz*cnz;

                // Soft angular falloff
                float angleWeight = clamp((dot - angleMatchCos) / (1.0f - angleMatchCos), 0.0f, 1.0f);
                if (angleWeight <= 0.0f) continue;

                float ddx = ox - texelPos[other * 3 + 0];
                float ddy = oy - texelPos[other * 3 + 1];
                float ddz = oz - texelPos[other * 3 + 2];
                float distSq = ddx*ddx + ddy*ddy + ddz*ddz;
                if (distSq > maxDistSq) continue;

                float w = native_exp(-distSq / twoSigmaSq) * angleWeight;
                
                // DENSITY NORMALIZATION
                float w_norm = w / density[other];
                
                float cR = texelColor[other * 3 + 0];
                float cG = texelColor[other * 3 + 1];
                float cB = texelColor[other * 3 + 2];

                totalR += cR * w_norm;
                totalG += cG * w_norm;
                totalB += cB * w_norm;
                totalWeight += w_norm;
                
                float lum = 0.2126f * cR + 0.7152f * cG + 0.0722f * cB;
                float dw = w_norm * lum;
                
                if (texelDir) {
                    totalDx += texelDir[other * 3 + 0] * dw;
                    totalDy += texelDir[other * 3 + 1] * dw;
                    totalDz += texelDir[other * 3 + 2] * dw;
                }
                if (texelNrm) {
                    totalNx += texelNrm[other * 3 + 0] * dw;
                    totalNy += texelNrm[other * 3 + 1] * dw;
                    totalNz += texelNrm[other * 3 + 2] * dw;
                }
                totalDWeight += dw;
            }
        }}}

        if (totalWeight > 0.0001f) {
            finalR += totalR / totalWeight;
            finalG += totalG / totalWeight;
            finalB += totalB / totalWeight;
            finalWeight += 1.0f;
            
            if (texelDir) {
                if (totalDWeight > 0.0001f) {
                    float len = sqrt(totalDx*totalDx + totalDy*totalDy + totalDz*totalDz);
                    if (len > 0.001f) { finalDx += totalDx/len; finalDy += totalDy/len; finalDz += totalDz/len; }
                    else { finalDx += texelDir[tid * 3 + 0]; finalDy += texelDir[tid * 3 + 1]; finalDz += texelDir[tid * 3 + 2]; }
                }
            }
            if (texelNrm) {
                if (totalDWeight > 0.0001f) {
                    float len = sqrt(totalNx*totalNx + totalNy*totalNy + totalNz*totalNz);
                    if (len > 0.001f) { finalNx += totalNx/len; finalNy += totalNy/len; finalNz += totalNz/len; }
                    else { finalNx += texelNrm[tid * 3 + 0]; finalNy += texelNrm[tid * 3 + 1]; finalNz += texelNrm[tid * 3 + 2]; }
                }
            }
            finalDWeight += 1.0f;
        }
    }

    if (finalWeight > 0.0001f) {
        int outIdx = tid * 3;
        output[outIdx + 0] = finalR / finalWeight;
        output[outIdx + 1] = finalG / finalWeight;
        output[outIdx + 2] = finalB / finalWeight;
        
        if (texelDir && outputDir) {
            if (finalDWeight > 0.0001f) {
                float len = sqrt(finalDx*finalDx + finalDy*finalDy + finalDz*finalDz);
                if (len > 0.001f) {
                    outputDir[outIdx + 0] = finalDx / len;
                    outputDir[outIdx + 1] = finalDy / len;
                    outputDir[outIdx + 2] = finalDz / len;
                }
            }
        }
        if (texelNrm && outputNrm) {
            if (finalDWeight > 0.0001f) {
                float len = sqrt(finalNx*finalNx + finalNy*finalNy + finalNz*finalNz);
                if (len > 0.001f) {
                    outputNrm[outIdx + 0] = finalNx / len;
                    outputNrm[outIdx + 1] = finalNy / len;
                    outputNrm[outIdx + 2] = finalNz / len;
                }
            }
        }
    }
}
