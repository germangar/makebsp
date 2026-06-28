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
#include "../libs/assimp/include/assimp/cfileio.h"
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
====================
Assimp-VFS Bridge
====================
*/
typedef struct {
    byte *data;
    size_t size;
    size_t pos;
} vfs_mem_file_t;

static size_t VFS_Read(struct aiFile* file, char* buffer, size_t size, size_t count) {
    vfs_mem_file_t *mf = (vfs_mem_file_t*)file->UserData;
    size_t to_read = size * count;
    if (mf->pos + to_read > mf->size) to_read = mf->size - mf->pos;
    memcpy(buffer, mf->data + mf->pos, to_read);
    mf->pos += to_read;
    return to_read / size;
}

static size_t VFS_Tell(struct aiFile* file) {
    vfs_mem_file_t *mf = (vfs_mem_file_t*)file->UserData;
    return mf->pos;
}

static size_t VFS_FileSize(struct aiFile* file) {
    vfs_mem_file_t *mf = (vfs_mem_file_t*)file->UserData;
    return mf->size;
}

static aiReturn VFS_Seek(struct aiFile* file, size_t offset, enum aiOrigin origin) {
    vfs_mem_file_t *mf = (vfs_mem_file_t*)file->UserData;
    switch(origin) {
        case aiOrigin_SET: mf->pos = offset; break;
        case aiOrigin_CUR: mf->pos += offset; break;
        case aiOrigin_END: mf->pos = mf->size - offset; break;
        default: return aiReturn_FAILURE;
    }
    if (mf->pos > mf->size) mf->pos = mf->size;
    return aiReturn_SUCCESS;
}

static struct aiFile* VFS_Open(struct aiFileIO* io, const char* filename, const char* mode) {
    void *buffer = NULL;
    int len = vfsLoadFile(filename, &buffer);
    
    if (len < 0 && io->UserData && ((char*)io->UserData)[0]) {
        char relPath[1024];
        char base[1024];
        strcpy(base, (char*)io->UserData);
        int blen = strlen(base);
        if (blen > 0 && base[blen-1] != '/' && base[blen-1] != '\\') {
            strcat(base, "/");
        }
        sprintf(relPath, "%s%s", base, filename);
        len = vfsLoadFile(relPath, &buffer);
    }
    
    if (len < 0) return NULL;
    
    vfs_mem_file_t *mf = malloc(sizeof(vfs_mem_file_t));
    mf->data = buffer;
    mf->size = (size_t)len;
    mf->pos = 0;
    
    struct aiFile *file = malloc(sizeof(struct aiFile));
    memset(file, 0, sizeof(struct aiFile));
    file->ReadProc = VFS_Read;
    file->TellProc = VFS_Tell;
    file->FileSizeProc = VFS_FileSize;
    file->SeekProc = VFS_Seek;
    file->UserData = (aiUserData)mf;
    
    return file;
}

static void VFS_Close(struct aiFileIO* io, struct aiFile* file) {
    vfs_mem_file_t *mf = (vfs_mem_file_t*)file->UserData;
    free(mf->data);
    free(mf);
    free(file);
}

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
    }
    else
    {
        ExtractFileExtension(modelPath, ext);

        if (!Q_stricmp(ext, "obj"))
        {
            if (aiGetMaterialString(mat, "$tex.file", 0, 0, &path) ==
                aiReturn_SUCCESS)
            {
                strncpy(shaderName, path.data, MAX_QPATH - 1);
                shaderName[MAX_QPATH - 1] = '\0';
                StripExtension(shaderName);
            }
            else if (aiGetMaterialString(mat, AI_MATKEY_NAME, &matName) == aiReturn_SUCCESS)
            {
                strncpy(shaderName, matName.data, MAX_QPATH - 1);
                shaderName[MAX_QPATH - 1] = '\0';
            }
            else
            {
                strcpy(shaderName, "default");
                return;
            }
        }
        else
        {
            if (aiGetMaterialString(mat, AI_MATKEY_NAME, &matName) == aiReturn_SUCCESS)
            {
                strncpy(shaderName, matName.data, MAX_QPATH - 1);
                shaderName[MAX_QPATH - 1] = '\0';
            }
            else
            {
                strcpy(shaderName, "default");
                return;
            }
        }
    }

    // Step 1.5: Virtualize absolute paths
    // If the path contains directories, check if it contains any active gamedir name
    if (strchr(shaderName, '/') != NULL || strchr(shaderName, '\\') != NULL)
    {
        for (int i = 0; i < numActiveGamedirs; i++)
        {
            const char *gamedir = activeGamedirs[i];
            int glen = strlen(gamedir);
            const char *p = shaderName;
            
            while ((p = strstr(p, gamedir)) != NULL)
            {
                qboolean isStart = (p == shaderName);
                qboolean hasPreSlash = (!isStart && (*(p - 1) == '/' || *(p - 1) == '\\'));
                
                if (isStart || hasPreSlash)
                {
                    const char *after = p + glen;
                    if (*after == '/' || *after == '\\' || *after == '\0')
                    {
                        char temp[MAX_QPATH];
                        if (*after == '/' || *after == '\\') after++;
                        strcpy(temp, after);
                        strcpy(shaderName, temp);
                        goto virtualization_done;
                    }
                }
                p++;
            }
        }
    }
