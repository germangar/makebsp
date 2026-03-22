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


int c_triangleModels;
int c_triangleSurfaces;
int c_triangleVertexes;
int c_triangleIndexes;

#define MAX_MODEL_INSTANCES 1024
modelInstance_t modelInstances[MAX_MODEL_INSTANCES];
int numModelInstances;

typedef struct modelCache_s {
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
                          const struct aiScene *scene, char *shaderName) {
  char ext[16];
  struct aiMaterial *mat;
  struct aiString path;
  struct aiString matName;

  if (mesh->mMaterialIndex >= scene->mNumMaterials) {
    strcpy(shaderName, "default");
    return;
  }

  mat = scene->mMaterials[mesh->mMaterialIndex];

  // Step 1: Prioritize high-level texture API (Diffuse) for all formats
  if (aiGetMaterialTexture(mat, aiTextureType_DIFFUSE, 0, &path, NULL, NULL,
                           NULL, NULL, NULL, NULL) == aiReturn_SUCCESS) {
    strncpy(shaderName, path.data, MAX_QPATH - 1);
    shaderName[MAX_QPATH - 1] = '\0';
    StripExtension(shaderName);
    return;
  }

  ExtractFileExtension(modelPath, ext);

  if (!Q_stricmp(ext, "obj")) {
    if (aiGetMaterialString(mat, "$tex.file", 0, 0, &path) ==
        aiReturn_SUCCESS) {
      strncpy(shaderName, path.data, MAX_QPATH - 1);
      shaderName[MAX_QPATH - 1] = '\0';
      StripExtension(shaderName);
      return;
    }
  }

  if (aiGetMaterialString(mat, AI_MATKEY_NAME, &matName) == aiReturn_SUCCESS) {
    strncpy(shaderName, matName.data, MAX_QPATH - 1);
    shaderName[MAX_QPATH - 1] = '\0';
  } else {
    strcpy(shaderName, "default");
  }
}

