#include "light.h"
#include "connect.h"

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
/*
================
TicketConsumeMAOProbes

Ticket wavefront (Phase 4b, Stage 3): consume MAO hemisphere probes in
generation order. Weight accumulation stays in the generation stage (it is
trace-independent and the per-probe order is identical), so this applies
only the trace outcome: clear → sky/ground color + direction accumulation,
blocked → nothing. Hemisphere via ssIdx (0 = sky, 1 = ground).
================
*/
static void TicketConsumeMAOProbes(lightTicket_t *tickets, int n,
                                   float *skyAccum, float *groundAccum,
                                   vec3_t dirAccum, float *skyWeight, float *groundWeight,
                                   float skyLum, float groundLum)
{
    int i;

    for (i = 0; i < n; i++)
    {
        lightTicket_t *t = &tickets[i];

        if (!t->trace.passSolid)
        {
            #pragma omp atomic
            c_ticketsContributed++;

            if (t->ssIdx == 0)
            {
                skyAccum[0] += t->angle * skyColor[0];
                skyAccum[1] += t->angle * skyColor[1];
                skyAccum[2] += t->angle * skyColor[2];
                VectorMA(dirAccum, t->angle * skyLum, t->dir, dirAccum);
            }
            else
            {
                groundAccum[0] += t->angle * groundColor[0];
                groundAccum[1] += t->angle * groundColor[1];
                groundAccum[2] += t->angle * groundColor[2];
                VectorMA(dirAccum, t->angle * groundLum, t->dir, dirAccum);
            }
        }
        else
        {
            #pragma omp atomic
            c_ticketsBlocked++;
        }
    }
}

/*
================
TicketConsumeBleedTickets

Ticket wavefront (Phase 4b, Stage 3): consume grid ambient-bleed tickets in
generation order. Bleed semantics (different from MAO!): blocked rays are
skipped ENTIRELY — they add neither color nor weight.
================
*/
static void TicketConsumeBleedTickets(lightTicket_t *tickets, int n,
                                      vec3_t envColor, float *totalWeight)
{
    int i;

    for (i = 0; i < n; i++)
    {
        lightTicket_t *t = &tickets[i];

        if (!t->trace.passSolid)
        {
            envColor[0] += t->dir[0] * t->angle;
            envColor[1] += t->dir[1] * t->angle;
            envColor[2] += t->dir[2] * t->angle;
            *totalWeight += t->angle;

            #pragma omp atomic
            c_ticketsContributed++;
        }
        else
        {
            #pragma omp atomic
            c_ticketsBlocked++;
        }
    }
}

void ComputeMAOPoint(int num)
{
    int mod, x, y, z, i;
    vec3_t origin;
    traceWork_t *tw;
    float skyAccum[3]   = {0, 0, 0};
    float groundAccum[3] = {0, 0, 0};
    float skyWeight = 0.0f, groundWeight = 0.0f;
    int samples;
    lightTicketBatch_t batch;   // Ticket wavefront (Phase 4b)

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

    if (gridInvalidMask[num] & GRID_FLAG_INVALID)
        return;

    if (gridInvalidMask[num] & GRID_FLAG_SOLID)
    {
        // Lattice point sits inside solid geometry: compute the macro
        // ambient at the open point resolved by the grid classification
        // pass (the same origin TraceGrid and the radiosity grid sample
        // at), so solid-but-reachable cells carry a full ambient term
        // instead of collapsing to zero.
        VectorCopy(&gridResolvedOrigins[num * 3], origin);
    }

    tw = Q_Alloc(sizeof(traceWork_t));
    if (!tw) return;
    memset(tw, 0, sizeof(traceWork_t));
    tw->ignoreSurface = -1;

    samples = ambient_samples;

    // Ticket wavefront (Phase 4b): one group per grid point; the probe batch
    // holds every hemisphere probe (capacity = sample count exactly).
    #pragma omp atomic
    c_ticketGroups++;
    {
        int batchCap = samples > 0 ? samples : 1;
        TicketBatch_Init(&batch, batchCap);
    }

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

        // Ticket wavefront: generate the probe; the consume applies the
        // clear/blocked outcome (TicketConsumeMAOProbes below).
        {
            lightTicket_t t;
            lightTicket_t *slot;
            memset(&t, 0, sizeof(t));
            t.light = NULL;
            VectorCopy(origin, t.origin);
            VectorCopy(end, t.end);
            VectorCopy(dir, t.dir);          // probe direction (consume uses it)
            t.angle = w;                     // probe weight
            t.testAll = qfalse;
            t.applyColorFilter = qfalse;
            t.normalValid = qtrue;
            t.texelIdx = num;
            t.ssIdx = 0;                     // sky hemisphere
            slot = TicketBatch_Append(&batch);
            if (slot)
                *slot = t;
            else
            {
                #pragma omp atomic
                c_ticketAppendWarnings++;
            }
        }
    }

    // Lower hemisphere (ground): same strategy, flip z
    int groundN = samples - skyN;
    for (i = 0; i < groundN; i++)
    {
        vec3_t dir, end;
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

        {
            lightTicket_t t;
            lightTicket_t *slot;
            memset(&t, 0, sizeof(t));
            t.light = NULL;
            VectorCopy(origin, t.origin);
            VectorCopy(end, t.end);
            VectorCopy(dir, t.dir);
            t.angle = w;
            t.testAll = qfalse;
            t.applyColorFilter = qfalse;
            t.normalValid = qtrue;
            t.texelIdx = num;
            t.ssIdx = 1;                     // ground hemisphere
            slot = TicketBatch_Append(&batch);
            if (slot)
                *slot = t;
            else
            {
                #pragma omp atomic
                c_ticketAppendWarnings++;
            }
        }
    }

    // Stage 2+3: flush all probes, consume in generation order (sky probes
    // first, then ground — the old interleave order per hemisphere).
    {
        int n = TicketBatch_FlushTraces(&batch, tw);
        TicketConsumeMAOProbes(batch.tickets, n, skyAccum, groundAccum, dirAccum,
                               &skyWeight, &groundWeight, skyLum, groundLum);
    }
    TicketBatch_Free(&batch);

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

    _printf("--- RunMAOPass (Ambient Pre-computation, %i samples/gridpoint) ---\n", ambient_samples);
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
TicketConsumeAmbientGatherTickets