virtualization_done:

    // Step 2: Smart Guessing for poorly configured models
    // If the resolved shaderName has no path, attempt tiered fallbacks
    if (strchr(shaderName, '/') == NULL && strchr(shaderName, '\\') == NULL && Q_stricmp(shaderName, "default"))
    {
        char modelDir[1024];
        char modelNameOnly[1024];
        char candidate[MAX_QPATH];
        char original[MAX_QPATH];

        strcpy(original, shaderName);
        ExtractFilePath(modelPath, modelDir);
        
        // Extract model filename without extension
        const char *lastSlash = strrchr(modelPath, '/');
        if (!lastSlash) lastSlash = strrchr(modelPath, '\\');
        const char *start = lastSlash ? lastSlash + 1 : modelPath;
        strcpy(modelNameOnly, start);
        StripExtension(modelNameOnly);

        // Tier 1: Literal (Already in shaderName)
        if (ShaderExists(shaderName)) return;

        // Tier 2: Same directory as model
        sprintf(candidate, "%s%s", modelDir, original);
        if (ShaderExists(candidate)) {
            strcpy(shaderName, candidate);
            return;
        }

        // Tier 3: Subdirectory named after model
        sprintf(candidate, "%s%s/%s", modelDir, modelNameOnly, original);
        if (ShaderExists(candidate)) {
            strcpy(shaderName, candidate);
            return;
        }

        // Tier 4: "textures" subdirectory
        sprintf(candidate, "%stextures/%s", modelDir, original);
        if (ShaderExists(candidate)) {
            strcpy(shaderName, candidate);
            return;
        }

        // Tier 5: Accept defeat, revert to original for standard warning
        strcpy(shaderName, original);
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
    char baseDir[1024];

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

    ExtractFilePath(modelName, baseDir);

    struct aiFileIO io;
    io.OpenProc = VFS_Open;
    io.CloseProc = VFS_Close;
    io.UserData = (aiUserData)baseDir;

    const struct aiScene *scene = aiImportFileEx(
        modelName,
        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
            aiProcess_SortByPType | aiProcess_FlipUVs |
            aiProcess_FlipWindingOrder |
            aiProcess_PreTransformVertices,
        &io);

    if (!scene)
    {
        _printf("WARNING: Could not load or parse model file %s (Assimp error: %s)\n", modelName, aiGetErrorString());
        return NULL;
    }

    if (strlen(modelName) >= MAX_QPATH)
    {
        _printf("WARNING: Model name %s exceeds MAX_QPATH\n", modelName);
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

    int targetRes;
    if (guessUVs)
    {
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
        targetRes = (int)ceil(targetResFloat);
        if (targetRes > LIGHTMAP_WIDTH - 2) targetRes = LIGHTMAP_WIDTH - 2;
        if (targetRes < 16) targetRes = 16;
    }
    else
    {
        targetRes = (LIGHTMAP_WIDTH >= 64) ? LIGHTMAP_WIDTH : 1024;
    }

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
GenerateXAtlasUVsFromScratch

Use xatlas library to fully generate a UV map from scratch.
====================
*/
static uv_t *GenerateXAtlasUVsFromScratch(const struct aiMesh *mesh, int ssize, float lightmapScale, vec3_t scale_vec)
{
    xatlasAtlas *atlas = xatlasCreate();
    if (!atlas)
        return NULL;

    xatlasMeshDecl decl;
    xatlasMeshDeclInit(&decl);

    float *positions = malloc(sizeof(float) * 3 * mesh->mNumVertices);
    for (int i = 0; i < (int)mesh->mNumVertices; i++)
    {
        positions[i * 3 + 0] = mesh->mVertices[i].x * scale_vec[0];
        positions[i * 3 + 1] = mesh->mVertices[i].y * scale_vec[1];
        positions[i * 3 + 2] = mesh->mVertices[i].z * scale_vec[2];
    }

    uint32_t *indices = malloc(sizeof(uint32_t) * mesh->mNumFaces * 3);
    int validTris = 0;
    for (int i = 0; i < (int)mesh->mNumFaces; i++)
    {
        if (mesh->mFaces[i].mNumIndices == 3)
        {
            indices[validTris * 3 + 0] = mesh->mFaces[i].mIndices[0];
            indices[validTris * 3 + 1] = mesh->mFaces[i].mIndices[1];
            indices[validTris * 3 + 2] = mesh->mFaces[i].mIndices[2];
            validTris++;
        }
    }

    decl.vertexPositionData = positions;
    decl.vertexPositionStride = sizeof(float) * 3;
    decl.vertexCount = mesh->mNumVertices;
    decl.indexData = indices;
    decl.indexCount = validTris * 3;
    decl.indexFormat = xatlasIndexFormat_UInt32;

    xatlasAddMeshError error = xatlasAddMesh(atlas, &decl, 1);
    if (error != xatlasAddMeshError_Success)
    {
        _printf("xatlasAddMesh failed: %s\n", xatlasAddMeshErrorString(error));
        xatlasDestroy(atlas);
        free(positions);
        free(indices);
        return NULL;
    }

    xatlasAddMeshJoin(atlas);

    xatlasChartOptions chartOptions;
    xatlasChartOptionsInit(&chartOptions);
    xatlasComputeCharts(atlas, &chartOptions);

    xatlasPackOptions packOptions;
    xatlasPackOptionsInit(&packOptions);
    packOptions.padding = 2; // 2 texels of padding

    int targetRes;
    if (guessUVs)
    {
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
        targetRes = (int)ceil(targetResFloat);
        if (targetRes > LIGHTMAP_WIDTH - 2) targetRes = LIGHTMAP_WIDTH - 2;
        if (targetRes < 16) targetRes = 16;
    }
    else
    {
        targetRes = (LIGHTMAP_WIDTH >= 64) ? LIGHTMAP_WIDTH : 1024;
    }

    packOptions.resolution = targetRes;
    packOptions.texelsPerUnit = 0.0f;
    xatlasPackCharts(atlas, &packOptions);

    if (atlas->meshCount == 0 || atlas->width == 0 || atlas->height == 0)
    {
        xatlasDestroy(atlas);
        free(positions);
        free(indices);
        return NULL;
    }

    uv_t *outUVs = calloc(mesh->mNumFaces * 3, sizeof(uv_t));
    xatlasMesh *xMesh = &atlas->meshes[0];

    int validIdx = 0;
    for (int i = 0; i < (int)mesh->mNumFaces; i++)
    {
        if (mesh->mFaces[i].mNumIndices == 3)
        {
            for (int v = 0; v < 3; v++)
            {
                uint32_t xIdx = xMesh->indexArray[validIdx * 3 + v];
                xatlasVertex *xv = &xMesh->vertexArray[xIdx];

                outUVs[i * 3 + v].u = xv->uv[0] / (float)atlas->width;
                outUVs[i * 3 + v].v = xv->uv[1] / (float)atlas->height;
            }
            validIdx++;
        }
    }

    xatlasDestroy(atlas);
    free(positions);
    free(indices);
    return outUVs;
}


/*
====================
LoadTriangleModels

Initial pass to load and transform all misc_model entities.
====================
*/
void LoadTriangleModels(entity_t *eparent)
{
    int entity_num;
    entity_t *entity;
    const char *model;
    const struct aiScene *scene;
    vec3_t origin, angles;
    float scale;
    vec3_t scale_vec;
    float rotationMatrix[3][3];

    const char *modelGroup;
    if (eparent == &entities[0])
    {
        modelGroup = "";
    }
    else
    {
        modelGroup = ValueForKey(eparent, "modelgroup");
        if (!modelGroup[0])
        {
            modelGroup = ValueForKey(eparent, "modelsgroup");
        }

        if (!modelGroup[0])
        {
            return;
        }

        // Check for duplicate modelgroup in other brushmodels
        for (int i = 1; i < num_entities; i++)
        {
            entity_t *other = &entities[i];
            if (other == eparent) continue;
            
            // Only check other brushmodels (entities with brushes or patches)
            if (!other->brushes && !other->patches) continue;

            const char *otherGroup = ValueForKey(other, "modelgroup");
            if (!otherGroup[0])
            {
                otherGroup = ValueForKey(other, "modelsgroup");
            }

            if (otherGroup[0] && !Q_stricmp(otherGroup, modelGroup))
            {
                Error("Duplicate modelgroup '%s' found on multiple brushmodel entities", modelGroup);
            }
        }
    }

    for (entity_num = 1; entity_num < num_entities; entity_num++)
    {
        entity = &entities[entity_num];
        const char *classname = ValueForKey(entity, "classname");

        if (!Q_stricmp("misc_model", classname))
        {
            const char *entGroup = ValueForKey(entity, "modelgroup");
            if (!entGroup[0])
            {
                entGroup = ValueForKey(entity, "modelsgroup");
            }

            if (Q_stricmp(entGroup, modelGroup))
            {
                continue;
            }

            model = ValueForKey(entity, "model");
            if (!model[0])
                continue;

            GetVectorForKey(entity, "origin", origin);
            if (eparent != &entities[0])
            {
                vec3_t parentOrigin;
                GetVectorForKey(eparent, "origin", parentOrigin);
                VectorSubtract(origin, parentOrigin, origin);
            }

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
            const char *radStr = ValueForKey(entity, "smooth");
            if (radStr[0])
            {
                smoothingRadius = atof(radStr);
            }

            // vertexcolor override for all surfaces of this model instance
            int overrideVertexColor = 0;
            vec3_t vertexColor;
            VectorClear(vertexColor);
            const char *vcolStr = ValueForKey(entity, "vertexcolor");
            if (vcolStr[0])
            {
                overrideVertexColor = 1;
                ParseColor(vcolStr, vertexColor);
            }

            int upscale = 0;
            const char *upscaleStr = ValueForKey(entity, "upscale");
            if (upscaleStr[0])
            {
                upscale = atoi(upscaleStr);
            }

            int castShadows = -1;
            const char *csStr = ValueForKey(entity, "castshadows");
            if (!csStr[0]) csStr = ValueForKey(entity, "cs"); // alias
            if (csStr[0])
            {
                castShadows = atoi(csStr);
            }

            // supersample override
            float superSampleRadius = -1.0f;
            const char *ssStr = ValueForKey(entity, "supersample");
            if (ssStr[0])
            {
                superSampleRadius = atof(ssStr);
                if (superSampleRadius < 0.0f)
                    superSampleRadius = 0.0f;
            }

            inst->lightmapScale = 1.0f;
            const char *ent_scale_str = ValueForKey(entity, "lightmapscale");
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

            int forceUVGen = game->forceUVGen;
            const char *world_forceuv_str = ValueForKey(&entities[0], "forceuvgen");
            if (world_forceuv_str[0])
                forceUVGen = atoi(world_forceuv_str);

            const char *forceuv_str = ValueForKey(entity, "forceuvgen");
            if (forceuv_str[0])
                forceUVGen = atoi(forceuv_str);

            inst->has_collision_type_override = qfalse;
            const char *col_type_str = ValueForKey(entity, "collisiontype");
            
            if (col_type_str[0])
            {
                inst->has_collision_type_override = qtrue;
                if (!Q_stricmp(col_type_str, "shell")) inst->collision_type_override = MC_SHELL;
                else if (!Q_stricmp(col_type_str, "object")) inst->collision_type_override = MC_OBJECT;
                else if (!Q_stricmp(col_type_str, "walkable")) inst->collision_type_override = MC_WALKABLE;
                else if (!Q_stricmp(col_type_str, "wrap")) inst->collision_type_override = MC_WRAP;
                else if (!Q_stricmp(col_type_str, "extrude")) inst->collision_type_override = MC_EXTRUDE;
                else if (!Q_stricmp(col_type_str, "terrain")) inst->collision_type_override = MC_TERRAIN;
                else if (!Q_stricmp(col_type_str, "none") || !Q_stricmp(col_type_str, "nosolid") || !Q_stricmp(col_type_str, "nonsolid")) inst->collision_type_override = MC_NONE;
                else
                {
                    _printf("WARNING: Unknown collisiontype '%s' on misc_model, falling back to auto\n", col_type_str);
                    inst->has_collision_type_override = qfalse;
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
                // Misc_Model Light Tag Processing
                // ==========================================
                if (!Q_stricmp(shaderName, "tag_light"))
                {
                    const char *type = ValueForKey(entity, "lighttype");
                    qboolean isPoint = qfalse;
                    if (!Q_stricmp(type, "point") || !Q_stricmp(type, "pointlight"))
                    {
                        isPoint = qtrue;
                    }

                    for (int j = 0; j < (int)mesh->mNumFaces; j++)
                    {
                        if (mesh->mFaces[j].mNumIndices != 3)
                            continue;

                        vec3_t v[3];
                        for (int k = 0; k < 3; k++)
                        {
                            int idx = mesh->mFaces[j].mIndices[k];
                            float mx = mesh->mVertices[idx].x * scale_vec[0];
                            float my = mesh->mVertices[idx].y * scale_vec[1];
                            float mz = mesh->mVertices[idx].z * scale_vec[2];

                            // Axis Swap (Assimp Y-Up -> Quake Z-Up)
                            vec3_t tx;
                            tx[0] = mx; tx[1] = -mz; tx[2] = my;

                            // Rotation & Translation
                            v[k][0] = origin[0] + (tx[0] * rotationMatrix[0][0] + tx[1] * rotationMatrix[1][0] + tx[2] * rotationMatrix[2][0]);
                            v[k][1] = origin[1] + (tx[0] * rotationMatrix[0][1] + tx[1] * rotationMatrix[1][1] + tx[2] * rotationMatrix[2][1]);
                            v[k][2] = origin[2] + (tx[0] * rotationMatrix[0][2] + tx[1] * rotationMatrix[1][2] + tx[2] * rotationMatrix[2][2]);
                        }

                        // Calculate edge lengths
                        vec3_t e0, e1, e2;
                        VectorSubtract(v[1], v[0], e0);
                        VectorSubtract(v[2], v[1], e1);
                        VectorSubtract(v[0], v[2], e2);

                        float len0 = VectorLength(e0);
                        float len1 = VectorLength(e1);
                        float len2 = VectorLength(e2);

                        vec3_t vBase1, vBase2, vTip;
                        if (len0 <= len1 && len0 <= len2)
                        {
                            VectorCopy(v[0], vBase1);
                            VectorCopy(v[1], vBase2);
                            VectorCopy(v[2], vTip);
                        }
                        else if (len1 <= len0 && len1 <= len2)
                        {
                            VectorCopy(v[1], vBase1);
                            VectorCopy(v[2], vBase2);
                            VectorCopy(v[0], vTip);
                        }
                        else
                        {
                            VectorCopy(v[2], vBase1);
                            VectorCopy(v[0], vBase2);
                            VectorCopy(v[1], vTip);
                        }

                        vec3_t baseDir, vecToTip;
                        VectorSubtract(vBase2, vBase1, baseDir);
                        VectorNormalize(baseDir, baseDir);
                        VectorSubtract(vTip, vBase1, vecToTip);
                        
                        float projLen = DotProduct(vecToTip, baseDir);
                        vec3_t lightOrigin;
                        VectorMA(vBase1, projLen, baseDir, lightOrigin);

                        vec3_t lightDir;
                        VectorSubtract(vTip, lightOrigin, lightDir);
                        VectorNormalize(lightDir, lightDir);

                        SpawnLightEntity(lightOrigin, lightDir, isPoint, entity, "tag_light");
                    }

                    // Discard mesh from visual and collision generation
                    continue;
                }

                // ==========================================
                // UV Automatic Spreading
                // ==========================================
                uv_t *xatlasUVs = NULL;
                int uvChannel = (mesh->mTextureCoords[1]) ? 1 : 0;

                int ssize = samplesize;
                if (si && si->lightmapSampleSize > 0)
                    ssize = si->lightmapSampleSize;
                
                // Fast mode: ignore requests for higher resolution than the compilation setting
                if (g_fast && ssize < samplesize)
                    ssize = samplesize;

                // Apply maxSampleSize floor (trisoup: use exact fractional value, rounded up to int for xatlas)
                if (!g_fast && si && si->maxSampleSize > 0.0f && si->maxSampleSize < (float)ssize)
                    ssize = (int)ceil(si->maxSampleSize);

                if (mesh->mTextureCoords[uvChannel] && !forceUVGen)
                {
                    xatlasUVs = TryXAtlasUVs(mesh, uvChannel, ssize, inst->lightmapScale, scale_vec);
                }
                
                if (!xatlasUVs)
                {
                    if (forceUVGen)
                        _printf("Model %s (mesh %d) forcing UV generation from scratch...\n", model, i);
                    else
                        _printf("Mesh missing or invalid UVs for model %s (mesh %d). Generating entirely new UVs from scratch...\n", model, i);
                    xatlasUVs = GenerateXAtlasUVsFromScratch(mesh, ssize, inst->lightmapScale, scale_vec);
                }

                if (!xatlasUVs)
                    _printf("WARNING: Total xatlas generation failure.\n");

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
                    
                    memset(ds->decalgroup, 0, sizeof(ds->decalgroup));
                    strncpy(ds->decalgroup, ValueForKey(entity, "decalgroup"), sizeof(ds->decalgroup) - 1);

                    ds->superSampleRadius = superSampleRadius;
                    ds->smoothingRadius = smoothingRadius;
                    
                    float globalSmooth = game->defaultSmoothRadius;
                    const char *wsSmooth = ValueForKey(&entities[0], "smooth");
                    if (wsSmooth[0])
                        globalSmooth = atof(wsSmooth);
                    
                    if (ds->smoothingRadius < 0.0f && si && si->minSmoothRadius >= 0.0f && si->minSmoothRadius > globalSmooth)
                        ds->smoothingRadius = si->minSmoothRadius;
                        
                    ds->overrideVertexColor = overrideVertexColor;
                    ds->upscale = upscale;
                    ds->castShadows = castShadows;
                    if (overrideVertexColor)
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

                    // Fast mode: ignore requests for higher resolution than the compilation setting
                    if (g_fast && ds->samplesize < samplesize)
                    {
                        ds->samplesize = samplesize;
                    }
                    
                    // Apply maxSampleSize floor (trisoup: exact fractional value)
                    if (!g_fast && si && si->maxSampleSize > 0.0f && si->maxSampleSize < ds->samplesize)
                    {
                        ds->samplesize = si->maxSampleSize;
                    }

                    // Entity-level lightmapscale for models
                    ds->lightmapScale = inst->lightmapScale;

                    _printf("Final samplesize for misc_model: %.1f, lightmapScale: %.2f\n", ds->samplesize, ds->lightmapScale);

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
                                    // Original UVs
                                    if (mesh->mTextureCoords[uvChannel])
                                    {
                                        dv->lightmap[0][0] = mesh->mTextureCoords[uvChannel][oldIdx].x;
                                        dv->lightmap[0][1] = mesh->mTextureCoords[uvChannel][oldIdx].y;
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
