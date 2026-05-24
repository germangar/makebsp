/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "../libs/assimp/include/assimp/cimport.h"
#include "../libs/assimp/include/assimp/postprocess.h"
#include "../libs/assimp/include/assimp/scene.h"
#include "qbsp.h"
#include "model_collision.h"
#include "xatlas_c.h"

typedef struct
{
    float u, v;
} uv_t;

int c_triangleModels;
int c_triangleSurfaces;
int c_triangleVertexes;
int c_triangleIndexes;

#define MAX_MODEL_INSTANCES 1024
modelInstance_t modelInstances[MAX_MODEL_INSTANCES];
int numModelInstances;

typedef struct modelCache_s
{
    char name[MAX_QPATH];
    const struct aiScene *scene;
} modelCache_t;

#define MAX_MODEL_CACHE 256
static modelCache_t modelCache[MAX_MODEL_CACHE];
static int numModelCache;

/*
============
ShaderForMesh

Determine the shader name for a given mesh, applying format-specific rules.
============
*/
static void ShaderForMesh(const char *modelPath, const struct aiMesh *mesh,
                          const struct aiScene *scene, char *shaderName)
{
    char ext[16];
    struct aiMaterial *mat;
    struct aiString path;
    struct aiString matName;

    if (mesh->mMaterialIndex >= scene->mNumMaterials)
    {
        strcpy(shaderName, "default");
        return;
    }

    mat = scene->mMaterials[mesh->mMaterialIndex];

    // Step 1: Prioritize high-level texture API (Diffuse) for all formats
    if (aiGetMaterialTexture(mat, aiTextureType_DIFFUSE, 0, &path, NULL, NULL,
                             NULL, NULL, NULL, NULL) == aiReturn_SUCCESS)
    {
        strncpy(shaderName, path.data, MAX_QPATH - 1);
        shaderName[MAX_QPATH - 1] = '\0';
        StripExtension(shaderName);
        return;
    }

    ExtractFileExtension(modelPath, ext);

    if (!Q_stricmp(ext, "obj"))
    {
        if (aiGetMaterialString(mat, "$tex.file", 0, 0, &path) ==
            aiReturn_SUCCESS)
        {
            strncpy(shaderName, path.data, MAX_QPATH - 1);
            shaderName[MAX_QPATH - 1] = '\0';
            StripExtension(shaderName);
            return;
        }
    }

    if (aiGetMaterialString(mat, AI_MATKEY_NAME, &matName) == aiReturn_SUCCESS)
    {
        strncpy(shaderName, matName.data, MAX_QPATH - 1);
        shaderName[MAX_QPATH - 1] = '\0';
    }
    else
    {
        strcpy(shaderName, "default");
    }
}

/*
====================
GetCachedModel
====================
*/
static const struct aiScene *GetCachedModel(const char *modelName)
{
    int i;
    char filename[1024];

    for (i = 0; i < numModelCache; i++)
    {
        if (!Q_stricmp(modelCache[i].name, modelName))
        {
            return modelCache[i].scene;
        }
    }

    if (numModelCache == MAX_MODEL_CACHE)
    {
        Error("MAX_MODEL_CACHE reached");
    }

    sprintf(filename, "%s%s", gamedir, modelName);

    const struct aiScene *scene = aiImportFile(
        filename, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                      aiProcess_SortByPType | aiProcess_FlipUVs |
                      aiProcess_FlipWindingOrder |
                      aiProcess_PreTransformVertices);

    if (!scene)
    {
        return NULL;
    }

    if (strlen(modelName) >= MAX_QPATH)
    {
    }
    strncpy(modelCache[numModelCache].name, modelName, MAX_QPATH - 1);
    modelCache[numModelCache].name[MAX_QPATH - 1] = '\0';
    modelCache[numModelCache].scene = scene;
    numModelCache++;

    return scene;
}

/*
====================
AnglesToMatrix
====================
*/
static void AnglesToMatrix(vec3_t angles, float matrix[3][3])
{
    float angle, sr, cr, sp, cp, sy, cy;

    angle = DEG2RAD(angles[1]); // Yaw
    sy = sin(angle);
    cy = cos(angle);
    angle = DEG2RAD(angles[0]); // Pitch
    sp = sin(angle);
    cp = cos(angle);
    angle = DEG2RAD(angles[2]); // Roll
    sr = sin(angle);
    cr = cos(angle);

    matrix[0][0] = cp * cy;
    matrix[0][1] = cp * sy;
    matrix[0][2] = -sp;

    matrix[1][0] = sr * sp * cy + cr * -sy;
    matrix[1][1] = sr * sp * sy + cr * cy;
    matrix[1][2] = sr * cp;

    matrix[2][0] = cr * sp * cy + -sr * -sy;
    matrix[2][1] = cr * sp * sy + -sr * cy;
    matrix[2][2] = cr * cp;
}

typedef struct
{
    int originalIdx;
    float x, y, z;
} sortVert_t;

static int CompareVerts(const void *a, const void *b)
{
    sortVert_t *v1 = (sortVert_t *)a;
    sortVert_t *v2 = (sortVert_t *)b;
    if (v1->x != v2->x)
        return (v1->x < v2->x) ? -1 : 1;
    if (v1->y != v2->y)
        return (v1->y < v2->y) ? -1 : 1;
    if (v1->z != v2->z)
        return (v1->z < v2->z) ? -1 : 1;
    return 0;
}

