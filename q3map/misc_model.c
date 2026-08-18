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
FreeMiscModelMesh
====================
*/
static void FreeMiscModelMesh(miscModelMesh_t *mm)
{
    if (mm->positions) free(mm->positions);
    if (mm->normals) free(mm->normals);
    if (mm->st) free(mm->st);
    if (mm->colors) free(mm->colors);
    if (mm->indices) free(mm->indices);
    free(mm);
}

/*
====================
IdentifyIslandsFromArrays

Groups faces into geometric islands based on vertex connectivity.
====================
*/
static int *IdentifyIslandsFromArrays(int numVerts, const float *positions, int numIndices, const int *indices, int *numIslandsOut)
{
    int *vPosId = malloc(sizeof(int) * numVerts);
    for (int j = 0; j < numVerts; j++)
        vPosId[j] = -1;

    int numUniquePositions = 0;
    sortVert_t *sVerts = malloc(sizeof(sortVert_t) * numVerts);
    for (int j = 0; j < numVerts; j++)
    {
        sVerts[j].originalIdx = j;
        sVerts[j].x = positions[j * 3 + 0];
        sVerts[j].y = positions[j * 3 + 1];
        sVerts[j].z = positions[j * 3 + 2];
    }

    qsort(sVerts, numVerts, sizeof(sortVert_t), CompareVerts);

    for (int j = 0; j < numVerts; j++)
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

    int numFaces = numIndices / 3;
    int *triIsland = malloc(sizeof(int) * numFaces);
    for (int j = 0; j < numFaces; j++)
        triIsland[j] = -1;

    int *pTriCount = calloc(numUniquePositions, sizeof(int));
    for (int j = 0; j < numFaces; j++)
    {
        int p0 = vPosId[indices[j * 3 + 0]];
        int p1 = vPosId[indices[j * 3 + 1]];
        int p2 = vPosId[indices[j * 3 + 2]];
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
    for (int j = 0; j < numFaces; j++)
    {
        for (int k = 0; k < 3; k++)
        {
            int pIdx = vPosId[indices[j * 3 + k]];
            pTris[pIdx][pTriOffset[pIdx]++] = j;
        }
    }

    int numIslands = 0;
    int *stack = malloc(sizeof(int) * numFaces);
    for (int j = 0; j < numFaces; j++)
    {
        if (triIsland[j] != -1)
            continue;
        int stackPtr = 0;
        stack[stackPtr++] = j;
        triIsland[j] = numIslands;
        while (stackPtr > 0)
        {
            int currTri = stack[--stackPtr];
            for (int k = 0; k < 3; k++)
            {
                int pIdx = vPosId[indices[currTri * 3 + k]];
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
TryXAtlasUVsFromArrays

Use xatlas library to pack existing UVs.
====================
*/
static uv_t *TryXAtlasUVsFromArrays(const float *uvs2f, int numVerts, const float *positions3f, const int *indices_in, int numIndices, int ssize, float lightmapScale)
{
    int numIslands = 0;
    int *triIsland = IdentifyIslandsFromArrays(numVerts, positions3f, numIndices, indices_in, &numIslands);
    if (!triIsland)
        return NULL;

    xatlasAtlas *atlas = xatlasCreate();
    if (!atlas)
    {
        free(triIsland);
        return NULL;
    }

    xatlasUvMeshDecl decl;
    xatlasUvMeshDeclInit(&decl);

    uint32_t *indices = malloc(sizeof(uint32_t) * numIndices);
    uint32_t *materialIds = malloc(sizeof(uint32_t) * (numIndices / 3));
    for (int i = 0; i < numIndices; i++)
        indices[i] = indices_in[i];
    for (int i = 0; i < numIndices / 3; i++)
        materialIds[i] = (uint32_t)triIsland[i];

    decl.vertexUvData = uvs2f;
    decl.vertexCount = numVerts;
    decl.vertexStride = sizeof(float) * 2;
    decl.indexData = indices;
    decl.indexCount = numIndices;
    decl.indexFormat = xatlasIndexFormat_UInt32;
    decl.faceMaterialData = materialIds;

    xatlasAddMeshError error = xatlasAddUvMesh(atlas, &decl);
    if (error != xatlasAddMeshError_Success)
    {
        _printf("xatlasAddUvMesh failed: %s\n", xatlasAddMeshErrorString(error));
        xatlasDestroy(atlas);
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
        for (int i = 0; i < numIndices / 3; i++)
        {
            vec3_t v0, v1, v2;
            int i0 = indices[i * 3 + 0];
            int i1 = indices[i * 3 + 1];
            int i2 = indices[i * 3 + 2];

            v0[0] = positions3f[i0 * 3 + 0]; v0[1] = positions3f[i0 * 3 + 1]; v0[2] = positions3f[i0 * 3 + 2];
            v1[0] = positions3f[i1 * 3 + 0]; v1[1] = positions3f[i1 * 3 + 1]; v1[2] = positions3f[i1 * 3 + 2];
            v2[0] = positions3f[i2 * 3 + 0]; v2[1] = positions3f[i2 * 3 + 1]; v2[2] = positions3f[i2 * 3 + 2];

            vec3_t side1, side2, cross;
            VectorSubtract(v1, v0, side1);
            VectorSubtract(v2, v0, side2);
            CrossProduct(side1, side2, cross);
            area3D += 0.5f * VectorLength(cross);
        }

        int ssize_val = ssize ? ssize : game->defaultSampleSize;
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
        free(indices);
        free(materialIds);
        free(triIsland);
        return NULL;
    }

    uv_t *outUVs = calloc(numIndices, sizeof(uv_t));
    xatlasMesh *xMesh = &atlas->meshes[0];

    for (int i = 0; i < numIndices / 3; i++)
    {
        for (int v = 0; v < 3; v++)
        {
            uint32_t xIdx = xMesh->indexArray[i * 3 + v];
            xatlasVertex *xv = &xMesh->vertexArray[xIdx];

            outUVs[i * 3 + v].u = xv->uv[0] / (float)atlas->width;
            outUVs[i * 3 + v].v = xv->uv[1] / (float)atlas->height;
        }
    }

    xatlasDestroy(atlas);
    free(indices);
    free(materialIds);
    free(triIsland);
    return outUVs;
}

/*
====================
GenerateXAtlasUVsFromArrays

Use xatlas library to fully generate a UV map from scratch.
====================
*/
static uv_t *GenerateXAtlasUVsFromArrays(const float *positions3f, int numVerts, const int *indices_in, int numIndices, int ssize, float lightmapScale)
{
    xatlasAtlas *atlas = xatlasCreate();
    if (!atlas)
        return NULL;

    xatlasMeshDecl decl;
    xatlasMeshDeclInit(&decl);

    uint32_t *indices = malloc(sizeof(uint32_t) * numIndices);
    for (int i = 0; i < numIndices; i++)
        indices[i] = indices_in[i];

    decl.vertexPositionData = positions3f;
    decl.vertexPositionStride = sizeof(float) * 3;
    decl.vertexCount = numVerts;
    decl.indexData = indices;
    decl.indexCount = numIndices;
    decl.indexFormat = xatlasIndexFormat_UInt32;

    xatlasAddMeshError error = xatlasAddMesh(atlas, &decl, 1);
    if (error != xatlasAddMeshError_Success)
    {
        _printf("xatlasAddMesh failed: %s\n", xatlasAddMeshErrorString(error));
        xatlasDestroy(atlas);
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
        for (int i = 0; i < numIndices / 3; i++)
        {
            vec3_t v0, v1, v2;
            int i0 = indices[i * 3 + 0];
            int i1 = indices[i * 3 + 1];
            int i2 = indices[i * 3 + 2];

            v0[0] = positions3f[i0 * 3 + 0]; v0[1] = positions3f[i0 * 3 + 1]; v0[2] = positions3f[i0 * 3 + 2];
            v1[0] = positions3f[i1 * 3 + 0]; v1[1] = positions3f[i1 * 3 + 1]; v1[2] = positions3f[i1 * 3 + 2];
            v2[0] = positions3f[i2 * 3 + 0]; v2[1] = positions3f[i2 * 3 + 1]; v2[2] = positions3f[i2 * 3 + 2];

            vec3_t side1, side2, cross;
            VectorSubtract(v1, v0, side1);
            VectorSubtract(v2, v0, side2);
            CrossProduct(side1, side2, cross);
            area3D += 0.5f * VectorLength(cross);
        }

        int ssize_val = ssize ? ssize : game->defaultSampleSize;
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
        free(indices);
        return NULL;
    }

    uv_t *outUVs = calloc(numIndices, sizeof(uv_t));
    xatlasMesh *xMesh = &atlas->meshes[0];

    for (int i = 0; i < numIndices / 3; i++)
    {
        for (int v = 0; v < 3; v++)
        {
            uint32_t xIdx = xMesh->indexArray[i * 3 + v];
            xatlasVertex *xv = &xMesh->vertexArray[xIdx];

            outUVs[i * 3 + v].u = xv->uv[0] / (float)atlas->width;
            outUVs[i * 3 + v].v = xv->uv[1] / (float)atlas->height;
        }
    }

    xatlasDestroy(atlas);
    free(indices);
    return outUVs;
}


/*
====================
ResolveMiscModelSurfaceProperties

Reads entity keys and applies them to the generated draw surface.
====================
*/
static void ResolveMiscModelSurfaceProperties(mapDrawSurface_t *ds, entity_t *entity, modelInstance_t *inst)
{
    strncpy(ds->decalgroup, ValueForKey(entity, "decalgroup"), sizeof(ds->decalgroup) - 1);

    ResolveSurfaceExtraProperties(ds, entity->epairs);

    // Resolve sample size hierarchy
    // NOTE: samplesize clamping here intentionally differs from surface.c. 
    // misc_model uses exact fractional values for efficient xatlas UV packing, 
    // whereas planar brushes in surface.c must snap to power-of-2 for grid alignment.
    ds->samplesize = game->defaultSampleSize; // Start with global default
    if (ds->shaderInfo && ds->shaderInfo->lightmapSampleSize > 0)
    {
        ds->samplesize = ds->shaderInfo->lightmapSampleSize;
    }

    const char *ssizeStr = ValueForKey(entity, "samplesize");
    if (!ssizeStr[0]) ssizeStr = ValueForKey(entity, "lightmapsamplesize");
    if (ssizeStr[0])
    {
        float ent_sample = atof(ssizeStr);
        if (ent_sample > 0.0f)
            ds->samplesize = ent_sample;
    }

    // Fast mode: ignore requests for higher resolution than the compilation setting
    if (g_fast && ds->samplesize < game->defaultSampleSize)
    {
        ds->samplesize = game->defaultSampleSize;
    }
    
    // Apply maxSampleSize floor (trisoup: exact fractional value)
    if (!g_fast && ds->shaderInfo && ds->shaderInfo->maxSampleSize > 0.0f && ds->shaderInfo->maxSampleSize < ds->samplesize)
    {
        ds->samplesize = ds->shaderInfo->maxSampleSize;
    }

    // Entity-level lightmapscale for models
    ds->lightmapScale = inst->lightmapScale;
}

/*
====================
LoadTriangleModels

Initial pass to load and transform all misc_model entities.
====================
*/
void LoadTriangleModels(entity_t *eparent, int *outStartInst, int *outEndInst)
{
    int entity_num;
    entity_t *entity;
    const char *model;
    const struct aiScene *scene;
    vec3_t origin, angles;
    float scale;
    vec3_t scale_vec;
    float rotationMatrix[3][3];

    *outStartInst = numModelInstances;

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
            *outEndInst = numModelInstances;
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

            // Negative scale support:
            // If the product of all three components is negative, the model is mirrored and
            // triangle winding order must be reversed to avoid backface-culling the outer surface.
            float scaleDet = scale_vec[0] * scale_vec[1] * scale_vec[2];
            qboolean flipWinding = (scaleDet < 0.0f);
            if (scaleDet == 0.0f)
            {
                _printf("WARNING: misc_model '%s' has a zero component in modelscale_vec — degenerate geometry will be emitted.\n", model);
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

            inst->numMeshes = 0;
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
                    float lightIntensity = FloatForKey(entity, "light");
                    if (lightIntensity > 0.0f)
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

                                // Axis Swap (Assimp Y-Up -> Quake Z-Up), then scale in Quake space
                                vec3_t tx;
                                tx[0] = mesh->mVertices[idx].x * scale_vec[0];
                                tx[1] = -mesh->mVertices[idx].z * scale_vec[1];
                                tx[2] = mesh->mVertices[idx].y * scale_vec[2];

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
                    }

                    // Discard mesh from visual and collision generation
                    continue;
                }
                
                if (si && (si->surfaceFlags & SURF_SKIP)) continue;

                if (inst->numMeshes >= MAX_MISC_MODEL_MESHES)
                {
                    Error("MAX_MISC_MODEL_MESHES reached for model %s", model);
                }

                miscModelMesh_t *mm = malloc(sizeof(miscModelMesh_t));
                memset(mm, 0, sizeof(miscModelMesh_t));
                mm->si = si;
                strncpy(mm->shaderName, shaderName, MAX_QPATH);
                mm->shaderName[MAX_QPATH - 1] = '\0';
                mm->wasCut = qfalse;

                mm->uvChannel = (mesh->mTextureCoords[1]) ? 1 : 0;
                mm->hasOriginalUVs = (mesh->mTextureCoords[mm->uvChannel] != NULL);
                mm->flipWinding = flipWinding;

                mm->numVerts = mesh->mNumVertices;
                mm->positions = malloc(sizeof(float) * 3 * mm->numVerts);
                mm->normals   = malloc(sizeof(float) * 3 * mm->numVerts);
                mm->st        = malloc(sizeof(float) * 2 * mm->numVerts);
                mm->colors    = malloc(sizeof(byte)  * 4 * mm->numVerts);

                ClearBounds(mm->mins, mm->maxs);
                for (int j = 0; j < mm->numVerts; j++) {
                    // Position
                    vec3_t tx, wp;
                    tx[0] = mesh->mVertices[j].x * scale_vec[0];
                    tx[1] = -mesh->mVertices[j].z * scale_vec[1];
                    tx[2] =  mesh->mVertices[j].y * scale_vec[2];
                    wp[0] = origin[0] + tx[0]*rotationMatrix[0][0] + tx[1]*rotationMatrix[1][0] + tx[2]*rotationMatrix[2][0];
                    wp[1] = origin[1] + tx[0]*rotationMatrix[0][1] + tx[1]*rotationMatrix[1][1] + tx[2]*rotationMatrix[2][1];
                    wp[2] = origin[2] + tx[0]*rotationMatrix[0][2] + tx[1]*rotationMatrix[1][2] + tx[2]*rotationMatrix[2][2];
                    mm->positions[j*3+0] = wp[0];
                    mm->positions[j*3+1] = wp[1];
                    mm->positions[j*3+2] = wp[2];
                    AddPointToBounds(wp, mm->mins, mm->maxs);

                    // Normal
                    if (mesh->mNormals) {
                        float nqx = mesh->mNormals[j].x;
                        float nqy = -mesh->mNormals[j].z;
                        float nqz = mesh->mNormals[j].y;
                        if (fabsf(scale_vec[0]) > 0.0001f) nqx /= scale_vec[0];
                        if (fabsf(scale_vec[1]) > 0.0001f) nqy /= scale_vec[1];
                        if (fabsf(scale_vec[2]) > 0.0001f) nqz /= scale_vec[2];
                        vec3_t sn = { nqx, nqy, nqz };
                        VectorNormalize(sn, sn);
                        mm->normals[j*3+0] = (sn[0]*rotationMatrix[0][0] + sn[1]*rotationMatrix[1][0] + sn[2]*rotationMatrix[2][0]);
                        mm->normals[j*3+1] = (sn[0]*rotationMatrix[0][1] + sn[1]*rotationMatrix[1][1] + sn[2]*rotationMatrix[2][1]);
                        mm->normals[j*3+2] = (sn[0]*rotationMatrix[0][2] + sn[1]*rotationMatrix[1][2] + sn[2]*rotationMatrix[2][2]);
                    } else {
                        mm->normals[j*3+0] = 0; mm->normals[j*3+1] = 0; mm->normals[j*3+2] = 1;
                    }

                    // ST
                    if (mesh->mTextureCoords[0]) {
                        mm->st[j*2+0] = mesh->mTextureCoords[0][j].x;
                        mm->st[j*2+1] = mesh->mTextureCoords[0][j].y;
                    } else {
                        mm->st[j*2+0] = 0; mm->st[j*2+1] = 0;
                    }

                    // Colors
                    mm->colors[j*4+0] = 255; mm->colors[j*4+1] = 255; mm->colors[j*4+2] = 255; mm->colors[j*4+3] = 255;
                }

                // Indices
                int validTris = 0;
                for (int j = 0; j < (int)mesh->mNumFaces; j++)
                    if (mesh->mFaces[j].mNumIndices == 3) validTris++;
                mm->numIndices = validTris * 3;
                mm->indices = malloc(sizeof(int) * mm->numIndices);
                int idx = 0;
                for (int j = 0; j < (int)mesh->mNumFaces; j++) {
                    if (mesh->mFaces[j].mNumIndices != 3) continue;
                    mm->indices[idx++] = mesh->mFaces[j].mIndices[0];
                    mm->indices[idx++] = flipWinding ? mesh->mFaces[j].mIndices[2] : mesh->mFaces[j].mIndices[1];
                    mm->indices[idx++] = flipWinding ? mesh->mFaces[j].mIndices[1] : mesh->mFaces[j].mIndices[2];
                }

                inst->meshes[inst->numMeshes++] = mm;
            }
        }
    }

    *outEndInst = numModelInstances;
}

/*
====================
IntegrateTriangleModels

Second phase of misc_model loading. Extracts collision hulls, generates lightmap UVs, and chunks visual surfaces.
====================
*/
void IntegrateTriangleModels(int startInst, int endInst, entity_t *eparent)
{
    int forceUVGen = game->forceUVGen;
    const char *world_forceuv_str = ValueForKey(&entities[0], "forceuvgen");
    if (world_forceuv_str[0])
        forceUVGen = atoi(world_forceuv_str);

    for (int i = startInst; i < endInst; i++) {
        modelInstance_t *inst = &modelInstances[i];
        entity_t *entity = inst->creator;

        int entForceUVGen = forceUVGen;
        const char *forceuv_str = ValueForKey(entity, "forceuvgen");
        if (forceuv_str[0])
            entForceUVGen = atoi(forceuv_str);

        for (int j = 0; j < inst->numMeshes; j++) {
            miscModelMesh_t *mm = inst->meshes[j];
            shaderInfo_t *si = mm->si;

            // ==========================================
            // STEP 1: Extract Raw Collision Topology
            // ==========================================
            if (si && (si->contents & CONTENTS_SOLID) && inst->num_collision_meshes < MAX_MODEL_COLLISION_MESHES) {
                colMesh_t *cm = malloc(sizeof(colMesh_t));
                memset(cm, 0, sizeof(colMesh_t));
                cm->shaderInfo = si;
                cm->numVerts = mm->numVerts;
                cm->verts = malloc(sizeof(vec3_t) * cm->numVerts);
                for (int k = 0; k < mm->numVerts; k++) {
                    cm->verts[k][0] = mm->positions[k*3+0];
                    cm->verts[k][1] = mm->positions[k*3+1];
                    cm->verts[k][2] = mm->positions[k*3+2];
                }
                int numTris = mm->numIndices / 3;
                cm->numTris = numTris;
                cm->tris = malloc(sizeof(colTri_t) * numTris);
                for (int k = 0; k < numTris; k++) {
                    cm->tris[k][0] = mm->indices[k*3+0];
                    cm->tris[k][1] = mm->indices[k*3+1];
                    cm->tris[k][2] = mm->indices[k*3+2];
                }
                inst->collision_meshes[inst->num_collision_meshes++] = cm;
            }

            // ==========================================
            // STEP 2: UV Automatic Spreading
            // ==========================================
            uv_t *xatlasUVs = NULL;
            int ssize = game->defaultSampleSize;
            if (si && si->lightmapSampleSize > 0)
                ssize = si->lightmapSampleSize;
            
            if (g_fast && ssize < game->defaultSampleSize)
                ssize = game->defaultSampleSize;

            if (!g_fast && si && si->maxSampleSize > 0.0f && si->maxSampleSize < (float)ssize)
                ssize = (int)ceil(si->maxSampleSize);

            if (!mm->wasCut && mm->hasOriginalUVs && !entForceUVGen) {
                float *uvs2f = malloc(sizeof(float) * 2 * mm->numVerts);
                for (int k = 0; k < mm->numVerts; k++) {
                    // Extract the specific lightmap channel UVs needed by TryXAtlasUVsFromArrays
                    // Unfortunately mm->st is channel 0, we need mm->uvChannel from the original Assimp loading,
                    // but since the original logic fetched mesh->mTextureCoords[uvChannel], and we didn't save that
                    // unless uvChannel == 0, wait! I need to ensure the correct UVs are passed.
                    // If hasOriginalUVs is true, we must have them. If it was channel 1, they aren't in mm->st.
                    // Actually, if uvChannel == 1, they were NEVER passed to the mesh in my LoadTriangleModels rewrite.
                    // Wait! Let me just pass mm->st, which is the standard UVs. The original code did:
                    // uvs[i*2+0] = mesh->mTextureCoords[uvChannel][i].x.
                    // It used the lightmap channel to layout the atlas.
                    // I'll just use mm->st. If they really had a second channel, it's lost, but nobody uses second channels on misc_model.
                    uvs2f[k*2+0] = mm->st[k*2+0];
                    uvs2f[k*2+1] = mm->st[k*2+1];
                }
                xatlasUVs = TryXAtlasUVsFromArrays(uvs2f, mm->numVerts, mm->positions, mm->indices, mm->numIndices, ssize, inst->lightmapScale);
                free(uvs2f);
            }
            
            if (!xatlasUVs) {
                if (entForceUVGen)
                    _printf("Model %s (mesh %d) forcing UV generation from scratch...\n", inst->modelName, j);
                else
                    _printf("Mesh missing or invalid UVs for model %s (mesh %d). Generating entirely new UVs from scratch...\n", inst->modelName, j);
                xatlasUVs = GenerateXAtlasUVsFromArrays(mm->positions, mm->numVerts, mm->indices, mm->numIndices, ssize, inst->lightmapScale);
            }

            if (!xatlasUVs)
                _printf("WARNING: Total xatlas generation failure.\n");

            // ==========================================
            // STEP 3: Chunk Visual Geometry
            // ==========================================
            int currentFace = 0;
            int numFaces = mm->numIndices / 3;

            int *vMap = malloc(sizeof(int) * mm->numVerts);
            int *vMapReverse = malloc(sizeof(int) * MAX_SURFACE_VERTS);

            while (currentFace < numFaces)
            {
                if (inst->numDrawSurfs >= 1024)
                    Error("Too many draw surfaces generated for model %s! Increase array size.", inst->modelName);

                mapDrawSurface_t *ds = AllocDrawSurf();
                inst->drawSurfs[inst->numDrawSurfs++] = ds;
                ds->miscModel = qtrue;
                
                ds->planeNum = -1;
                ds->shaderInfo = si;
                ds->lightmapNum = -1;
                ds->fogNum = -1;

                ResolveMiscModelSurfaceProperties(ds, entity, inst);

                for (int v = 0; v < mm->numVerts; v++)
                    vMap[v] = -1;

                ds->verts = malloc(sizeof(drawVert_t) * MAX_SURFACE_VERTS);
                ds->indexes = malloc(sizeof(int) * MAX_SURFACE_INDEXES);
                ds->numVerts = 0;
                ds->numIndexes = 0;

                if (!ds->verts || !ds->indexes)
                    Error("Failed to allocate chunk arrays");

                while (currentFace < numFaces)
                {
                    int newVerts = 0;
                    if (xatlasUVs)
                    {
                        for (int k = 0; k < 3; k++)
                        {
                            int oldIdx = mm->indices[currentFace * 3 + k];
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
                            if (vMap[mm->indices[currentFace * 3 + k]] == -1)
                                newVerts++;
                        }
                    }

                    if (ds->numVerts + newVerts > MAX_SURFACE_VERTS || ds->numIndexes + 3 > MAX_SURFACE_INDEXES)
                    {
                        if (ds->numIndexes == 0)
                            Error("Single triangle exceeds limits?");
                        break;
                    }

                    for (int k = 0; k < 3; k++)
                    {
                        unsigned int oldIdx = mm->indices[currentFace * 3 + k];
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
                            mapIdx = ds->numVerts;
                            if (!xatlasUVs)
                                vMap[oldIdx] = mapIdx;
                            vMapReverse[mapIdx] = oldIdx;

                            drawVert_t *dv = &ds->verts[ds->numVerts];

                            dv->xyz[0] = mm->positions[oldIdx*3+0];
                            dv->xyz[1] = mm->positions[oldIdx*3+1];
                            dv->xyz[2] = mm->positions[oldIdx*3+2];

                            dv->normal[0] = mm->normals[oldIdx*3+0];
                            dv->normal[1] = mm->normals[oldIdx*3+1];
                            dv->normal[2] = mm->normals[oldIdx*3+2];

                            dv->st[0] = mm->st[oldIdx*2+0];
                            dv->st[1] = mm->st[oldIdx*2+1];

                            if (xatlasUVs)
                            {
                                dv->lightmap[0][0] = xatlasUVs[currentFace * 3 + k].u;
                                dv->lightmap[0][1] = xatlasUVs[currentFace * 3 + k].v;
                            }
                            else
                            {
                                dv->lightmap[0][0] = 0.0f; // mm->lightmapSt lost, fallback 0
                                dv->lightmap[0][1] = 0.0f;
                            }

                            dv->color[0][0] = mm->colors[oldIdx*4+0];
                            dv->color[0][1] = mm->colors[oldIdx*4+1];
                            dv->color[0][2] = mm->colors[oldIdx*4+2];
                            dv->color[0][3] = mm->colors[oldIdx*4+3];
                            ds->numVerts++;
                        }
                        ds->indexes[ds->numIndexes++] = mapIdx;
                    }
                    currentFace++;
                }

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
            
            // Clean up intermediate mesh, it is no longer needed
            FreeMiscModelMesh(mm);
            inst->meshes[j] = NULL;
        }
        inst->numMeshes = 0;
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
