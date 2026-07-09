            ds->parentSurfaceNum = -1;
            free(globalInsets[i]);
        }
    }
}

/*
=============================================================================
ATOMIC UNIT TRISOUP MERGING (PHASE 1)
=============================================================================
*/

typedef struct
{
    int numVerts;
    drawVert_t *verts;
} weldBuf_t;

static int WeldMergedVertex(weldBuf_t *buf, int maxVerts, const drawVert_t *v)
{
    for (int i = 0; i < buf->numVerts; i++)
    {
        if (VectorCompareEpsilon(buf->verts[i].xyz, v->xyz, 0.1f) &&
            VectorCompareEpsilon(buf->verts[i].normal, v->normal, 0.05f))
        {
            return i;
        }
    }
    if (buf->numVerts >= maxVerts)
    {
        return buf->numVerts - 1;
    }
    buf->verts[buf->numVerts] = *v;
    return buf->numVerts++;
}

static void GenerateAtomicUVsWithXAtlas(mapDrawSurface_t *ds)
{
    if (ds->numVerts < 3 || ds->numIndexes < 3)
        return;

    float *positions = malloc(ds->numVerts * 3 * sizeof(float));
    for (int i = 0; i < ds->numVerts; i++)
    {
        positions[i * 3 + 0] = ds->verts[i].xyz[0];
        positions[i * 3 + 1] = ds->verts[i].xyz[1];
        positions[i * 3 + 2] = ds->verts[i].xyz[2];
    }

    uint32_t *indices = malloc(ds->numIndexes * sizeof(uint32_t));
    for (int i = 0; i < ds->numIndexes; i++)
    {
        indices[i] = (uint32_t)ds->indexes[i];
    }

    xatlasMeshDecl decl;
    xatlasMeshDeclInit(&decl);
    decl.vertexPositionData = positions;
    decl.vertexPositionStride = sizeof(float) * 3;
    decl.vertexCount = (uint32_t)ds->numVerts;
    decl.indexData = indices;
    decl.indexCount = (uint32_t)ds->numIndexes;
    decl.indexFormat = xatlasIndexFormat_UInt32;

    xatlasAtlas *atlas = xatlasCreate();
    if (!atlas)
    {
        free(positions);
        free(indices);
        return;
    }

    if (xatlasAddMesh(atlas, &decl, 1) != xatlasAddMeshError_Success)
    {
        xatlasDestroy(atlas);
        free(positions);
        free(indices);
        return;
    }

    xatlasAddMeshJoin(atlas);

    xatlasChartOptions chartOpts;
    xatlasChartOptionsInit(&chartOpts);
    xatlasComputeCharts(atlas, &chartOpts);

    float area3D = 0.0f;
    for (int i = 0; i < ds->numIndexes; i += 3)
    {
        vec3_t s1, s2, cross;
        VectorSubtract(ds->verts[ds->indexes[i + 1]].xyz, ds->verts[ds->indexes[i]].xyz, s1);
        VectorSubtract(ds->verts[ds->indexes[i + 2]].xyz, ds->verts[ds->indexes[i]].xyz, s2);
        CrossProduct(s1, s2, cross);
        area3D += 0.5f * VectorLength(cross);
    }

    float sampleSizeVal = ds->samplesize > 0.0f ? ds->samplesize : (float)samplesize;
    float scaleVal = ds->lightmapScale > 0.0f ? ds->lightmapScale : 1.0f;
    int targetRes = (int)ceil(sqrt(area3D) / sampleSizeVal * scaleVal);
    if (targetRes > LIGHTMAP_WIDTH - 2)
        targetRes = LIGHTMAP_WIDTH - 2;
    if (targetRes < 16)
        targetRes = 16;

    xatlasPackOptions packOpts;
    xatlasPackOptionsInit(&packOpts);
    packOpts.padding = 2;
    packOpts.texelsPerUnit = 0.0f;
    packOpts.resolution = targetRes;
    xatlasPackCharts(atlas, &packOpts);

    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0)
    {
        xatlasDestroy(atlas);
        free(positions);
        free(indices);
        return;
    }

    xatlasMesh *xm = &atlas->meshes[0];

    // If xatlas split any vertices at UV seams, rebuild ds->verts
    if ((int)xm->vertexCount > ds->numVerts)
    {
        drawVert_t *newVerts = malloc(xm->vertexCount * sizeof(drawVert_t));
        for (uint32_t i = 0; i < xm->vertexCount; i++)
        {
            newVerts[i] = ds->verts[xm->vertexArray[i].xref];
        }
        free(ds->verts);
        ds->verts = newVerts;
        ds->numVerts = (int)xm->vertexCount;

        for (int i = 0; i < ds->numIndexes; i++)
        {
            ds->indexes[i] = (int)xm->indexArray[i];
        }
    }

    for (uint32_t i = 0; i < xm->vertexCount; i++)
    {
        ds->verts[i].lightmap[0][0] = xm->vertexArray[i].uv[0] / (float)atlas->width;
        ds->verts[i].lightmap[0][1] = xm->vertexArray[i].uv[1] / (float)atlas->height;
    }

    xatlasDestroy(atlas);
    free(positions);
    free(indices);
}