/*
====================
IdentifyIslands

Groups faces into geometric islands based on vertex connectivity.
====================
*/
static int *IdentifyIslands(const struct aiMesh *mesh, int *numIslandsOut)
{
    int *vPosId = malloc(sizeof(int) * mesh->mNumVertices);
    for (int j = 0; j < (int)mesh->mNumVertices; j++)
        vPosId[j] = -1;

    int numUniquePositions = 0;
    sortVert_t *sVerts = malloc(sizeof(sortVert_t) * mesh->mNumVertices);
    for (int j = 0; j < (int)mesh->mNumVertices; j++)
    {
        sVerts[j].originalIdx = j;
        sVerts[j].x = mesh->mVertices[j].x;
        sVerts[j].y = mesh->mVertices[j].y;
        sVerts[j].z = mesh->mVertices[j].z;
    }

    qsort(sVerts, mesh->mNumVertices, sizeof(sortVert_t), CompareVerts);

    for (int j = 0; j < (int)mesh->mNumVertices; j++)
    {
        if (j > 0 && sVerts[j].x == sVerts[j - 1].x && sVerts[j].y == sVerts[j - 1].y && sVerts[j].z == sVerts[j - 1].z)
        {
            vPosId[sVerts[j].originalIdx] = vPosId[sVerts[j - 1].originalIdx];
        }
        else
        {
            vPosId[sVerts[j].originalIdx] = numUniquePositions++;
        }
    }
    free(sVerts);

    int *triIsland = malloc(sizeof(int) * mesh->mNumFaces);
    for (int j = 0; j < (int)mesh->mNumFaces; j++)
        triIsland[j] = -1;

    int *pTriCount = calloc(numUniquePositions, sizeof(int));
    for (int j = 0; j < (int)mesh->mNumFaces; j++)
    {
        if (mesh->mFaces[j].mNumIndices != 3)
            continue;
        int p0 = vPosId[mesh->mFaces[j].mIndices[0]];
        int p1 = vPosId[mesh->mFaces[j].mIndices[1]];
        int p2 = vPosId[mesh->mFaces[j].mIndices[2]];
        pTriCount[p0]++;
        pTriCount[p1]++;
        pTriCount[p2]++;
    }
    int **pTris = malloc(sizeof(int *) * numUniquePositions);
    int *pTriOffset = calloc(numUniquePositions, sizeof(int));
    for (int j = 0; j < numUniquePositions; j++)
    {
        pTris[j] = malloc(sizeof(int) * pTriCount[j]);
    }
    for (int j = 0; j < (int)mesh->mNumFaces; j++)
    {
        if (mesh->mFaces[j].mNumIndices != 3)
            continue;
        for (int k = 0; k < 3; k++)
        {
            int pIdx = vPosId[mesh->mFaces[j].mIndices[k]];
            pTris[pIdx][pTriOffset[pIdx]++] = j;
        }
    }

    int numIslands = 0;
    int *stack = malloc(sizeof(int) * mesh->mNumFaces);
    for (int j = 0; j < (int)mesh->mNumFaces; j++)
    {
        if (triIsland[j] != -1 || mesh->mFaces[j].mNumIndices != 3)
            continue;
        int stackPtr = 0;
        stack[stackPtr++] = j;
        triIsland[j] = numIslands;
        while (stackPtr > 0)
        {
            int currTri = stack[--stackPtr];
            for (int k = 0; k < 3; k++)
            {
                int pIdx = vPosId[mesh->mFaces[currTri].mIndices[k]];
                for (int m = 0; m < pTriCount[pIdx]; m++)
                {
                    int nextTri = pTris[pIdx][m];
                    if (triIsland[nextTri] == -1)
                    {
                        triIsland[nextTri] = numIslands;
                        stack[stackPtr++] = nextTri;
                    }
                }
            }
        }
        numIslands++;
    }
    for (int j = 0; j < numUniquePositions; j++)
        free(pTris[j]);
    free(pTris);
    free(pTriCount);
    free(pTriOffset);
    free(vPosId);
    free(stack);

    *numIslandsOut = numIslands;
    return triIsland;
}

