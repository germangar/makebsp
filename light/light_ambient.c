#include "light.h"

/*
================
StratifiedCosineDir

Generates the i-th stratified cosine-weighted hemisphere sample.

The hemisphere is divided into N equal solid-angle strata arranged
along the azimuth. Within each stratum, random jitter (seeded per
texel via randU/randV) perturbs both azimuth and elevation.

Cosine weighting (cosTheta = sqrt(1 - xi)) concentrates samples
near the zenith where lambertian contribution is highest, and
eliminated wasteful grazing-angle samples entirely.

The result is a direction in the UPPER hemisphere (z >= 0).
Callers must negate z for ground-hemisphere rays.
================
*/
#if 0
static void StratifiedCosineDir(int i, int N, float randU, float randV, vec3_t out)
{
    // Stratified azimuth: divide [0, 2pi) into N equal slices,
    // then jitter within the slice using randU in [0, 1).
    float sliceWidth = (2.0f * (float)M_PI) / (float)N;
    float phi = ((float)i + randU) * sliceWidth;

    // Cosine-weighted elevation: xi maps uniformly to cos^2-distributed
    // polar angle via the CDF inverse: cosTheta = sqrt(1 - xi)
    float xi = ((float)i + randV) / (float)N;  // stratified in [0,1)
    xi = xi - floorf(xi);                        // wrap to [0,1) for safety
    float cosTheta = sqrtf(1.0f - xi);           // cosine-weighted
    float sinTheta = sqrtf(1.0f - cosTheta * cosTheta);

    out[0] = cosf(phi) * sinTheta;
    out[1] = sinf(phi) * sinTheta;
    out[2] = cosTheta;  // always >= 0 (upper hemisphere)
}
#endif

static void StratifiedUniformDir(int i, int N, float randU, float randV, vec3_t out)
{
    float phi = 2.0f * (float)M_PI * (float)i / (float)N;
    phi += randU * 2.0f * (float)M_PI; 

    float xi = ((float)i + randV) / (float)N;  // stratified in [0,1)
    xi = xi - floorf(xi);                        // wrap to [0,1) for safety
    
    // Uniform distribution over hemisphere
    float cosTheta = 1.0f - xi;
    float sinTheta = sqrtf(1.0f - cosTheta * cosTheta);

    out[0] = cosf(phi) * sinTheta;
    out[1] = sinf(phi) * sinTheta;
    out[2] = cosTheta;  // always >= 0 (upper hemisphere)
}

