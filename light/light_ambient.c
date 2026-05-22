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

    samples = mao_ambient_samples;

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

        end[0] = origin[0] + dir[0] * mao_radius;
        end[1] = origin[1] + dir[1] * mao_radius;
        end[2] = origin[2] + dir[2] * mao_radius;

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

        end[0] = origin[0] + dir[0] * mao_radius;
        end[1] = origin[1] + dir[1] * mao_radius;
        end[2] = origin[2] + dir[2] * mao_radius;

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
        }
    }

    free(tw);

    float skyOpen    = (skyWeight    > 0.0f) ? 1.0f / skyWeight    : 0.0f;
    float groundOpen = (groundWeight > 0.0f) ? 1.0f / groundWeight : 0.0f;

    maoAmbient[num * 3 + 0] = skyAccum[0] * skyOpen + groundAccum[0] * groundOpen;
    maoAmbient[num * 3 + 1] = skyAccum[1] * skyOpen + groundAccum[1] * groundOpen;
    maoAmbient[num * 3 + 2] = skyAccum[2] * skyOpen + groundAccum[2] * groundOpen;
}

/*
================
RunMAOPass
================
*/
void RunMAOPass(void)
{
    if (!mao_enabled)
        return;

    if (numGridPoints <= 0)
        return;

    _printf("--- RunMAOPass (Ambient Pre-computation) ---\n");
    if (!maoAmbient)
    {
        maoAmbient = Q_Alloc(numGridPoints * 3 * sizeof(float));
        if (!maoAmbient)
            Error("Failed to allocate MAO ambient array");
    }

    RunThreadsOnIndividual(numGridPoints, qtrue, ComputeMAOPoint);
}