Ticket wavefront (Phase 6, Stage 3): consume ambient-gather tickets in
generation order. The weight accumulation stayed in the generation stage
(it is trace-independent and the per-neighbor order is identical), so this
applies only the trace outcome: clear -> color + bent-direction
accumulation, blocked -> nothing here (its weight was already added at
generation). The neighbor luminance is recomputed from the ticket-carried
color with the identical expression the generation cull used (bit-identical
values, bit-identical order of operations). No rejection path: after a
clear trace the old code always accumulated.
================
*/
static void TicketConsumeAmbientGatherTickets(lightTicket_t *tickets, int n,
                                              vec3_t ambColor, vec3_t bentAccum)
{
    int i;

    for (i = 0; i < n; i++)
    {
        lightTicket_t *t = &tickets[i];

        if (!t->trace.passSolid)
        {
            #pragma omp atomic
            c_ticketsContributed++;

            ambColor[0] += t->cont.irradiance[0] * t->angle;
            ambColor[1] += t->cont.irradiance[1] * t->angle;
            ambColor[2] += t->cont.irradiance[2] * t->angle;

            float lum = t->cont.irradiance[0] * 0.299f + t->cont.irradiance[1] * 0.587f + t->cont.irradiance[2] * 0.114f;
            bentAccum[0] += t->dir[0] * t->angle * lum;
            bentAccum[1] += t->dir[1] * t->angle * lum;
            bentAccum[2] += t->dir[2] * t->angle * lum;
        }
        else
        {
            #pragma omp atomic
            c_ticketsBlocked++;
        }
    }
}