/*
==================
MergeChamferStripsIntoParents

Merges each inset planar fragment (parentSurfaceNum == -1) with its own
immediate child chamfer strips into an atomic MST_TRIANGLE_SOUP mesh.
==================
*/
void MergeChamferStripsIntoParents(entity_t *e)
{
    int i, j, k;
    int c_mergedParents = 0;
    int c_mergedStrips = 0;
    int numSurfsAtStart = numMapDrawSurfs;

    qprintf("----- MergeChamferStripsIntoParents -----\n");

    for (i = e->firstDrawSurf; i < numSurfsAtStart; i++)
    {
        mapDrawSurface_t *parent = &mapDrawSurfs[i];
        if (parent->numVerts < 3 || parent->parentSurfaceNum != -1)
            continue;

        // Check if any strip references this parent
        qboolean hasStrips = qfalse;
        for (j = e->firstDrawSurf; j < numSurfsAtStart; j++)
        {
            if (mapDrawSurfs[j].parentSurfaceNum == i && mapDrawSurfs[j].numVerts >= 4)
            {
                hasStrips = qtrue;
                break;
            }
        }

        if (!hasStrips)
            continue;

        int maxVerts = 8192;
        int maxIndexes = 32768;

        weldBuf_t welded;
        welded.numVerts = 0;
        welded.verts = malloc(maxVerts * sizeof(drawVert_t));

        int *outIndexes = malloc(maxIndexes * sizeof(int));
        int numOutIndexes = 0;

        // Step 1: Triangulate parent fragment N-gon as CCW fan from vertex 0
        for (k = 1; k <= parent->numVerts - 2; k++)
        {
            if (numOutIndexes + 3 > maxIndexes)
                break;
            outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, &parent->verts[0]);
            outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, &parent->verts[k]);
            outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, &parent->verts[k + 1]);
        }

        // Step 2: Triangulate each child chamfer strip and weld vertices
        for (j = e->firstDrawSurf; j < numSurfsAtStart; j++)
        {
            mapDrawSurface_t *strip = &mapDrawSurfs[j];
            if (strip->parentSurfaceNum != i || strip->numVerts < 4)
                continue;

            int chainLen = strip->numVerts / 2;
            for (k = 0; k <= chainLen - 2; k++)
            {
                if (numOutIndexes + 6 > maxIndexes)
                    break;

                drawVert_t *OuterA = &strip->verts[k];
                drawVert_t *OuterB = &strip->verts[k + 1];
                drawVert_t *InnerB = &strip->verts[2 * chainLen - 2 - k];
                drawVert_t *InnerA = &strip->verts[2 * chainLen - 1 - k];

                // Tri 1 (CCW): OuterA -> OuterB -> InnerB
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, OuterA);
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, OuterB);
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, InnerB);

                // Tri 2 (CCW): OuterA -> InnerB -> InnerA
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, OuterA);
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, InnerB);
                outIndexes[numOutIndexes++] = WeldMergedVertex(&welded, maxVerts, InnerA);
            }

            strip->numVerts = 0;
            if (strip->verts)
            {
                free(strip->verts);
                strip->verts = NULL;
            }
            if (strip->indexes)
            {
                free(strip->indexes);
                strip->indexes = NULL;
                strip->numIndexes = 0;
            }
            c_mergedStrips++;
        }

        // Step 3: Promote parent to atomic Trisoup (miscModel = qtrue)
        if (parent->verts)
            free(parent->verts);
        if (parent->indexes)
            free(parent->indexes);

        parent->verts = welded.verts;
        parent->numVerts = welded.numVerts;
        parent->indexes = outIndexes;
        parent->numIndexes = numOutIndexes;
        parent->miscModel = qtrue;

        GenerateAtomicUVsWithXAtlas(parent);

        c_mergedParents++;
    }

    qprintf("%6i atomic parents merged\n", c_mergedParents);
    qprintf("%6i child strips absorbed\n", c_mergedStrips);
}