/*
====================
TryXAtlasUVs

Use xatlas library to pack existing UVs.
====================
*/
static uv_t *TryXAtlasUVs(const struct aiMesh *mesh, int uvChannel, int ssize, float lightmapScale, vec3_t scale_vec)
{
    int numIslands = 0;
    int *triIsland = IdentifyIslands(mesh, &numIslands);
    if (!triIsland)
        return NULL;

    xatlasAtlas *atlas = xatlasCreate();
    if (!atlas)
    {
        free(triIsland);
        return NULL;
    }

    // Prepare mesh declaration for xatlas
    xatlasUvMeshDecl decl;
    xatlasUvMeshDeclInit(&decl);

    float *uvs = malloc(sizeof(float) * 2 * mesh->mNumVertices);
    for (int i = 0; i < (int)mesh->mNumVertices; i++)
    {
        uvs[i * 2 + 0] = mesh->mTextureCoords[uvChannel][i].x;
        uvs[i * 2 + 1] = mesh->mTextureCoords[uvChannel][i].y;
    }

    uint32_t *indices = malloc(sizeof(uint32_t) * mesh->mNumFaces * 3);
    uint32_t *materialIds = malloc(sizeof(uint32_t) * mesh->mNumFaces);
    int validTris = 0;
    for (int i = 0; i < (int)mesh->mNumFaces; i++)
    {
        if (mesh->mFaces[i].mNumIndices == 3)
        {
            indices[validTris * 3 + 0] = mesh->mFaces[i].mIndices[0];
            indices[validTris * 3 + 1] = mesh->mFaces[i].mIndices[1];
            indices[validTris * 3 + 2] = mesh->mFaces[i].mIndices[2];
            materialIds[validTris] = (uint32_t)triIsland[i];
            validTris++;
        }
    }

    decl.vertexUvData = uvs;
    decl.vertexCount = mesh->mNumVertices;
    decl.vertexStride = sizeof(float) * 2;
    decl.indexData = indices;
    decl.indexCount = validTris * 3;
    decl.indexFormat = xatlasIndexFormat_UInt32;
    decl.faceMaterialData = materialIds;

    xatlasAddMeshError error = xatlasAddUvMesh(atlas, &decl);
    if (error != xatlasAddMeshError_Success)
    {
        _printf("xatlasAddUvMesh failed: %s\n", xatlasAddMeshErrorString(error));
        xatlasDestroy(atlas);
        free(uvs);
        free(indices);
        free(materialIds);
        free(triIsland);
        return NULL;
    }

    xatlasChartOptions chartOptions;
    xatlasChartOptionsInit(&chartOptions);
    xatlasComputeCharts(atlas, &chartOptions);

    xatlasPackOptions packOptions;
    xatlasPackOptionsInit(&packOptions);
    packOptions.padding = 2; // 2 texels of padding

    float area3D = 0;
    for (int i = 0; i < (int)mesh->mNumFaces; i++)
    {
        if (mesh->mFaces[i].mNumIndices == 3)
        {
            vec3_t v0, v1, v2;
            int i0 = mesh->mFaces[i].mIndices[0];
            int i1 = mesh->mFaces[i].mIndices[1];
            int i2 = mesh->mFaces[i].mIndices[2];

            v0[0] = mesh->mVertices[i0].x * scale_vec[0];
            v0[1] = mesh->mVertices[i0].y * scale_vec[1];
            v0[2] = mesh->mVertices[i0].z * scale_vec[2];

            v1[0] = mesh->mVertices[i1].x * scale_vec[0];
            v1[1] = mesh->mVertices[i1].y * scale_vec[1];
            v1[2] = mesh->mVertices[i1].z * scale_vec[2];

            v2[0] = mesh->mVertices[i2].x * scale_vec[0];
            v2[1] = mesh->mVertices[i2].y * scale_vec[1];
            v2[2] = mesh->mVertices[i2].z * scale_vec[2];

            vec3_t side1, side2, cross;
            VectorSubtract(v1, v0, side1);
            VectorSubtract(v2, v0, side2);
            CrossProduct(side1, side2, cross);
            area3D += 0.5f * VectorLength(cross);
        }
    }

    int ssize_val = ssize ? ssize : samplesize;
    float targetResFloat = sqrt(area3D) / (float)ssize_val;
    targetResFloat *= lightmapScale;
    int targetRes = (int)ceil(targetResFloat);
    if (targetRes > LIGHTMAP_WIDTH - 2) targetRes = LIGHTMAP_WIDTH - 2;
    if (targetRes < 16) targetRes = 16;

    packOptions.resolution = targetRes;
    packOptions.texelsPerUnit = 0.0f;
    xatlasPackCharts(atlas, &packOptions);

    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0)
    {
        xatlasDestroy(atlas);
        free(uvs);
        free(indices);
        free(materialIds);
        free(triIsland);
        return NULL;
    }

    uv_t *outUVs = calloc(mesh->mNumFaces * 3, sizeof(uv_t));
    xatlasMesh *xMesh = &atlas->meshes[0];

    // xatlas might have split vertices, so we map them back using face corners.
    // We must be careful to map back to the original face index.
    int validIdx = 0;
    for (int i = 0; i < (int)mesh->mNumFaces; i++)
    {
        if (mesh->mFaces[i].mNumIndices == 3)
        {
            for (int v = 0; v < 3; v++)
            {
                uint32_t xIdx = xMesh->indexArray[validIdx * 3 + v];
                xatlasVertex *xv = &xMesh->vertexArray[xIdx];

                // Output UVs directly in the new texel-space bounds, optionally normalizing if texelsPerUnit was 0.
                // Wait, if texelsPerUnit > 0, atlas->width/height is the exact texel size needed.
                // By normalizing to [0, 1] relative to the generated atlas width/height,
                // the standard scaling logic in AllocateLightmapForMiscModel will mathematically
                // re-scale it back to EXACTLY atlas->width and atlas->height!
                // This ensures the 2 texels of padding we requested here are preserved.
                outUVs[i * 3 + v].u = xv->uv[0] / (float)atlas->width;
                outUVs[i * 3 + v].v = xv->uv[1] / (float)atlas->height;
            }
            validIdx++;
        }
    }

    xatlasDestroy(atlas);
    free(uvs);
    free(indices);
    free(materialIds);
    free(triIsland);
    return outUVs;
}