/*
================
GatherAmbientGenerate

Ticket wavefront (Phase 6, Stage 1): the neighborhood scan, culls and
weight accumulation of the old GatherAmbientAtPoint, verbatim. All weights
are trace-independent, so accumulating them here in scan order — solid
neighbors included (no trace, no ticket) — reproduces the old float sum
bit-exactly. Traced neighbors become tickets; flush-on-full applies the
older chunk's outcome before the scan continues (chunk boundaries only cut
between tickets, so the clear-neighbor subsequence keeps its scan order).
Returns totalWeight; the caller flushes the remainder, consumes, and
divides (the old in-function division).
================
*/
static float GatherAmbientGenerate(vec3_t origin, vec3_t normal, lightTicketBatch_t *batch, traceWork_t *tw, vec3_t ambColor, vec3_t bentAccum)
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

    // Ticket wavefront (Phase 6): one group per gather call (both the vertex
    // and the texel pass count). Flush-on-full — the fold is a straight
    // accumulation into the caller's ambColor/bentAccum (no group
    // sub-state), so chunked flushing is bit-identical to the old interleave.
    #pragma omp atomic
    c_ticketGroups++;

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

                // Points inside solid geometry are guaranteed to be occluded.
                // Accumulate weight directly for ambient occlusion without casting an Embree ray.
                if (gridInvalidMask && (gridInvalidMask[gidx] & GRID_FLAG_SOLID))
                {
                    totalWeight += w;
                    continue;
                }

                // Trace ray — Phase 6: becomes a ticket; the consume applies
                // the clear/blocked outcome. Flush-on-full keeps the batch small.
                if (batch->count >= batch->capacity)
                {
                    int fn = TicketBatch_FlushTraces(batch, tw);
                    TicketConsumeAmbientGatherTickets(batch->tickets, fn, ambColor, bentAccum);
                }
                {
                    lightTicket_t t;
                    lightTicket_t *slot;
                    memset(&t, 0, sizeof(t));
                    t.light = NULL;
                    VectorCopy(origin, t.origin);
                    VectorCopy(gPos, t.end);
                    VectorCopy(dir, t.dir);              // normalized neighbor direction
                    t.angle = w;                         // weight (consume uses it)
                    VectorCopy(gCol, t.cont.irradiance); // neighbor MAO color
                    t.testAll = qfalse;
                    t.applyColorFilter = qfalse;
                    t.normalValid = qtrue;
                    t.texelIdx = gidx;
                    t.ssIdx = 0;
                    slot = TicketBatch_Append(batch);
                    if (slot)
                        *slot = t;
                    else
                    {
                        #pragma omp atomic
                        c_ticketAppendWarnings++;
                    }
                }

                // Accumulate weight regardless of occlusion so blocked rays darken the final result
                totalWeight += w;
            }
        }
    }

    return totalWeight;
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

    // Ticket wavefront (Phase 4b): one group per call. Fixed modest capacity
    // with flush-on-full — the fold is a straight accumulation (no group
    // sub-state), so chunked flushing is bit-identical to the old interleave.
    lightTicketBatch_t batch;
    #pragma omp atomic
    c_ticketGroups++;
    TicketBatch_Init(&batch, 256);

    for (int gz = iz_min; gz <= iz_max; gz++) {
        for (int gy = iy_min; gy <= iy_max; gy++) {
            for (int gx = ix_min; gx <= ix_max; gx++) {
                int gidx = (gz * gridBounds[1] + gy) * gridBounds[0] + gx;
                float *gCol = &maoAmbient[gidx * 3];
                
                // cull 1: Cheapest check. Skip black/solid neighbor voxels immediately.
                float lum = gCol[0] * 0.299f + gCol[1] * 0.587f + gCol[2] * 0.114f;
                if (lum <= 0.0001f) continue;
                if (gridInvalidMask && (gridInvalidMask[gidx] & (GRID_FLAG_INVALID | GRID_FLAG_SOLID))) continue;

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

                // cull 4 (the trace) becomes a ticket; the consume applies the
                // clear/blocked outcome. Flush-on-full keeps the batch small.
                if (batch.count >= batch.capacity)
                {
                    int fn = TicketBatch_FlushTraces(&batch, tw);
                    TicketConsumeBleedTickets(batch.tickets, fn, envColor, &totalWeight);
                }
                {
                    lightTicket_t t;
                    lightTicket_t *slot;
                    memset(&t, 0, sizeof(t));
                    t.light = NULL;
                    VectorCopy(origin, t.origin);
                    VectorCopy(gPos, t.end);
                    VectorCopy(gCol, t.dir);         // neighbor color (consume uses it)
                    t.angle = w;                     // weight
                    t.testAll = qfalse;
                    t.applyColorFilter = qfalse;
                    t.normalValid = qtrue;
                    t.texelIdx = gidx;
                    t.ssIdx = 0;
                    slot = TicketBatch_Append(&batch);
                    if (slot)
                        *slot = t;
                    else
                    {
                        #pragma omp atomic
                        c_ticketAppendWarnings++;
                    }
                }
            }
        }
    }

    {
        int n = TicketBatch_FlushTraces(&batch, tw);
        TicketConsumeBleedTickets(batch.tickets, n, envColor, &totalWeight);
    }
    TicketBatch_Free(&batch);

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

    // Ticket wavefront (Phase 6): ONE batch per call (hoisted — the vertex +
    // texel passes make thousands of gather calls per surface). Freed at
    // BOTH exits (the no-lightmap early return and the normal end).
    lightTicketBatch_t batch;
    TicketBatch_Init(&batch, 256);

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
        
        float totalWeight = GatherAmbientGenerate(origin, normal, &batch, tw, ambColor, bentAccum);
        {
            int n = TicketBatch_FlushTraces(&batch, tw);
            TicketConsumeAmbientGatherTickets(batch.tickets, n, ambColor, bentAccum);
        }
        if (totalWeight > 0.0001f)
        {
            ambColor[0] /= totalWeight;
            ambColor[1] /= totalWeight;
            ambColor[2] /= totalWeight;

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
        TicketBatch_Free(&batch);
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
            
            float totalWeight = GatherAmbientGenerate(origin, normal, &batch, tw, ambColor, bentAccum);
            {
                int n = TicketBatch_FlushTraces(&batch, tw);
                TicketConsumeAmbientGatherTickets(batch.tickets, n, ambColor, bentAccum);
            }
            if (totalWeight <= 0.0001f)
                continue;

            ambColor[0] /= totalWeight;
            ambColor[1] /= totalWeight;
            ambColor[2] /= totalWeight;

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
                Broadcast_KeepAlive();
            }
        }
    }
    TicketBatch_Free(&batch);
    free(tw);
    ThreadCompletedWeighted(surfWeight);
}

void LightAmbient(void)
{
    double start, end;
    if (!ambient_enabled)
        return;
    _printf("--- TraceAmbient ---\n");
    start = I_FloatTime();
    RunThreadsOnWeighted(numDrawSurfaces, numTotalLuxels, qtrue, TraceAmbient);
    end = I_FloatTime();
    _printf("\n");
    _printf("%5.0f seconds elapsed in TraceAmbient\n", end - start);
}
