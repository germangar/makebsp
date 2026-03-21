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
    _printf("WARNING: mesh->mMaterialIndex (%i) >= scene->mNumMaterials (%i)\n",
            mesh->mMaterialIndex, scene->mNumMaterials);
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
      _printf("      obj $tex.file: %s\n", path.data);
      fflush(stdout);
      strncpy(shaderName, path.data, MAX_QPATH - 1);
      shaderName[MAX_QPATH - 1] = '\0';
      StripExtension(shaderName);
      return;
    }
  }

  if (aiGetMaterialString(mat, AI_MATKEY_NAME, &matName) == aiReturn_SUCCESS) {
    _printf("      AI_MATKEY_NAME: %s\n", matName.data);
    fflush(stdout);
    strncpy(shaderName, matName.data, MAX_QPATH - 1);
    shaderName[MAX_QPATH - 1] = '\0';
  } else {
    _printf("      Using default shader\n");
    fflush(stdout);
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
  _printf("--- Loading Model: %s ---\n", filename);
  fflush(stdout);

  const struct aiScene *scene = aiImportFile(
      filename, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                    aiProcess_SortByPType | aiProcess_FlipUVs |
                    aiProcess_FlipWindingOrder |
                    aiProcess_PreTransformVertices);

  if (!scene) {
    _printf("WARNING: Assimp failed to load model %s: %s\n", filename,
            aiGetErrorString());
    fflush(stdout);
    return NULL;
  }

  _printf("    Assimp success: %i meshes, %i materials\n", scene->mNumMeshes,
          scene->mNumMaterials);

  if (strlen(modelName) >= MAX_QPATH) {
    _printf("WARNING: modelName too long: %s\n", modelName);
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

    if (!Q_stricmp("misc_model", ValueForKey(entity, "classname"))) {
      model = ValueForKey(entity, "model");
      if (!model[0])
        continue;

      _printf("  Processing entity %i: %s\n", entity_num, model);

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
                  dv->lightmap[0][0] = mesh->mTextureCoords[1][oldIdx].x;
                  dv->lightmap[0][1] = mesh->mTextureCoords[1][oldIdx].y;
                } else if (mesh->mTextureCoords[0]) {
                  dv->lightmap[0][0] = dv->st[0];
                  dv->lightmap[0][1] = dv->st[1];
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
      }
    }
  }

  _printf("----- LoadTriangleModels finished -----\n");
}

/*
=====================
AddTriangleModels

Second pass: Insert the pre-calculated surfaces into the BSP tree.
=====================
*/
void AddTriangleModels(tree_t *tree) {
  int i;
  _printf("----- AddTriangleModels (Insertion) -----\n");

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