/*
====================
TrySpreadUVs

Advanced UV overlap detection and resolving.
Resolves mirrored/stacked UVs by spreading them into a 1D or 2D slot array.
Safeguarded to only touch simple mirrored props.
====================
*/
static void TrySpreadUVs(const struct aiMesh *mesh, int uvChannel, const char *modelName, int meshIdx, float *outU, float *outV)
{
    int numIslands = 0;
    int *triIsland = IdentifyIslands(mesh, &numIslands);
    if (!triIsland)
        return;

    float initialMaxU = -999999, initialMaxV = -999999;
    for (int j = 0; j < (int)mesh->mNumFaces; j++)
    {
        if (triIsland[j] == -1)
            continue;
        for (int k = 0; k < 3; k++)
        {
            int vIdx = mesh->mFaces[j].mIndices[k];
            struct aiVector3D *u = &mesh->mTextureCoords[uvChannel][vIdx];
            if (u->x > initialMaxU)
                initialMaxU = u->x;
            if (u->y > initialMaxV)
                initialMaxV = u->y;
        }
    }

    qboolean abortSpreading = (numIslands > 500 || initialMaxU > 5.0f || initialMaxV > 5.0f) ? qtrue : qfalse;
    int numShifted = 0;
    typedef struct
    {
        float mins[2], maxs[2], offset[2];
    } uvIsland_t;
    uvIsland_t *islands = NULL;

    if (!abortSpreading)
    {
        islands = malloc(sizeof(uvIsland_t) * numIslands);
        for (int j = 0; j < numIslands; j++)
        {
            islands[j].mins[0] = islands[j].mins[1] = 999999;
            islands[j].maxs[0] = islands[j].maxs[1] = -999999;
            islands[j].offset[0] = islands[j].offset[1] = 0;
        }
        for (int j = 0; j < (int)mesh->mNumFaces; j++)
        {
            if (triIsland[j] == -1)
                continue;
            int islId = triIsland[j];
            for (int k = 0; k < 3; k++)
            {
                int vIdx = mesh->mFaces[j].mIndices[k];
                struct aiVector3D *u = &mesh->mTextureCoords[uvChannel][vIdx];
                if (u->x < islands[islId].mins[0])
                    islands[islId].mins[0] = u->x;
                if (u->y < islands[islId].mins[1])
                    islands[islId].mins[1] = u->y;
                if (u->x > islands[islId].maxs[0])
                    islands[islId].maxs[0] = u->x;
                if (u->y > islands[islId].maxs[1])
                    islands[islId].maxs[1] = u->y;
            }
        }

        // ------------------------------------------
        // BEST-FIT CORNER-SEARCH PACKER
        // ------------------------------------------
        // 1. Sort by Area (Descending)
        int *sortIds = malloc(sizeof(int) * numIslands);
        for (int j = 0; j < numIslands; j++)
        {
            sortIds[j] = j;
        }

        // Simple Bubble Sort (numIslands is small)
        for (int a = 0; a < numIslands - 1; a++)
        {
            for (int b = a + 1; b < numIslands; b++)
            {
                float areaA = (islands[sortIds[a]].maxs[0] - islands[sortIds[a]].mins[0]) * (islands[sortIds[a]].maxs[1] - islands[sortIds[a]].mins[1]);
                float areaB = (islands[sortIds[b]].maxs[0] - islands[sortIds[b]].mins[0]) * (islands[sortIds[b]].maxs[1] - islands[sortIds[b]].mins[1]);
                if (areaB > areaA)
                {
                    int t = sortIds[a];
                    sortIds[a] = sortIds[b];
                    sortIds[b] = t;
                }
            }
        }

        // 2. Greedy Best-Fit Corner Search
        float gutter = 0.1f;
        float currentGlobalMaxU = 0;
        float currentGlobalMaxV = 0;

        for (int i = 0; i < numIslands; i++)
        {
            int targetId = sortIds[i];
            float bestU = -1, bestV = -1, bestScore = 999999;
            float iw = islands[targetId].maxs[0] - islands[targetId].mins[0];
            float ih = islands[targetId].maxs[1] - islands[targetId].mins[1];

            // Generate Candidate Positions (0,0 + all corners of placed islands)
            int numCand = i * 2 + 1;
            for (int c = 0; c < numCand; c++)
            {
                float px, py;
                if (c == 0)
                {
                    px = 0;
                    py = 0;
                }
                else
                {
                    int k = (c - 1) / 2;
                    int kId = sortIds[k];
                    if ((c - 1) % 2 == 0)
                    {
                        // Corner Right: (maxU_k + gutter, minV_k)
                        px = (islands[kId].maxs[0] + islands[kId].offset[0]) + gutter;
                        py = (islands[kId].mins[1] + islands[kId].offset[1]);
                    }
                    else
                    {
                        // Corner Top: (minU_k, maxV_k + gutter)
                        px = (islands[kId].mins[0] + islands[kId].offset[0]);
                        py = (islands[kId].maxs[1] + islands[kId].offset[1]) + gutter;
                    }
                }

                // Target Offsets to reach (px, py)
                float tx = px - islands[targetId].mins[0];
                float ty = py - islands[targetId].mins[1];

                // Collision Check (against all islands from 0 to i-1)
                qboolean collision = qfalse;
                float targetMins[2] = {px, py};
                float targetMaxs[2] = {px + iw, py + ih};
                float targetArea = iw * ih;

                for (int m = 0; m < i; m++)
                {
                    int mId = sortIds[m];
                    float testMins[2] = {islands[mId].mins[0] + islands[mId].offset[0], islands[mId].mins[1] + islands[mId].offset[1]};
                    float testMaxs[2] = {islands[mId].maxs[0] + islands[mId].offset[0], islands[mId].maxs[1] + islands[mId].offset[1]};

                    float interMinU = (targetMins[0] > testMins[0]) ? targetMins[0] : testMins[0], interMaxU = (targetMaxs[0] < testMaxs[0]) ? targetMaxs[0] : testMaxs[0];
                    float interMinV = (targetMins[1] > testMins[1]) ? targetMins[1] : testMins[1], interMaxV = (targetMaxs[1] < testMaxs[1]) ? targetMaxs[1] : testMaxs[1];
                    float interW = interMaxU - interMinU, interH = interMaxV - interMinV;
                    if (interW > 0.001f && interH > 0.001f)
                    {
                        float testArea = (testMaxs[0] - testMins[0]) * (testMaxs[1] - testMins[1]);
                        float interArea = interW * interH, minArea = (testArea < targetArea) ? testArea : targetArea;
                        if (interArea > 0.01f * minArea)
                        {
                            collision = qtrue;
                            break;
                        }
                    }
                }

                if (!collision)
                {
                    float newMaxU = (currentGlobalMaxU > targetMaxs[0]) ? currentGlobalMaxU : targetMaxs[0];
                    float newMaxV = (currentGlobalMaxV > targetMaxs[1]) ? currentGlobalMaxV : targetMaxs[1];
                    // Score prioritizing square aspect ratio and minimum range
                    float score = (newMaxU > newMaxV) ? newMaxU : newMaxV;
                    if (score < bestScore)
                    {
                        bestScore = score;
                        bestU = tx;
                        bestV = ty;
                    }
                }
            }

            if (bestU != -1)
            {
                islands[targetId].offset[0] = bestU;
                islands[targetId].offset[1] = bestV;
                if (bestU != 0 || bestV != 0)
                    numShifted++;
                float finalMaxU = islands[targetId].maxs[0] + bestU;
                float finalMaxV = islands[targetId].maxs[1] + bestV;
                if (finalMaxU > currentGlobalMaxU)
                    currentGlobalMaxU = finalMaxU;
                if (finalMaxV > currentGlobalMaxV)
                    currentGlobalMaxV = finalMaxV;
            }
            else
            {
                abortSpreading = qtrue;
                break;
            }

            if (currentGlobalMaxU > 20.0f || currentGlobalMaxV > 20.0f)
            {
                abortSpreading = qtrue;
                break;
            }
        }
        free(sortIds);
    }

    // ------------------------------------------
    // STEP 3: Write Back Offsets
    // ------------------------------------------
    if (!abortSpreading)
    {
        for (int j = 0; j < (int)mesh->mNumFaces; j++)
        {
            if (triIsland[j] == -1)
                continue;
            int id = triIsland[j];
            outU[j] = islands[id].offset[0];
            outV[j] = islands[id].offset[1];
        }
    }
    else
    {
        for (int j = 0; j < (int)mesh->mNumFaces; j++)
            outU[j] = outV[j] = 0;
    }

    if (islands)
        free(islands);
    free(triIsland);
}