/*
====================
GetCachedModel
====================
*/
static const struct aiScene *GetCachedModel(const char *modelName) {
  int i;
  char filename[1024];

  for (i = 0; i < numModelCache; i++) {
    if (!Q_stricmp(modelCache[i].name, modelName)) {
      return modelCache[i].scene;
    }
  }

  if (numModelCache == MAX_MODEL_CACHE) {
    Error("MAX_MODEL_CACHE reached");
  }

  sprintf(filename, "%s%s", gamedir, modelName);
  
  const struct aiScene *scene = aiImportFile(
      filename, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                    aiProcess_SortByPType | aiProcess_FlipUVs |
                    aiProcess_FlipWindingOrder |
                    aiProcess_PreTransformVertices);

  if (!scene) {
    return NULL;
  }

  if (strlen(modelName) >= MAX_QPATH) {
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
static void AnglesToMatrix(vec3_t angles, float matrix[3][3]) {
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

typedef struct {
  int originalIdx;
  float x, y, z;
} sortVert_t;

static int CompareVerts(const void *a, const void *b) {
  sortVert_t *v1 = (sortVert_t *)a;
  sortVert_t *v2 = (sortVert_t *)b;
  if (v1->x != v2->x) return (v1->x < v2->x) ? -1 : 1;
  if (v1->y != v2->y) return (v1->y < v2->y) ? -1 : 1;
  if (v1->z != v2->z) return (v1->z < v2->z) ? -1 : 1;
  return 0;
}


/*
====================
TrySpreadUVs

Advanced UV overlap detection and resolving.
Resolves mirrored/stacked UVs by spreading them into a 1D or 2D slot array.
Safeguarded to only touch simple mirrored props.
====================
*/
static void TrySpreadUVs(const struct aiMesh *mesh, int uvChannel, const char *modelName, int meshIdx, float *outU, float *outV) {
  // 1. Identify Geometric Islands (using XYZ connectivity)
  int *vPosId = malloc(sizeof(int) * mesh->mNumVertices);
  for (int j = 0; j < (int)mesh->mNumVertices; j++) vPosId[j] = -1;
  
  int numUniquePositions = 0;
  sortVert_t *sVerts = malloc(sizeof(sortVert_t) * mesh->mNumVertices);
  for (int j = 0; j < (int)mesh->mNumVertices; j++) {
    sVerts[j].originalIdx = j;
    sVerts[j].x = mesh->mVertices[j].x;
    sVerts[j].y = mesh->mVertices[j].y;
    sVerts[j].z = mesh->mVertices[j].z;
  }
  
  qsort(sVerts, mesh->mNumVertices, sizeof(sortVert_t), CompareVerts);
  
  for (int j = 0; j < (int)mesh->mNumVertices; j++) {
    if (j > 0 && sVerts[j].x == sVerts[j-1].x && sVerts[j].y == sVerts[j-1].y && sVerts[j].z == sVerts[j-1].z) {
      vPosId[sVerts[j].originalIdx] = vPosId[sVerts[j-1].originalIdx];
    } else {
      vPosId[sVerts[j].originalIdx] = numUniquePositions++;
    }
  }
  free(sVerts);

  int *triIsland = malloc(sizeof(int) * mesh->mNumFaces);
  for (int j = 0; j < (int)mesh->mNumFaces; j++) triIsland[j] = -1;

  int *pTriCount = calloc(numUniquePositions, sizeof(int));
  for (int j = 0; j < (int)mesh->mNumFaces; j++) {
    if (mesh->mFaces[j].mNumIndices != 3) continue;
    int p0 = vPosId[mesh->mFaces[j].mIndices[0]];
    int p1 = vPosId[mesh->mFaces[j].mIndices[1]];
    int p2 = vPosId[mesh->mFaces[j].mIndices[2]];
    pTriCount[p0]++; pTriCount[p1]++; pTriCount[p2]++;
  }
  int **pTris = malloc(sizeof(int *) * numUniquePositions);
  int *pTriOffset = calloc(numUniquePositions, sizeof(int));
  for (int j = 0; j < numUniquePositions; j++) {
    pTris[j] = malloc(sizeof(int) * pTriCount[j]);
  }
  for (int j = 0; j < (int)mesh->mNumFaces; j++) {
    if (mesh->mFaces[j].mNumIndices != 3) continue;
    for (int k = 0; k < 3; k++) {
      int pIdx = vPosId[mesh->mFaces[j].mIndices[k]];
      pTris[pIdx][pTriOffset[pIdx]++] = j;
    }
  }

  int numIslands = 0;
  int *stack = malloc(sizeof(int) * mesh->mNumFaces);
  for (int j = 0; j < (int)mesh->mNumFaces; j++) {
    if (triIsland[j] != -1 || mesh->mFaces[j].mNumIndices != 3) continue;
    int stackPtr = 0;
    stack[stackPtr++] = j;
    triIsland[j] = numIslands;
    while (stackPtr > 0) {
      int currTri = stack[--stackPtr];
      for (int k = 0; k < 3; k++) {
        int pIdx = vPosId[mesh->mFaces[currTri].mIndices[k]];
        for (int m = 0; m < pTriCount[pIdx]; m++) {
          int nextTri = pTris[pIdx][m];
          if (triIsland[nextTri] == -1) {
            triIsland[nextTri] = numIslands;
            stack[stackPtr++] = nextTri;
          }
        }
      }
    }
    numIslands++;
  }
  for (int j = 0; j < numUniquePositions; j++) free(pTris[j]);
  free(pTris); free(pTriCount); free(pTriOffset); free(vPosId);

  float initialMaxU = -999999, initialMaxV = -999999;
  for (int j = 0; j < (int)mesh->mNumFaces; j++) {
    if (triIsland[j] == -1) continue;
    for (int k = 0; k < 3; k++) {
      int vIdx = mesh->mFaces[j].mIndices[k];
      struct aiVector3D *u = &mesh->mTextureCoords[uvChannel][vIdx];
      if (u->x > initialMaxU) initialMaxU = u->x;
      if (u->y > initialMaxV) initialMaxV = u->y;
    }
  }

  qboolean abortSpreading = (numIslands > 500 || initialMaxU > 5.0f || initialMaxV > 5.0f) ? qtrue : qfalse;
  int numShifted = 0;
  typedef struct { float mins[2], maxs[2], offset[2]; } uvIsland_t;
  uvIsland_t *islands = NULL;

  if (!abortSpreading) {
    islands = malloc(sizeof(uvIsland_t) * numIslands);
    for (int j = 0; j < numIslands; j++) {
      islands[j].mins[0] = islands[j].mins[1] = 999999;
      islands[j].maxs[0] = islands[j].maxs[1] = -999999;
      islands[j].offset[0] = islands[j].offset[1] = 0;
    }
    for (int j = 0; j < (int)mesh->mNumFaces; j++) {
      if (triIsland[j] == -1) continue;
      int islId = triIsland[j];
      for (int k = 0; k < 3; k++) {
        int vIdx = mesh->mFaces[j].mIndices[k];
        struct aiVector3D *u = &mesh->mTextureCoords[uvChannel][vIdx];
        if (u->x < islands[islId].mins[0]) islands[islId].mins[0] = u->x;
        if (u->y < islands[islId].mins[1]) islands[islId].mins[1] = u->y;
        if (u->x > islands[islId].maxs[0]) islands[islId].maxs[0] = u->x;
        if (u->y > islands[islId].maxs[1]) islands[islId].maxs[1] = u->y;
      }
    }

    // ------------------------------------------
    // TIGHT SHELF PACKER
    // ------------------------------------------
    // 1. Calculate Total Area and Sort by Height
    float totalArea = 0;
    int sortIds[500];
    for (int j = 0; j < numIslands; j++) {
      sortIds[j] = j;
      float w = islands[j].maxs[0] - islands[j].mins[0];
      float h = islands[j].maxs[1] - islands[j].mins[1];
      totalArea += w * h;
    }

    // Simple Bubble Sort (numIslands is small)
    for (int a = 0; a < numIslands - 1; a++) {
      for (int b = a + 1; b < numIslands; b++) {
        float hA = islands[sortIds[a]].maxs[1] - islands[sortIds[a]].mins[1];
        float hB = islands[sortIds[b]].maxs[1] - islands[sortIds[b]].mins[1];
        if (hB > hA) { int t = sortIds[a]; sortIds[a] = sortIds[b]; sortIds[b] = t; }
      }
    }

    // 2. Greedy Shelf Packing
    float atlasWidth = sqrtf(totalArea) * 1.5f; // Target square-ish area with breathing room
    if (atlasWidth < 5.0f) atlasWidth = 5.0f;
    
    float shelfX = 0, shelfY = 0, currentShelfHeight = 0, gutter = 0.1f;
    for (int i = 0; i < numIslands; i++) {
      int id = sortIds[i];
      float w = islands[id].maxs[0] - islands[id].mins[0];
      float h = islands[id].maxs[1] - islands[id].mins[1];

      if (shelfX + w + gutter > atlasWidth && shelfX > 0) {
        shelfX = 0;
        shelfY += currentShelfHeight + gutter;
        currentShelfHeight = 0;
      }

      islands[id].offset[0] = shelfX;
      islands[id].offset[1] = shelfY;
      
      if (shelfX != 0 || shelfY != 0) numShifted++;
      shelfX += w + gutter;
      if (h > currentShelfHeight) currentShelfHeight = h;
      
      if (shelfY + h > 20.0f) { abortSpreading = qtrue; break; } // Safeguard
    }
  }

  if (abortSpreading && islands) {
    for (int j = 0; j < numIslands; j++) islands[j].offset[0] = islands[j].offset[1] = 0;
  }

  float currentMaxU = -999999;
  if (islands) {
    for (int j = 0; j < numIslands; j++) {
      float m = islands[j].maxs[0] + islands[j].offset[0];
      if (m > currentMaxU) currentMaxU = m;
    }
  }

  for (int j = 0; j < (int)mesh->mNumFaces; j++) {
    if (triIsland[j] != -1 && islands) {
      int islId = triIsland[j];
      outU[j] = islands[islId].offset[0];
      outV[j] = islands[islId].offset[1];
    } else {
      outU[j] = outV[j] = 0;
    }
  }

  free(triIsland); free(stack); if (islands) free(islands);
  if (!abortSpreading && currentMaxU > 1.05f) {
    _printf("  NOTICE: model %s (mesh %d) UVs spread and TIGHT-SHELF-PACKED (Max U: %.2f, Islands: %d, Shifted: %d)\n", modelName, meshIdx, currentMaxU, numIslands, numShifted);
  }
}

/*
====================
LoadTriangleModels

Initial pass to load and transform all misc_model entities.
====================
*/
void LoadTriangleModels(void) {
  int entity_num;
  entity_t *entity;
  const char *model;
  const struct aiScene *scene;
  vec3_t origin, angles;
  float scale;
  vec3_t scale_vec;
  float rotationMatrix[3][3];

  for (entity_num = 1; entity_num < num_entities; entity_num++) {
    entity = &entities[entity_num];
    const char *classname = ValueForKey(entity, "classname");

    if (!Q_stricmp("misc_model", classname)) {
      model = ValueForKey(entity, "model");
      if (!model[0])
        continue;

      GetVectorForKey(entity, "origin", origin);
      GetVectorForKey(entity, "angles", angles);
      if (angles[0] == 0 && angles[1] == 0 && angles[2] == 0) {
        angles[1] = FloatForKey(entity, "angle");
      }

      scale = FloatForKey(entity, "modelscale");
      if (scale == 0)
        scale = 1.0f;

      GetVectorForKey(entity, "modelscale_vec", scale_vec);
      if (scale_vec[0] == 0 && scale_vec[1] == 0 && scale_vec[2] == 0) {
        scale_vec[0] = scale_vec[1] = scale_vec[2] = scale;
      }

      scene = GetCachedModel(model);
      if (!scene) {
        continue;
      }

      if (numModelInstances == MAX_MODEL_INSTANCES) {
        Error("MAX_MODEL_INSTANCES reached");
      }

      modelInstance_t *inst = &modelInstances[numModelInstances++];
      strncpy(inst->modelName, model, MAX_QPATH - 1);
      inst->modelName[MAX_QPATH - 1] = '\0';
      inst->creator = entity;

      inst->numDrawSurfs = 0;
      inst->drawSurfs = malloc(sizeof(mapDrawSurface_t *) * 1024); // Allocate space for many potential chunks
      if (!inst->drawSurfs) {
        Error("Failed to allocate drawSurfs array");
      }
      inst->num_collision_meshes = 0;

      AnglesToMatrix(angles, rotationMatrix);

      for (int i = 0; i < scene->mNumMeshes; i++) {
        struct aiMesh *mesh = scene->mMeshes[i];
        char shaderName[MAX_QPATH];
        ShaderForMesh(model, mesh, scene, shaderName);
        shaderInfo_t *si = ShaderInfoForShader(shaderName);

        // ==========================================
        // UV Overlap Detection & Automatic Spreading
        // ==========================================
        float *uOffsets = calloc(mesh->mNumFaces, sizeof(float));
        float *vOffsets = calloc(mesh->mNumFaces, sizeof(float));
        int uvChannel = (mesh->mTextureCoords[1]) ? 1 : 0;

        if (mesh->mTextureCoords[uvChannel]) {
          // Most Basic Check: UV Area Ratio Detection (Sampled).
          // Ratio > 1.1 means significant overlaps/mirroring.
          qboolean potentialOverlap = qfalse;
          float areaSum = 0;
          float uvMins[2] = {999999, 999999}, uvMaxs[2] = {-999999, -999999};
          int sampleStep = (mesh->mNumFaces > 200) ? 10 : 1;
          for (int k = 0; k < (int)mesh->mNumFaces; k += sampleStep) {
            if (mesh->mFaces[k].mNumIndices != 3) continue;
            struct aiVector3D *v0 = &mesh->mTextureCoords[uvChannel][mesh->mFaces[k].mIndices[0]];
            struct aiVector3D *v1 = &mesh->mTextureCoords[uvChannel][mesh->mFaces[k].mIndices[1]];
            struct aiVector3D *v2 = &mesh->mTextureCoords[uvChannel][mesh->mFaces[k].mIndices[2]];
            
            // Triangle Area (Shoelace formula)
            float a = 0.5f * fabsf((v0->x - v2->x) * (v1->y - v0->y) - (v0->x - v1->x) * (v2->y - v0->y));
            areaSum += a;

            // UV Bounds
            for (int m = 0; m < 3; m++) {
              struct aiVector3D *v = &mesh->mTextureCoords[uvChannel][mesh->mFaces[k].mIndices[m]];
              if (v->x < uvMins[0]) uvMins[0] = v->x;
              if (v->x > uvMaxs[0]) uvMaxs[0] = v->x;
              if (v->y < uvMins[1]) uvMins[1] = v->y;
              if (v->y > uvMaxs[1]) uvMaxs[1] = v->y;
            }
          }
          float bboxArea = (uvMaxs[0] - uvMins[0]) * (uvMaxs[1] - uvMins[1]);
          float areaRatio = (bboxArea > 0.001f) ? (areaSum * sampleStep / bboxArea) : 0;
          
          if (areaRatio > 1.05f) {
            potentialOverlap = qtrue;
          }

          if (potentialOverlap) {
            TrySpreadUVs(mesh, uvChannel, model, i, uOffsets, vOffsets);
          }
        }

        // ==========================================
        // STEP 1: Extract Raw Collision Topology
        // ==========================================
        if (si && (si->contents & CONTENTS_SOLID) && inst->num_collision_meshes < MAX_MODEL_COLLISION_MESHES) {
          colMesh_t *cm = malloc(sizeof(colMesh_t));
          memset(cm, 0, sizeof(colMesh_t));
          cm->shaderInfo = si;

          cm->numVerts = mesh->mNumVertices;
          cm->verts = malloc(sizeof(vec3_t) * cm->numVerts);
          for (int j = 0; j < mesh->mNumVertices; j++) {
            float mx = mesh->mVertices[j].x * scale_vec[0];
            float my = mesh->mVertices[j].y * scale_vec[1];
            float mz = mesh->mVertices[j].z * scale_vec[2];

            // Axis Swap (Assimp Y-Up -> Quake Z-Up)
            vec3_t tx;
            tx[0] = mx; tx[1] = -mz; tx[2] = my;

            // Rotation
            cm->verts[j][0] = origin[0] + (tx[0] * rotationMatrix[0][0] + tx[1] * rotationMatrix[1][0] + tx[2] * rotationMatrix[2][0]);
            cm->verts[j][1] = origin[1] + (tx[0] * rotationMatrix[0][1] + tx[1] * rotationMatrix[1][1] + tx[2] * rotationMatrix[2][1]);
            cm->verts[j][2] = origin[2] + (tx[0] * rotationMatrix[0][2] + tx[1] * rotationMatrix[1][2] + tx[2] * rotationMatrix[2][2]);
          }

          // Count valid triangles
          int validTris = 0;
          for (int j = 0; j < (int)mesh->mNumFaces; j++) {
            if (mesh->mFaces[j].mNumIndices == 3) {
              validTris++;
            }
          }

          cm->numTris = validTris;
          if (cm->numTris > 0) {
            cm->tris = malloc(sizeof(colTri_t) * cm->numTris);
            int triIdx = 0;
            for (int j = 0; j < (int)mesh->mNumFaces; j++) {
              if (mesh->mFaces[j].mNumIndices == 3) {
                cm->tris[triIdx][0] = mesh->mFaces[j].mIndices[0];
                cm->tris[triIdx][1] = mesh->mFaces[j].mIndices[1];
                cm->tris[triIdx][2] = mesh->mFaces[j].mIndices[2];
                triIdx++;
              }
            }
            inst->collision_meshes[inst->num_collision_meshes++] = cm;
          } else {
            free(cm->verts);
            free(cm);
          }
        }

        // ==========================================
        // STEP 2: Chunk Visual Geometry
        // ==========================================
        if (si && (si->surfaceFlags & SURF_SKIP)) {
          continue;
        }

        int currentFace = 0;
        
        // Vertex mapping array (old index -> new index inside this chunk)
        int *vMap = malloc(sizeof(int) * mesh->mNumVertices);
        
        while (currentFace < (int)mesh->mNumFaces) {
          if (inst->numDrawSurfs >= 1024) {
            Error("Too many draw surfaces generated for model %s! Increase array size.", model);
          }

          mapDrawSurface_t *ds = AllocDrawSurf();
          inst->drawSurfs[inst->numDrawSurfs++] = ds;
          memset(ds, 0, sizeof(*ds));
          ds->miscModel = qtrue;
          ds->planeNum = -1;
          ds->shaderInfo = si;
          ds->lightmapNum = -1;
          ds->fogNum = -1;

          // Reset vMap for this new chunk
          for (int v = 0; v < mesh->mNumVertices; v++) vMap[v] = -1;

          // Max sizes
          ds->verts = malloc(sizeof(drawVert_t) * MAX_SURFACE_VERTS);
          ds->indexes = malloc(sizeof(int) * MAX_SURFACE_INDEXES);
          ds->numVerts = 0;
          ds->numIndexes = 0;

          if (!ds->verts || !ds->indexes) Error("Failed to allocate chunk arrays");

          while (currentFace < (int)mesh->mNumFaces) {
            struct aiFace *face = &mesh->mFaces[currentFace];
            if (face->mNumIndices != 3) {
              currentFace++;
              continue;
            }

            // How many NEW vertices would this face add?
            int newVerts = 0;
            for (int k = 0; k < 3; k++) {
              if (vMap[face->mIndices[k]] == -1) newVerts++;
            }

            // Check limits!
            if (ds->numVerts + newVerts > MAX_SURFACE_VERTS || ds->numIndexes + 3 > MAX_SURFACE_INDEXES) {
              // Time for a new chunk!
              if (ds->numIndexes == 0) {
                 // A single triangle is bigger than the limit? Impossible, but safeguard
                 Error("Single triangle exceeds limits?");
              }
              break; // Break the inner while, start a new drawSurf
            }

            // Add the face to this chunk
            for (int k = 0; k < 3; k++) {
              unsigned int oldIdx = face->mIndices[k];
              if (vMap[oldIdx] == -1) {
                // Add the vertex!
                vMap[oldIdx] = ds->numVerts;
                drawVert_t *dv = &ds->verts[ds->numVerts];
                
                float mx = mesh->mVertices[oldIdx].x * scale_vec[0];
                float my = mesh->mVertices[oldIdx].y * scale_vec[1];
                float mz = mesh->mVertices[oldIdx].z * scale_vec[2];

                vec3_t tx;
                tx[0] = mx; tx[1] = -mz; tx[2] = my;

                dv->xyz[0] = origin[0] + (tx[0] * rotationMatrix[0][0] + tx[1] * rotationMatrix[1][0] + tx[2] * rotationMatrix[2][0]);
                dv->xyz[1] = origin[1] + (tx[0] * rotationMatrix[0][1] + tx[1] * rotationMatrix[1][1] + tx[2] * rotationMatrix[2][1]);
                dv->xyz[2] = origin[2] + (tx[0] * rotationMatrix[0][2] + tx[1] * rotationMatrix[1][2] + tx[2] * rotationMatrix[2][2]);

                if (mesh->mNormals) {
                  float nx = mesh->mNormals[oldIdx].x;
                  float ny = mesh->mNormals[oldIdx].y;
                  float nz = mesh->mNormals[oldIdx].z;

                  tx[0] = nx; tx[1] = -nz; tx[2] = ny;

                  dv->normal[0] = (tx[0] * rotationMatrix[0][0] + tx[1] * rotationMatrix[1][0] + tx[2] * rotationMatrix[2][0]);
                  dv->normal[1] = (tx[0] * rotationMatrix[0][1] + tx[1] * rotationMatrix[1][1] + tx[2] * rotationMatrix[2][1]);
                  dv->normal[2] = (tx[0] * rotationMatrix[0][2] + tx[1] * rotationMatrix[1][2] + tx[2] * rotationMatrix[2][2]);
                }

                if (mesh->mTextureCoords[0]) {
                  dv->st[0] = mesh->mTextureCoords[0][oldIdx].x;
                  dv->st[1] = mesh->mTextureCoords[0][oldIdx].y;
                }

                // Prefer channel 1 for lightmap UVs, fallback to channel 0
                if (mesh->mTextureCoords[1]) {
                  dv->lightmap[0][0] = mesh->mTextureCoords[1][oldIdx].x + uOffsets[currentFace];
                  dv->lightmap[0][1] = mesh->mTextureCoords[1][oldIdx].y + vOffsets[currentFace];
                } else if (mesh->mTextureCoords[0]) {
                  dv->lightmap[0][0] = dv->st[0] + uOffsets[currentFace];
                  dv->lightmap[0][1] = dv->st[1] + vOffsets[currentFace];
                }

                dv->color[0][0] = dv->color[0][1] = dv->color[0][2] = dv->color[0][3] = 255;
                ds->numVerts++;
              }
              // Add the index
              ds->indexes[ds->numIndexes++] = vMap[oldIdx];
            }
            currentFace++;
          }

          // Realloc to save memory
          if (ds->numVerts > 0) {
            ds->verts = realloc(ds->verts, sizeof(drawVert_t) * ds->numVerts);
            ds->indexes = realloc(ds->indexes, sizeof(int) * ds->numIndexes);
          } else {
            // Empty chunk
            inst->numDrawSurfs--;
          }
        }
        
        free(vMap);
        if (uOffsets) free(uOffsets);
        if (vOffsets) free(vOffsets);
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
void AddTriangleModels(tree_t *tree) {
  int i;

  for (i = 0; i < numModelInstances; i++) {
    modelInstance_t *inst = &modelInstances[i];
    c_triangleModels++;
    for (int j = 0; j < inst->numDrawSurfs; j++) {
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