/*
================
ComputeMAOPoint
================
*/
void ComputeMAOPoint(int num)
{
    int mod, x, y, z, i;
    vec3_t origin;
    traceWork_t *tw;
    float skyAccum[3]   = {0, 0, 0};
    float groundAccum[3] = {0, 0, 0};
    float skyWeight = 0.0f, groundWeight = 0.0f;
    int samples;
    
    vec3_t dirAccum = {0, 0, 0};
    float skyLum = skyColor[0] * 0.299f + skyColor[1] * 0.587f + skyColor[2] * 0.114f;
    float groundLum = groundColor[0] * 0.299f + groundColor[1] * 0.587f + groundColor[2] * 0.114f;

    if (!maoAmbient)
        return;

    mod = num;
    z   = mod / (gridBounds[0] * gridBounds[1]);
    mod -= z * (gridBounds[0] * gridBounds[1]);
    y   = mod / gridBounds[0];
    mod -= y * gridBounds[0];
    x   = mod;

    origin[0] = gridMins[0] + x * gridSize[0];
    origin[1] = gridMins[1] + y * gridSize[1];
    origin[2] = gridMins[2] + z * gridSize[2];

    if (PointInSolid(origin))
        return;

    tw = Q_Alloc(sizeof(traceWork_t));
    if (!tw) return;
    memset(tw, 0, sizeof(traceWork_t));
    tw->ignoreSurface = -1;

    samples = ambient_samples;

    // Per-point random seed for jitter — uses the grid index hashed with
    // a large prime so neighbouring grid points get uncorrelated jitter.
    unsigned int seed = (unsigned int)num * 2654435761u;
    float randU = (float)(seed & 0xFFFF) / 65536.0f;
    float randV = (float)((seed >> 16) & 0xFFFF) / 65536.0f;

    // Upper hemisphere (sky): stratified cosine-weighted
    int skyN = samples / 2;
    for (i = 0; i < skyN; i++)
    {
        vec3_t dir, end;
        trace_t trace;
        float jU = fmodf(randU + (float)i * 0.618034f, 1.0f);  // golden ratio shuffle
        float jV = fmodf(randV + (float)i * 0.381966f, 1.0f);

        StratifiedUniformDir(i, skyN, jU, jV, dir);  // dir.z >= 0

        end[0] = origin[0] + dir[0] * ambient_testradius;
        end[1] = origin[1] + dir[1] * ambient_testradius;
        end[2] = origin[2] + dir[2] * ambient_testradius;

        // Half-lambert weight: pow(NdotL * 0.5 + 0.5, 2.0)
        float NdotL = dir[2];
        float w = (NdotL * 0.5f + 0.5f);
        w = w * w;
        
        skyWeight += w;
        TraceLine(origin, end, &trace, qfalse, tw);
        if (!trace.passSolid)
        {
            skyAccum[0] += w * skyColor[0];
            skyAccum[1] += w * skyColor[1];
            skyAccum[2] += w * skyColor[2];
            VectorMA(dirAccum, w * skyLum, dir, dirAccum);
        }
    }

    // Lower hemisphere (ground): same strategy, flip z
    int groundN = samples - skyN;
    for (i = 0; i < groundN; i++)
    {
        vec3_t dir, end;
        trace_t trace;
        float jU = fmodf(randU + (float)i * 0.618034f + 0.5f, 1.0f);
        float jV = fmodf(randV + (float)i * 0.381966f + 0.5f, 1.0f);

        StratifiedUniformDir(i, groundN, jU, jV, dir);
        dir[2] = -dir[2];  // flip to lower hemisphere

        end[0] = origin[0] + dir[0] * ambient_testradius;
        end[1] = origin[1] + dir[1] * ambient_testradius;
        end[2] = origin[2] + dir[2] * ambient_testradius;

        // Half-lambert weight: pow(NdotL * 0.5 + 0.5, 2.0)
        float NdotL = -dir[2]; // Normal is down (0, 0, -1), dir is down
        float w = (NdotL * 0.5f + 0.5f);
        w = w * w;
        
        groundWeight += w;
        TraceLine(origin, end, &trace, qfalse, tw);
        if (!trace.passSolid)
        {
            groundAccum[0] += w * groundColor[0];
            groundAccum[1] += w * groundColor[1];
            groundAccum[2] += w * groundColor[2];
            VectorMA(dirAccum, w * groundLum, dir, dirAccum);
        }
    }

    free(tw);

    float skyOpen    = (skyWeight    > 0.0f) ? 1.0f / skyWeight    : 0.0f;
    float groundOpen = (groundWeight > 0.0f) ? 1.0f / groundWeight : 0.0f;

    float skyScale = skyOpen;
    float groundScale = groundOpen;
    
    if (ambient_color_scale > 0.0f) {
        skyScale /= ambient_color_scale;
        groundScale /= ambient_color_scale;
    }

    maoAmbient[num * 3 + 0] = skyAccum[0] * skyScale + groundAccum[0] * groundScale;
    maoAmbient[num * 3 + 1] = skyAccum[1] * skyScale + groundAccum[1] * groundScale;
    maoAmbient[num * 3 + 2] = skyAccum[2] * skyScale + groundAccum[2] * groundScale;

    if (maoDir)
    {
        float dirLen = VectorLength(dirAccum);
        if (dirLen > 0.0001f)
        {
            float invLen = 1.0f / dirLen;
            maoDir[num * 3 + 0] = dirAccum[0] * invLen;
            maoDir[num * 3 + 1] = dirAccum[1] * invLen;
            maoDir[num * 3 + 2] = dirAccum[2] * invLen;
        }
        else
        {
            maoDir[num * 3 + 0] = 0.0f;
            maoDir[num * 3 + 1] = 0.0f;
            maoDir[num * 3 + 2] = 1.0f;
        }
    }
}