/*
====================
LoadTriangleModels

Initial pass to load and transform all misc_model entities.
====================
*/
void LoadTriangleModels(void)
{
    int entity_num;
    entity_t *entity;
    const char *model;
    const struct aiScene *scene;
    vec3_t origin, angles;
    float scale;
    vec3_t scale_vec;
    float rotationMatrix[3][3];

    for (entity_num = 1; entity_num < num_entities; entity_num++)
    {
        entity = &entities[entity_num];
        const char *classname = ValueForKey(entity, "classname");

        if (!Q_stricmp("misc_model", classname))
        {
            model = ValueForKey(entity, "model");
            if (!model[0])
                continue;

            GetVectorForKey(entity, "origin", origin);
            GetVectorForKey(entity, "angles", angles);
            if (angles[0] == 0 && angles[1] == 0 && angles[2] == 0)
            {
                angles[1] = FloatForKey(entity, "angle");
            }

            scale = FloatForKey(entity, "modelscale");
            if (scale == 0)
                scale = 1.0f;

            GetVectorForKey(entity, "modelscale_vec", scale_vec);
            if (scale_vec[0] == 0 && scale_vec[1] == 0 && scale_vec[2] == 0)
            {
                scale_vec[0] = scale_vec[1] = scale_vec[2] = scale;
            }

            scene = GetCachedModel(model);
            if (!scene)
            {
                continue;
            }

            if (numModelInstances == MAX_MODEL_INSTANCES)
            {
                Error("MAX_MODEL_INSTANCES reached");
            }

            modelInstance_t *inst = &modelInstances[numModelInstances++];
            strncpy(inst->modelName, model, MAX_QPATH - 1);
            inst->modelName[MAX_QPATH - 1] = '\0';
            inst->creator = entity;

            // smoothing_radius override
            float smoothingRadius = -1.0f;
            const char *radStr = ValueForKey(entity, "smoothradius");
            if (!radStr[0])
                radStr = ValueForKey(entity, "_smoothradius");
            if (!radStr[0])
                radStr = ValueForKey(entity, "smooth");
            if (!radStr[0])
                radStr = ValueForKey(entity, "_smooth");
            if (radStr[0])
            {
                smoothingRadius = atof(radStr);
            }

            // vertexcolor override for all surfaces of this model instance
            int hasVertexColor = 0;
            vec3_t vertexColor;
            VectorClear(vertexColor);
            const char *vcolStr = ValueForKey(entity, "vertexcolor");
            if (!vcolStr[0])
                vcolStr = ValueForKey(entity, "_vertexcolor");
            if (vcolStr[0])
            {
                hasVertexColor = 1;
                ParseColor(vcolStr, vertexColor);
            }

            // supersample override
            float superSampleRadius = -1.0f;
            const char *ssStr = ValueForKey(entity, "supersample");
            if (!ssStr[0])
                ssStr = ValueForKey(entity, "_supersample");
            if (ssStr[0])
            {
                superSampleRadius = atof(ssStr);
                if (superSampleRadius < 0.0f)
                    superSampleRadius = 0.0f;
            }

            inst->lightmapScale = 1.0f;
            const char *ent_scale_str = ValueForKey(entity, "_lightmapscale");
            if (!ent_scale_str[0])
                ent_scale_str = ValueForKey(entity, "lightmapscale");
            if (ent_scale_str[0])
            {
                float ent_scale = atof(ent_scale_str);
                if (ent_scale > 0)
                {
                    if (ent_scale < 0.01f) ent_scale = 0.01f;
                    if (ent_scale > 16.0f) ent_scale = 16.0f;
                    inst->lightmapScale = ent_scale;
                }
            }

            inst->numDrawSurfs = 0;
            inst->drawSurfs = malloc(sizeof(mapDrawSurface_t *) * 1024); // Allocate space for many potential chunks
            if (!inst->drawSurfs)
            {
                Error("Failed to allocate drawSurfs array");
            }
            inst->num_collision_meshes = 0;

            AnglesToMatrix(angles, rotationMatrix);

            for (int i = 0; i < scene->mNumMeshes; i++)
            {
                struct aiMesh *mesh = scene->mMeshes[i];
                char shaderName[MAX_QPATH];
                ShaderForMesh(model, mesh, scene, shaderName);
                shaderInfo_t *si = ShaderInfoForShader(shaderName);

                // ==========================================
                // UV Overlap Detection & Automatic Spreading
                // ==========================================
                float *uOffsets = calloc(mesh->mNumFaces, sizeof(float));
                float *vOffsets = calloc(mesh->mNumFaces, sizeof(float));
                uv_t *xatlasUVs = NULL;
                int uvChannel = (mesh->mTextureCoords[1]) ? 1 : 0;

                if (mesh->mTextureCoords[uvChannel])
                {
                    // Most Basic Check: UV Area Ratio Detection (Sampled).
                    // Ratio > 1.1 means significant overlaps/mirroring.
                    qboolean potentialOverlap = forceUVGen;
                    float areaSum = 0;
                    float uvMins[2] = {999999, 999999}, uvMaxs[2] = {-999999, -999999};
                    int sampleStep = (mesh->mNumFaces > 200) ? 10 : 1;
                    for (int k = 0; k < (int)mesh->mNumFaces; k += sampleStep)
                    {
                        if (mesh->mFaces[k].mNumIndices != 3)
                            continue;
                        struct aiVector3D *v0 = &mesh->mTextureCoords[uvChannel][mesh->mFaces[k].mIndices[0]];
                        struct aiVector3D *v1 = &mesh->mTextureCoords[uvChannel][mesh->mFaces[k].mIndices[1]];
                        struct aiVector3D *v2 = &mesh->mTextureCoords[uvChannel][mesh->mFaces[k].mIndices[2]];

                        // Triangle Area (Shoelace formula)
                        float a = 0.5f * fabsf((v0->x - v2->x) * (v1->y - v0->y) - (v0->x - v1->x) * (v2->y - v0->y));
                        areaSum += a;

                        // UV Bounds
                        for (int m = 0; m < 3; m++)
                        {
                            struct aiVector3D *v = &mesh->mTextureCoords[uvChannel][mesh->mFaces[k].mIndices[m]];
                            if (v->x < uvMins[0])
                                uvMins[0] = v->x;
                            if (v->x > uvMaxs[0])
                                uvMaxs[0] = v->x;
                            if (v->y < uvMins[1])
                                uvMins[1] = v->y;
                            if (v->y > uvMaxs[1])
                                uvMaxs[1] = v->y;
                        }
                    }
                    float bboxArea = (uvMaxs[0] - uvMins[0]) * (uvMaxs[1] - uvMins[1]);
                    float areaRatio = (bboxArea > 0.001f) ? (areaSum * sampleStep / bboxArea) : 0;

                    if (areaRatio > 1.05f)
                    {
                        potentialOverlap = qtrue;
                    }

                    if (potentialOverlap)
                    {
                        _printf("Potential UV overlap detected (ratio: %.2f). Trying xatlas packing...\n", areaRatio);
                        int ssize = samplesize;
                        if (si && si->lightmapSampleSize > 0)
                            ssize = si->lightmapSampleSize;

                        xatlasUVs = TryXAtlasUVs(mesh, uvChannel, ssize, inst->lightmapScale, scale_vec);
                        if (xatlasUVs)
                        {
                            _printf("xatlas packing successful.\n");
                        }
                        else
                        {
                            _printf("xatlas packing failed, falling back to original spreading.\n");
                            TrySpreadUVs(mesh, uvChannel, model, i, uOffsets, vOffsets);
                        }
                    }
                }

                // ==========================================
                // STEP 1: Extract Raw Collision Topology
                // ==========================================
                if (si && (si->contents & CONTENTS_SOLID) && inst->num_collision_meshes < MAX_MODEL_COLLISION_MESHES)
                {
                    colMesh_t *cm = malloc(sizeof(colMesh_t));
                    memset(cm, 0, sizeof(colMesh_t));
                    cm->shaderInfo = si;

                    cm->numVerts = mesh->mNumVertices;
                    cm->verts = malloc(sizeof(vec3_t) * cm->numVerts);
                    for (int j = 0; j < mesh->mNumVertices; j++)
                    {
                        float mx = mesh->mVertices[j].x * scale_vec[0];
                        float my = mesh->mVertices[j].y * scale_vec[1];
                        float mz = mesh->mVertices[j].z * scale_vec[2];

                        // Axis Swap (Assimp Y-Up -> Quake Z-Up)
                        vec3_t tx;
                        tx[0] = mx;
                        tx[1] = -mz;
                        tx[2] = my;

                        // Rotation
                        cm->verts[j][0] = origin[0] + (tx[0] * rotationMatrix[0][0] + tx[1] * rotationMatrix[1][0] + tx[2] * rotationMatrix[2][0]);
                        cm->verts[j][1] = origin[1] + (tx[0] * rotationMatrix[0][1] + tx[1] * rotationMatrix[1][1] + tx[2] * rotationMatrix[2][1]);
                        cm->verts[j][2] = origin[2] + (tx[0] * rotationMatrix[0][2] + tx[1] * rotationMatrix[1][2] + tx[2] * rotationMatrix[2][2]);
                    }

                    // Count valid triangles
                    int validTris = 0;
                    for (int j = 0; j < (int)mesh->mNumFaces; j++)
                    {
                        if (mesh->mFaces[j].mNumIndices == 3)
                        {
                            validTris++;
                        }
                    }

                    cm->numTris = validTris;
                    if (cm->numTris > 0)
                    {
                        cm->tris = malloc(sizeof(colTri_t) * cm->numTris);
                        int triIdx = 0;
                        for (int j = 0; j < (int)mesh->mNumFaces; j++)
                        {
                            if (mesh->mFaces[j].mNumIndices == 3)
                            {
                                cm->tris[triIdx][0] = mesh->mFaces[j].mIndices[0];
                                cm->tris[triIdx][1] = mesh->mFaces[j].mIndices[1];
                                cm->tris[triIdx][2] = mesh->mFaces[j].mIndices[2];
                                triIdx++;
                            }
                        }
                        inst->collision_meshes[inst->num_collision_meshes++] = cm;
                    }
                    else
                    {
                        free(cm->verts);
                        free(cm);
                    }
                }

                // ==========================================
                // STEP 2: Chunk Visual Geometry
                // ==========================================
                if (si && (si->surfaceFlags & SURF_SKIP))
                {
                    if (xatlasUVs)
                        free(xatlasUVs);
                    if (uOffsets)
                        free(uOffsets);
                    if (vOffsets)
                        free(vOffsets);
                    continue;
                }

                int currentFace = 0;

                // Vertex mapping array (old index -> new index inside this chunk)
                int *vMap = malloc(sizeof(int) * mesh->mNumVertices);
                int *vMapReverse = malloc(sizeof(int) * MAX_SURFACE_VERTS);

                while (currentFace < (int)mesh->mNumFaces)
                {
                    if (inst->numDrawSurfs >= 1024)
                    {
                        Error("Too many draw surfaces generated for model %s! Increase array size.", model);
                    }

                    mapDrawSurface_t *ds = AllocDrawSurf();
                    inst->drawSurfs[inst->numDrawSurfs++] = ds;
                    ds->miscModel = qtrue;
                    ds->superSampleRadius = superSampleRadius;
                    ds->smoothingRadius = smoothingRadius;
                    ds->hasVertexColor = hasVertexColor;
                    if (hasVertexColor)
                        VectorCopy(vertexColor, ds->vertexColor);
                    ds->planeNum = -1;
                    ds->shaderInfo = si;
                    ds->lightmapNum = -1;
                    ds->fogNum = -1;

                    // Resolve sample size hierarchy (must be AFTER memset!)
                    ds->samplesize = samplesize; // Start with global default
                    if (si && si->lightmapSampleSize > 0)
                    {
                        ds->samplesize = si->lightmapSampleSize;
                    }

                    // Entity-level lightmapscale for models
                    ds->lightmapScale = inst->lightmapScale;

                    _printf("Final samplesize for misc_model: %d, lightmapScale: %.2f\n", ds->samplesize, ds->lightmapScale);

                    // Reset vMap for this new chunk
                    for (int v = 0; v < mesh->mNumVertices; v++)
                        vMap[v] = -1;

                    // Max sizes
                    ds->verts = malloc(sizeof(drawVert_t) * MAX_SURFACE_VERTS);
                    ds->indexes = malloc(sizeof(int) * MAX_SURFACE_INDEXES);
                    ds->numVerts = 0;
                    ds->numIndexes = 0;

                    if (!ds->verts || !ds->indexes)
                        Error("Failed to allocate chunk arrays");

                    while (currentFace < (int)mesh->mNumFaces)
                    {
                        struct aiFace *face = &mesh->mFaces[currentFace];
                        if (face->mNumIndices != 3)
                        {
                            currentFace++;
                            continue;
                        }

                        // How many NEW vertices would this face add?
                        int newVerts = 0;
                        if (xatlasUVs)
                        {
                            // With xatlas, we might need more vertices due to UV splits.
                            // We'll check for matching vertices in the current chunk.
                            for (int k = 0; k < 3; k++)
                            {
                                int oldIdx = face->mIndices[k];
                                int found = -1;
                                for (int v = 0; v < ds->numVerts; v++)
                                {
                                    if (vMapReverse[v] == oldIdx)
                                    {
                                        if (ds->verts[v].lightmap[0][0] == xatlasUVs[currentFace * 3 + k].u &&
                                            ds->verts[v].lightmap[0][1] == xatlasUVs[currentFace * 3 + k].v)
                                        {
                                            found = v;
                                            break;
                                        }
                                    }
                                }
                                if (found == -1)
                                    newVerts++;
                            }
                        }
                        else
                        {
                            for (int k = 0; k < 3; k++)
                            {
                                if (vMap[face->mIndices[k]] == -1)
                                    newVerts++;
                            }
                        }

                        // Check limits!
                        if (ds->numVerts + newVerts > MAX_SURFACE_VERTS || ds->numIndexes + 3 > MAX_SURFACE_INDEXES)
                        {
                            if (ds->numIndexes == 0)
                            {
                                Error("Single triangle exceeds limits?");
                            }
                            break;
                        }

                        // Add the face to this chunk
                        for (int k = 0; k < 3; k++)
                        {
                            unsigned int oldIdx = face->mIndices[k];
                            int mapIdx = -1;

                            if (xatlasUVs)
                            {
                                for (int v = 0; v < ds->numVerts; v++)
                                {
                                    if (vMapReverse[v] == oldIdx)
                                    {
                                        if (ds->verts[v].lightmap[0][0] == xatlasUVs[currentFace * 3 + k].u &&
                                            ds->verts[v].lightmap[0][1] == xatlasUVs[currentFace * 3 + k].v)
                                        {
                                            mapIdx = v;
                                            break;
                                        }
                                    }
                                }
                            }
                            else
                            {
                                mapIdx = vMap[oldIdx];
                            }

                            if (mapIdx == -1)
                            {
                                // Add the vertex!
                                mapIdx = ds->numVerts;
                                if (!xatlasUVs)
                                    vMap[oldIdx] = mapIdx;
                                vMapReverse[mapIdx] = oldIdx;

                                drawVert_t *dv = &ds->verts[ds->numVerts];

                                float mx = mesh->mVertices[oldIdx].x * scale_vec[0];
                                float my = mesh->mVertices[oldIdx].y * scale_vec[1];
                                float mz = mesh->mVertices[oldIdx].z * scale_vec[2];

                                vec3_t tx;
                                tx[0] = mx;
                                tx[1] = -mz;
                                tx[2] = my;

                                dv->xyz[0] = origin[0] + (tx[0] * rotationMatrix[0][0] + tx[1] * rotationMatrix[1][0] + tx[2] * rotationMatrix[2][0]);
                                dv->xyz[1] = origin[1] + (tx[0] * rotationMatrix[0][1] + tx[1] * rotationMatrix[1][1] + tx[2] * rotationMatrix[2][1]);
                                dv->xyz[2] = origin[2] + (tx[0] * rotationMatrix[0][2] + tx[1] * rotationMatrix[1][2] + tx[2] * rotationMatrix[2][2]);

                                if (mesh->mNormals)
                                {
                                    float nx = mesh->mNormals[oldIdx].x;
                                    float ny = mesh->mNormals[oldIdx].y;
                                    float nz = mesh->mNormals[oldIdx].z;

                                    tx[0] = nx;
                                    tx[1] = -nz;
                                    tx[2] = ny;

                                    dv->normal[0] = (tx[0] * rotationMatrix[0][0] + tx[1] * rotationMatrix[1][0] + tx[2] * rotationMatrix[2][0]);
                                    dv->normal[1] = (tx[0] * rotationMatrix[0][1] + tx[1] * rotationMatrix[1][1] + tx[2] * rotationMatrix[2][1]);
                                    dv->normal[2] = (tx[0] * rotationMatrix[0][2] + tx[1] * rotationMatrix[1][2] + tx[2] * rotationMatrix[2][2]);
                                }

                                if (mesh->mTextureCoords[0])
                                {
                                    dv->st[0] = mesh->mTextureCoords[0][oldIdx].x;
                                    dv->st[1] = mesh->mTextureCoords[0][oldIdx].y;
                                }

                                if (xatlasUVs)
                                {
                                    dv->lightmap[0][0] = xatlasUVs[currentFace * 3 + k].u;
                                    dv->lightmap[0][1] = xatlasUVs[currentFace * 3 + k].v;
                                }
                                else
                                {
                                    // Original or Spread UVs
                                    if (mesh->mTextureCoords[uvChannel])
                                    {
                                        dv->lightmap[0][0] = mesh->mTextureCoords[uvChannel][oldIdx].x + uOffsets[currentFace];
                                        dv->lightmap[0][1] = mesh->mTextureCoords[uvChannel][oldIdx].y + vOffsets[currentFace];
                                    }
                                }

                                dv->color[0][0] = dv->color[0][1] = dv->color[0][2] = dv->color[0][3] = 255;
                                ds->numVerts++;
                            }
                            // Add the index
                            ds->indexes[ds->numIndexes++] = mapIdx;
                        }
                        currentFace++;
                    }

                    // Realloc to save memory
                    if (ds->numVerts > 0)
                    {
                        ds->verts = realloc(ds->verts, sizeof(drawVert_t) * ds->numVerts);
                        ds->indexes = realloc(ds->indexes, sizeof(int) * ds->numIndexes);
                    }
                    else
                    {
                        inst->numDrawSurfs--;
                    }
                }

                free(vMap);
                free(vMapReverse);
                if (xatlasUVs)
                    free(xatlasUVs);
                if (uOffsets)
                    free(uOffsets);
                if (vOffsets)
                    free(vOffsets);
            }
        }
    }
}

/*
=====================
AddTriangleModels

Second pass: Insert the pre-calculated surfaces into the BSP tree.
=====================
*/
void AddTriangleModels(tree_t *tree)
{
    int i, j;

    // 1. Basic Stats
    for (i = 0; i < numModelInstances; i++)
    {
        modelInstance_t *inst = &modelInstances[i];
        c_triangleModels++;
        for (j = 0; j < inst->numDrawSurfs; j++)
        {
            c_triangleSurfaces++;
            mapDrawSurface_t *ds = inst->drawSurfs[j];
            c_triangleVertexes += ds->numVerts;
            c_triangleIndexes += ds->numIndexes;
        }
    }

    qprintf("%5i triangle models\n", c_triangleModels);
    qprintf("%5i triangle surfaces\n", c_triangleSurfaces);
    qprintf("%5i triangle vertexes\n", c_triangleVertexes);
    qprintf("%5i triangle indexes\n", c_triangleIndexes);
}