/*
================
TraceAmbient
================
*/
void TraceAmbient(int num)
{
    int i, j, k, c;
    int realSurfIndex;
    dsurface_t *ds;
    mesh_t *mesh = NULL;
    int sampleWidth, sampleHeight;
    vec3_t lightmapVecs[2];
    vec3_t lightmapOrigin, normal;
    int surfWeight;
    traceWork_t *tw;

    tw = Q_Alloc(sizeof(traceWork_t));
    if (!tw) return;
    memset(tw, 0, sizeof(traceWork_t));

    realSurfIndex = surfaceWorkOrder[num];
    ds = &drawSurfaces[realSurfIndex];
    surfWeight = (ds->lightmapNum[0] >= 0) ? (ds->lightmapWidth * ds->lightmapHeight) : 1;

    if (ds->lightmapNum[0] < 0)
    {
        free(tw);
        ThreadCompletedWeighted(surfWeight);
        return;
    }

    int isDilated = upscale || (ds->surfaceType == MST_TRIANGLE_SOUP);
    int use_upscale = upscale;
    // TraceAmbient evaluates interior texels globally. We use the TraceLtm math formula verbatim 
    // for unification, so we define local scale=1 and currentGutter=0 to represent the global mapping.
    int scale = 1;
    int currentGutter = 0;

    if (ds->surfaceType == MST_PATCH)
    {
        mesh = localSurfaces[realSurfIndex].patchMesh;
        sampleWidth = ds->lightmapWidth;
        sampleHeight = ds->lightmapHeight;
    }
    else
    {
        VectorCopy(ds->lightmapVecs[2], normal);
        VectorCopy(ds->lightmapOrigin, lightmapOrigin);
        if (ds->surfaceType == MST_PLANAR)
        {
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[0], lightmapOrigin);
            VectorMA(lightmapOrigin, -0.5f, ds->lightmapVecs[1], lightmapOrigin);
        }

        VectorCopy(ds->lightmapVecs[0], lightmapVecs[0]);
        VectorCopy(ds->lightmapVecs[1], lightmapVecs[1]);
        sampleWidth = ds->lightmapWidth;
        sampleHeight = ds->lightmapHeight;
    }

    for (i = 0; i < sampleWidth; i++)
    {
        for (j = 0; j < sampleHeight; j++)
        {
            float u = (float)(i - currentGutter) + 0.5f; // TraceAmbient has no jdx
            float v = (float)(j - currentGutter) + 0.5f; // TraceAmbient has no jdy
            float step = 1.0f / (float)scale;
            vec3_t origin;
            double base[3];

            if (ds->surfaceType == MST_TRIANGLE_SOUP)
            {
                float st[2];
                vec3_t temp_origin;
                st[0] = (float)ds->lightmapOffset[0][0] + u * step;
                st[1] = (float)ds->lightmapOffset[0][1] + v * step;
                if (!TriSoupSamplePoint(ds, st, temp_origin, normal))
                    continue;
                for (c = 0; c < 3; c++)
                    base[c] = (double)temp_origin[c] + (double)normal[c] * SAMPLE_NUDGE;
            }
            else if (ds->surfaceType == MST_PATCH)
            {
                float st[2];
                vec3_t temp_origin;
                st[0] = (float)ds->lightmapOffset[0][0] + u * step;
                st[1] = (float)ds->lightmapOffset[0][1] + v * step;
                if (!PatchSamplePoint(mesh, st, temp_origin, normal))
                    continue;
                for (c = 0; c < 3; c++)
                    base[c] = (double)temp_origin[c] + (double)normal[c] * SAMPLE_NUDGE;
            }
            else
            {
                for (c = 0; c < 3; c++)
                {
                    base[c] = (double)lightmapOrigin[c]
                            + (double)normal[c] * SAMPLE_NUDGE
                            + (double)u * lightmapVecs[0][c]
                            + (double)v * lightmapVecs[1][c];
                }
            }

            for (c = 0; c < 3; c++)
            {
                base[c] += localSurfaces[realSurfIndex].entityOrigin[c];
                origin[c] = (float)base[c];
            }

            k = (ds->lightmapNum[0] * LIGHTMAP_HEIGHT + ds->lightmapOffset[0][1] + j)
                    * LIGHTMAP_WIDTH
                    + ds->lightmapOffset[0][0] + i;
            if (k < 0 || k >= numLightBytes / 3)
                continue;

            if (lightAlphaMask && lightAlphaMask[k] == 0)
                continue;

            // --- Volumetric Irradiance Gathering ---
            float gatherRadius = mao_gather_radius;
            float gatherRadiusSq = gatherRadius * gatherRadius;

            int ix_min = (int)((origin[0] - gatherRadius - gridMins[0]) / gridSize[0]);
            int ix_max = (int)((origin[0] + gatherRadius - gridMins[0]) / gridSize[0]);
            int iy_min = (int)((origin[1] - gatherRadius - gridMins[1]) / gridSize[1]);
            int iy_max = (int)((origin[1] + gatherRadius - gridMins[1]) / gridSize[1]);
            int iz_min = (int)((origin[2] - gatherRadius - gridMins[2]) / gridSize[2]);
            int iz_max = (int)((origin[2] + gatherRadius - gridMins[2]) / gridSize[2]);

            // clamp to bounds
            if (ix_min < 0) ix_min = 0; if (ix_max > gridBounds[0] - 1) ix_max = gridBounds[0] - 1;
            if (iy_min < 0) iy_min = 0; if (iy_max > gridBounds[1] - 1) iy_max = gridBounds[1] - 1;
            if (iz_min < 0) iz_min = 0; if (iz_max > gridBounds[2] - 1) iz_max = gridBounds[2] - 1;

            vec3_t ambColor;
            VectorClear(ambColor);
            vec3_t bentAccum;
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

                        float falloff = 1.0f - (distSq / gatherRadiusSq);
                        
                        // Inverse-Square with Minimum Size (Offset Model)
                        // Gives a headstart of 32 units, as requested.
                        // float sizeSq = 32.0f * 32.0f;
                        // float falloff = 1.0f / (distSq + sizeSq);

                        float w = NdotL * falloff;

                        // Trace ray
                        trace_t trace;
                        TraceLine(origin, gPos, &trace, qfalse, tw);
                        if (!trace.passSolid) {
                            ambColor[0] += gCol[0] * w;
                            ambColor[1] += gCol[1] * w;
                            ambColor[2] += gCol[2] * w;
                            
                            float lum = gCol[0] * 0.299f + gCol[1] * 0.587f + gCol[2] * 0.114f;
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
            } else {
                continue; // completely occluded or dark
            }

            float ambLum = ambColor[0] * 0.299f + ambColor[1] * 0.587f + ambColor[2] * 0.114f;
            if (ambLum < 0.0001f)
                continue;

            // Use bent surface normal as the ambient direction for deluxemap blending
            vec3_t bentNormal;
            if (VectorNormalize(bentAccum, bentNormal) < 0.0001f) {
                VectorCopy(normal, bentNormal);
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
                                      ambColor, normalizedBent, ambColor, normal);

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
        }
    }
    free(tw);
    ThreadCompletedWeighted(surfWeight);
}

void LightAmbient(long long totalLuxels)
{
    double start, end;
    if (!mao_enabled)
        return;
    _printf("--- TraceAmbient (%i samples/texel) ---\n", mao_ambient_samples);
    start = I_FloatTime();
    RunThreadsOnWeighted(numDrawSurfaces, totalLuxels, qtrue, TraceAmbient);
    end = I_FloatTime();
    _printf("\n");
    _printf("%5.0f seconds elapsed in TraceAmbient\n", end - start);
}