/*
================
RunMAOPass
================
*/
void RunMAOPass(void)
{
    if (!ambient_enabled)
        return;

    if (numGridPoints <= 0)
        return;

    _printf("--- RunMAOPass (Ambient Pre-computation) ---\n");
    if (!maoAmbient)
    {
        maoAmbient = Q_Alloc(numGridPoints * 3 * sizeof(float));
        maoDir = Q_Alloc(numGridPoints * 3 * sizeof(float));
        if (!maoAmbient || !maoDir)
            Error("Failed to allocate MAO arrays");
    }

    RunThreadsOnIndividual(numGridPoints, qtrue, ComputeMAOPoint);
    _printf("\n");
}

/*
================
GatherAmbientAtPoint
================
*/
static qboolean GatherAmbientAtPoint(vec3_t origin, vec3_t normal, vec3_t ambColor, vec3_t bentAccum, traceWork_t *tw)
{
    float gatherRadius = ambient_gatheradius;
    float gatherRadiusSq = gatherRadius * gatherRadius;

    int ix_min = (int)((origin[0] - gatherRadius - gridMins[0]) / gridSize[0]);
    int ix_max = (int)((origin[0] + gatherRadius - gridMins[0]) / gridSize[0]);
    int iy_min = (int)((origin[1] - gatherRadius - gridMins[1]) / gridSize[1]);
    int iy_max = (int)((origin[1] + gatherRadius - gridMins[1]) / gridSize[1]);
    int iz_min = (int)((origin[2] - gatherRadius - gridMins[2]) / gridSize[2]);
    int iz_max = (int)((origin[2] + gatherRadius - gridMins[2]) / gridSize[2]);

    // clamp to bounds
    if (ix_min < 0) ix_min = 0; 
    if (ix_max > gridBounds[0] - 1) ix_max = gridBounds[0] - 1;
    
    if (iy_min < 0) iy_min = 0; 
    if (iy_max > gridBounds[1] - 1) iy_max = gridBounds[1] - 1;
    
    if (iz_min < 0) iz_min = 0; 
    if (iz_max > gridBounds[2] - 1) iz_max = gridBounds[2] - 1;

    VectorClear(ambColor);
    VectorClear(bentAccum);
    float totalWeight = 0.0f;
    
    for (int gz = iz_min; gz <= iz_max; gz++) {
        for (int gy = iy_min; gy <= iy_max; gy++) {
            for (int gx = ix_min; gx <= ix_max; gx++) {
                int gidx = (gz * gridBounds[1] + gy) * gridBounds[0] + gx;
                float *gCol = &maoAmbient[gidx * 3];
                
                float lum = gCol[0] * 0.299f + gCol[1] * 0.587f + gCol[2] * 0.114f;
                if (lum <= 0.0001f) continue; // Skip solid/black voxels

                vec3_t gPos;
                gPos[0] = gridMins[0] + gx * gridSize[0];
                gPos[1] = gridMins[1] + gy * gridSize[1];
                gPos[2] = gridMins[2] + gz * gridSize[2];

                vec3_t dir;
                VectorSubtract(gPos, origin, dir);
                
                float distSq = DotProduct(dir, dir);
                if (distSq > gatherRadiusSq) continue;
                if (distSq < 1.0f) distSq = 1.0f; // Prevent div by zero

                float dist = sqrtf(distSq);
                VectorScale(dir, 1.0f / dist, dir); // Normalize

                float NdotL = DotProduct(normal, dir);
                if (NdotL <= 0.01f) continue; // Behind surface

                float shadingModel = 1.0f - (distSq / gatherRadiusSq);
                
                float w = NdotL * shadingModel;

                // Trace ray
                trace_t trace;
                TraceLine(origin, gPos, &trace, qfalse, tw);
                if (!trace.passSolid) {
                    ambColor[0] += gCol[0] * w;
                    ambColor[1] += gCol[1] * w;
                    ambColor[2] += gCol[2] * w;
                    
                    bentAccum[0] += dir[0] * w * lum;
                    bentAccum[1] += dir[1] * w * lum;
                    bentAccum[2] += dir[2] * w * lum;
                }
                
                // Accumulate weight regardless of occlusion so blocked rays darken the final result
                totalWeight += w;
            }
        }
    }

    if (totalWeight > 0.0001f) {
        ambColor[0] /= totalWeight;
        ambColor[1] /= totalWeight;
        ambColor[2] /= totalWeight;
        return qtrue;
    }
    return qfalse;
}

