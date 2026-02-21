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

static FILE *modelsInfoFile;

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
  char infoName[1024];
  char base[1024];
  float rotationMatrix[3][3];

  _printf("----- LoadTriangleModels (Assimp) -----\n");

  numModelInstances = 0; // Reset instances

  ExtractFileBase(source, base);
  sprintf(infoName, "%s_modelsinfo.txt", base);
  modelsInfoFile = fopen(infoName, "w");
  if (modelsInfoFile) {
    fprintf(modelsInfoFile, "Output for map: %s\n\n", source);
  }

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

      if (modelsInfoFile) {
        fprintf(modelsInfoFile, "Entity %i (Model: %s) {\n", entity_num, model);
        fprintf(modelsInfoFile, "  Origin: %.2f %.2f %.2f\n", origin[0],
                origin[1], origin[2]);
        fprintf(modelsInfoFile, "  Angles: %.2f %.2f %.2f\n", angles[0],
                angles[1], angles[2]);
        fprintf(modelsInfoFile, "  Scale: %.2f %.2f %.2f\n", scale_vec[0],
                scale_vec[1], scale_vec[2]);
      }

      inst->numDrawSurfs = scene->mNumMeshes;
      inst->drawSurfs = malloc(sizeof(mapDrawSurface_t *) * inst->numDrawSurfs);
      if (!inst->drawSurfs) {
        Error("Failed to allocate drawSurfs array");
      }

      AnglesToMatrix(angles, rotationMatrix);

      for (int i = 0; i < scene->mNumMeshes; i++) {
        struct aiMesh *mesh = scene->mMeshes[i];
        char shaderName[MAX_QPATH];

        ShaderForMesh(model, mesh, scene, shaderName);

        mapDrawSurface_t *ds = AllocDrawSurf();
        inst->drawSurfs[i] = ds;
        memset(ds, 0, sizeof(*ds));
        ds->miscModel = qtrue;
        ds->planeNum = -1;
        ds->shaderInfo = ShaderInfoForShader(shaderName);

        ds->numVerts = mesh->mNumVertices;
        ds->verts = malloc(sizeof(drawVert_t) * ds->numVerts);
        if (!ds->verts)
          Error("Failed to allocate vertices");

        ds->numIndexes = mesh->mNumFaces * 3;
        ds->indexes = malloc(sizeof(int) * ds->numIndexes);
        if (!ds->indexes)
          Error("Failed to allocate indices");
        memset(ds->indexes, 0, sizeof(int) * ds->numIndexes);

        ds->lightmapNum = -1;
        ds->fogNum = -1;

        for (int j = 0; j < mesh->mNumVertices; j++) {
          drawVert_t *dv = &ds->verts[j];
          float mx = mesh->mVertices[j].x * scale_vec[0];
          float my = mesh->mVertices[j].y * scale_vec[1];
          float mz = mesh->mVertices[j].z * scale_vec[2];

          // Axis Swap (Assimp Y-Up -> Quake Z-Up)
          vec3_t tx;
          tx[0] = mx;
          tx[1] = -mz;
          tx[2] = my;

          // Rotation
          dv->xyz[0] = origin[0] + (tx[0] * rotationMatrix[0][0] +
                                    tx[1] * rotationMatrix[1][0] +
                                    tx[2] * rotationMatrix[2][0]);
          dv->xyz[1] = origin[1] + (tx[0] * rotationMatrix[0][1] +
                                    tx[1] * rotationMatrix[1][1] +
                                    tx[2] * rotationMatrix[2][1]);
          dv->xyz[2] = origin[2] + (tx[0] * rotationMatrix[0][2] +
                                    tx[1] * rotationMatrix[1][2] +
                                    tx[2] * rotationMatrix[2][2]);

          if (mesh->mNormals) {
            float nx = mesh->mNormals[j].x;
            float ny = mesh->mNormals[j].y;
            float nz = mesh->mNormals[j].z;

            // Axis Swap for Normals (No Scale!)
            tx[0] = nx;
            tx[1] = -nz;
            tx[2] = ny;

            dv->normal[0] =
                (tx[0] * rotationMatrix[0][0] + tx[1] * rotationMatrix[1][0] +
                 tx[2] * rotationMatrix[2][0]);
            dv->normal[1] =
                (tx[0] * rotationMatrix[0][1] + tx[1] * rotationMatrix[1][1] +
                 tx[2] * rotationMatrix[2][1]);
            dv->normal[2] =
                (tx[0] * rotationMatrix[0][2] + tx[1] * rotationMatrix[1][2] +
                 tx[2] * rotationMatrix[2][2]);
          }

          if (mesh->mTextureCoords[0]) {
            dv->st[0] = mesh->mTextureCoords[0][j].x;
            dv->st[1] = mesh->mTextureCoords[0][j].y;
          }

          dv->color[0] = dv->color[1] = dv->color[2] = dv->color[3] = 255;
        }

        for (int j = 0; j < (int)mesh->mNumFaces; j++) {
          struct aiFace *face = &mesh->mFaces[j];
          if (face->mNumIndices != 3)
            continue;

          // Robustness: Validation of indices against vertex count
          if (face->mIndices[0] >= (unsigned int)ds->numVerts ||
              face->mIndices[1] >= (unsigned int)ds->numVerts ||
              face->mIndices[2] >= (unsigned int)ds->numVerts) {
            _printf("WARNING: Mesh %i face %i has out-of-bounds indices "
                    "(%i,%i,%i) for %i verts. Skipping.\n",
                    i, j, face->mIndices[0], face->mIndices[1],
                    face->mIndices[2], ds->numVerts);
            continue;
          }

          ds->indexes[j * 3 + 0] = face->mIndices[0];
          ds->indexes[j * 3 + 1] = face->mIndices[1];
          ds->indexes[j * 3 + 2] = face->mIndices[2];
        }
      }

      if (modelsInfoFile) {
        fprintf(modelsInfoFile, "}\n");
      }
    }
  }

  if (modelsInfoFile) {
    fclose(modelsInfoFile);
    modelsInfoFile = NULL;
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