/*
================
GatherGridAmbientBleed

Gathers environmental MAO bleed for grid cells.
Occluded neighbors are skipped entirely (do not darken the result).
================
*/
qboolean GatherGridAmbientBleed(vec3_t origin, vec3_t envColor, traceWork_t *tw)
{
    float gatherRadius = ambient_gatheradius;
    float gatherRadiusSq = gatherRadius * gatherRadius;

    int ix_min = (int)((origin[0] - gatherRadius - gridMins[0]) / gridSize[0]);
    int ix_max = (int)((origin[0] + gatherRadius - gridMins[0]) / gridSize[0]);
    int iy_min = (int)((origin[1] - gatherRadius - gridMins[1]) / gridSize[1]);
    int iy_max = (int)((origin[1] + gatherRadius - gridMins[1]) / gridSize[1]);
    int iz_min = (int)((origin[2] - gatherRadius - gridMins[2]) / gridSize[2]);
    int iz_max = (int)((origin[2] + gatherRadius - gridMins[2]) / gridSize[2]);

    // clamp to bounds
    if (ix_min < 0) ix_min = 0; 
    if (ix_max > gridBounds[0] - 1) ix_max = gridBounds[0] - 1;
    if (iy_min < 0) iy_min = 0; 
    if (iy_max > gridBounds[1] - 1) iy_max = gridBounds[1] - 1;
    if (iz_min < 0) iz_min = 0; 
    if (iz_max > gridBounds[2] - 1) iz_max = gridBounds[2] - 1;

    VectorClear(envColor);
    float totalWeight = 0.0f;
    
    for (int gz = iz_min; gz <= iz_max; gz++) {
        for (int gy = iy_min; gy <= iy_max; gy++) {
            for (int gx = ix_min; gx <= ix_max; gx++) {
                int gidx = (gz * gridBounds[1] + gy) * gridBounds[0] + gx;
                float *gCol = &maoAmbient[gidx * 3];
                
                // cull 1: Cheapest check. Skip black/solid neighbor voxels immediately.
                float lum = gCol[0] * 0.299f + gCol[1] * 0.587f + gCol[2] * 0.114f;
                if (lum <= 0.0001f) continue;

                vec3_t gPos;
                gPos[0] = gridMins[0] + gx * gridSize[0];
                gPos[1] = gridMins[1] + gy * gridSize[1];
                gPos[2] = gridMins[2] + gz * gridSize[2];

                vec3_t dir;
                VectorSubtract(gPos, origin, dir);
                float distSq = DotProduct(dir, dir);
                
                // cull 2: Sphere cull.
                if (distSq > gatherRadiusSq) continue;
                
                // cull 3: Skip self.
                if (distSq < 1.0f) continue;

                float w = 1.0f - (distSq / gatherRadiusSq);

                // cull 4: Trace (most expensive).
                trace_t trace;
                TraceLine(origin, gPos, &trace, qfalse, tw);
                if (trace.passSolid) continue; // Skip blocked; do NOT add to totalWeight.
                
                envColor[0] += gCol[0] * w;
                envColor[1] += gCol[1] * w;
                envColor[2] += gCol[2] * w;
                totalWeight += w;
            }
        }
    }

    if (totalWeight > 0.0001f) {
        envColor[0] /= totalWeight;
        envColor[1] /= totalWeight;
        envColor[2] /= totalWeight;
        return qtrue;
    }
    return qfalse;
}

/*
================
TraceAmbient
================
*/
void TraceAmbient(int num)
{
    int i, j, k;
    int realSurfIndex;
    dsurface_t *ds;
    int surfWeight;
    traceWork_t *tw;

    tw = Q_Alloc(sizeof(traceWork_t));
    if (!tw) return;
    memset(tw, 0, sizeof(traceWork_t));

    realSurfIndex = surfaceWorkOrder[num];
    ds = &drawSurfaces[realSurfIndex];
    shaderInfo_t *si = ShaderInfoForShader(dshaders[ds->shaderNum].shader);
    
    surfWeight = ds->numVerts;
    if (ds->lightmapNum[0] >= 0) {
        surfWeight += ds->lightmapWidth * ds->lightmapHeight;
    }

    // --- Vertex Ambient Pass ---
    for (i = 0; i < ds->numVerts; i++)
    {
        int vIdx = ds->firstVert + i;
        vec3_t origin, normal, ambColor, bentAccum;
        
        VectorCopy(drawVerts[vIdx].xyz, origin);
        VectorCopy(drawVerts[vIdx].normal, normal);
        
        VectorMA(origin, SAMPLE_NUDGE, normal, origin);
        
        if (GatherAmbientAtPoint(origin, normal, ambColor, bentAccum, tw))
        {
            if (internalDrawVerts)
            {
                internalDrawVerts[vIdx].color[0][0] += ambColor[0];
                internalDrawVerts[vIdx].color[0][1] += ambColor[1];
                internalDrawVerts[vIdx].color[0][2] += ambColor[2];
            }
        }
    }

    if (ds->lightmapNum[0] < 0)
    {
        free(tw);
        ThreadCompletedWeighted(surfWeight);
        return;
    }

    int sampleWidth = ds->lightmapWidth;
    int sampleHeight = ds->lightmapHeight;

    for (i = 0; i < sampleWidth; i++)
    {
        for (j = 0; j < sampleHeight; j++)
        {
            int py = ds->lightmapOffset[0][1] + j;
            int px = ds->lightmapOffset[0][0] + i;
            
            if (px < 0 || px >= LIGHTMAP_WIDTH || py < 0 || py >= LIGHTMAP_HEIGHT)
                continue;

            k = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + py) * LIGHTMAP_WIDTH + px;
            if (k < 0 || k >= numLightBytes / 3)
                continue;

            if (unreachableMask && BITMAP_TEST(unreachableMask, k))
                continue;

            int scale = upscale ? 2 : 1;
            int base_k_upscale = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT * scale + py * scale) * LIGHTMAP_WIDTH * scale + px * scale;

            vec3_t origin, normal;
            int found_valid = 0;

            for (int sy = 0; sy < scale; sy++)
            {
                for (int sx = 0; sx < scale; sx++)
                {
                    int k_up = base_k_upscale + sy * (LIGHTMAP_WIDTH * scale) + sx;
                    if (texelNormals[k_up][0] != 0.0f || texelNormals[k_up][1] != 0.0f || texelNormals[k_up][2] != 0.0f)
                    {
                        VectorCopy(texelOrigins[k_up], origin);
                        VectorCopy(texelNormals[k_up], normal);
                        found_valid = 1;
                        break;
                    }
                }
                if (found_valid) break;
            }

            if (!found_valid)
                continue;

            if (lightAlphaMask && lightAlphaMask[k] == 0)
            {
                lightAlphaMask[k] = ds->surfaceType;
            }

            vec3_t ambColor;
            vec3_t bentAccum;
            
            if (!GatherAmbientAtPoint(origin, normal, ambColor, bentAccum, tw))
                continue;

            float ambLum = ambColor[0] * 0.299f + ambColor[1] * 0.587f + ambColor[2] * 0.114f;
            if (ambLum < 0.0001f)
                continue;

            // Use bent surface normal as the ambient direction for deluxemap blending
            vec3_t bentNormal;
            if (VectorNormalize(bentAccum, bentNormal) < 0.0001f) {
                VectorCopy(normal, bentNormal);
            }

            // Exaggerate the ambient bent normal away from the surface normal
            if (deluxeAmbientExaggerate > 1.0f) {
                float w = DotProduct(normal, bentNormal);
                vec3_t tangent;
                
                // Extract tangent
                for (int c = 0; c < 3; c++) {
                    tangent[c] = bentNormal[c] - (normal[c] * w);
                }
                
                // Scale up the tangent to bend it further
                for (int c = 0; c < 3; c++) {
                    tangent[c] *= deluxeAmbientExaggerate;
                }
                
                // Reconstruct and re-normalize
                for (int c = 0; c < 3; c++) {
                    bentNormal[c] = (normal[c] * w) + tangent[c];
                }
                
                if (VectorNormalize(bentNormal, bentNormal) < 0.0001f) {
                    VectorCopy(normal, bentNormal);
                }

                // Clamp to maximum 45 degrees (cos(45) = 0.70710678f)
                float newW = DotProduct(normal, bentNormal);
                float maxCos = 0.70710678f; // Both sin(45) and cos(45)
                if (newW < maxCos) {
                    vec3_t pureTangent;
                    for (int c = 0; c < 3; c++) {
                        pureTangent[c] = bentNormal[c] - (normal[c] * newW);
                    }
                    if (VectorNormalize(pureTangent, pureTangent) > 0.0001f) {
                        for (int c = 0; c < 3; c++) {
                            bentNormal[c] = (normal[c] * maxCos) + (pureTangent[c] * maxCos);
                        }
                        VectorNormalize(bentNormal, bentNormal);
                    }
                }
            }

            if (lightFloats && deluxeFloats && energyFloats)
            {
                vec3_t existColor, existDir, existEnergy, normalizedBent;
                existColor[0] = lightFloats[k * 3 + 0];
                existColor[1] = lightFloats[k * 3 + 1];
                existColor[2] = lightFloats[k * 3 + 2];
                existDir[0]   = deluxeFloats[k * 3 + 0];
                existDir[1]   = deluxeFloats[k * 3 + 1];
                existDir[2]   = deluxeFloats[k * 3 + 2];
                existEnergy[0] = energyFloats[k * 3 + 0];
                existEnergy[1] = energyFloats[k * 3 + 1];
                existEnergy[2] = energyFloats[k * 3 + 2];

                if (VectorNormalize(bentNormal, normalizedBent) < 0.001f)
                    VectorCopy(normal, normalizedBent);

                MergeAccumulatedState(existColor, existDir, existEnergy,
                                      ambColor, normalizedBent, ambColor, normal, si->deluxeMinAngle);

                deluxeFloats[k * 3 + 0] = existDir[0];
                deluxeFloats[k * 3 + 1] = existDir[1];
                deluxeFloats[k * 3 + 2] = existDir[2];
                energyFloats[k * 3 + 0] = existEnergy[0];
                energyFloats[k * 3 + 1] = existEnergy[1];
                energyFloats[k * 3 + 2] = existEnergy[2];
            }

            if (lightFloats)
            {
                lightFloats[k * 3 + 0] += ambColor[0];
                lightFloats[k * 3 + 1] += ambColor[1];
                lightFloats[k * 3 + 2] += ambColor[2];
            }

            if ((j & 31) == 0)
            {
                ThreadLock();
                Broadcast_KeepAlive();
                ThreadUnlock();
            }
        }
    }
    free(tw);
    ThreadCompletedWeighted(surfWeight);
}

void LightAmbient(void)
{
    double start, end;
    if (!ambient_enabled)
        return;
    _printf("--- TraceAmbient (%i samples/texel) ---\n", ambient_samples);
    start = I_FloatTime();
    RunThreadsOnWeighted(numDrawSurfaces, numTotalLuxels, qtrue, TraceAmbient);
    end = I_FloatTime();
    _printf("\n");
    _printf("%5.0f seconds elapsed in TraceAmbient\n", end - start);
}
