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

/*
============
ShaderForMesh

Determine the shader name for a given mesh, applying format-specific rules.
============
*/
static void ShaderForMesh(const char *modelPath, const struct aiMesh *mesh,
                          const struct aiScene *scene, char *shaderName) {
  char ext[16];
  struct aiMaterial *mat = scene->mMaterials[mesh->mMaterialIndex];
  struct aiString path;
  struct aiString matName;

  // Step 1: Prioritize high-level texture API (Diffuse) for all formats
  if (aiGetMaterialTexture(mat, aiTextureType_DIFFUSE, 0, &path, NULL, NULL,
                           NULL, NULL, NULL, NULL) == aiReturn_SUCCESS) {
    strncpy(shaderName, path.data, MAX_QPATH - 1);
    shaderName[MAX_QPATH - 1] = '\0';
    StripExtension(shaderName);
    return;
  }

  ExtractFileExtension(modelPath, ext);

  // Step 2: Fallback to raw '$tex.file' key for .obj models
  if (!Q_stricmp(ext, "obj")) {
    if (aiGetMaterialString(mat, "$tex.file", 0, 0, &path) ==
        aiReturn_SUCCESS) {
      strncpy(shaderName, path.data, MAX_QPATH - 1);
      shaderName[MAX_QPATH - 1] = '\0';
      StripExtension(shaderName);
      return;
    }
  }

  // Step 3: Fallback to the unified material name
  if (aiGetMaterialString(mat, AI_MATKEY_NAME, &matName) == aiReturn_SUCCESS) {
    strncpy(shaderName, matName.data, MAX_QPATH - 1);
    shaderName[MAX_QPATH - 1] = '\0';
  } else {
    // Step 4: Absolute final fallback
    strcpy(shaderName, "default");
  }
}

/*
============
InsertAssimpModel

Convert a model entity to raw geometry surfaces using Assimp
============
*/
void InsertAssimpModel(const char *modelName, vec3_t origin, float angle,
                       tree_t *tree) {
  char filename[1024];
  const struct aiScene *scene;
  int i, j;
  float angleRad;
  float angleCos, angleSin;
  mapDrawSurface_t *ds;
  drawVert_t *dv;
  struct aiMesh *mesh;
  char shaderName[MAX_QPATH];
  vec3_t temp;

  sprintf(filename, "%s%s", gamedir, modelName);
  _printf("--- InsertAssimpModel: %s ---\n", filename);

  if (modelsInfoFile) {
    fprintf(modelsInfoFile, "Model: \"%s\" {\n", modelName);
  }

  // Import the file with triangulation and standard Q3-friendly flags
  scene = aiImportFile(filename, aiProcess_Triangulate |
                                     aiProcess_JoinIdenticalVertices |
                                     aiProcess_SortByPType | aiProcess_FlipUVs |
                                     aiProcess_FlipWindingOrder |
                                     aiProcess_PreTransformVertices);

  if (!scene) {
    _printf("WARNING: Assimp failed to load model %s: %s\n", filename,
            aiGetErrorString());
    if (modelsInfoFile) {
      fprintf(modelsInfoFile, "  ERROR: %s\n}\n", aiGetErrorString());
    }
    return;
  }

  angleRad = angle / 180.0f * Q_PI;
  angleCos = cos(angleRad);
  angleSin = sin(angleRad);

  for (i = 0; i < scene->mNumMeshes; i++) {
    mesh = scene->mMeshes[i];

    c_triangleModels++;
    c_triangleSurfaces++;

    if (mesh->mNumVertices <= 0 || mesh->mNumFaces <= 0) {
      if (modelsInfoFile) {
        fprintf(modelsInfoFile, "  Mesh %i: (Empty)\n", i);
      }
      continue;
    }

    // Allocate a new draw surface
    ds = AllocDrawSurf();
    ds->miscModel = qtrue;

    // Determine shader name
    ShaderForMesh(modelName, mesh, scene, shaderName);
    _printf("  Mesh %i: Name: '%s', Verts: %i, Faces: %i, Shader: '%s'\n", i,
            mesh->mName.data, mesh->mNumVertices, mesh->mNumFaces, shaderName);

    if (modelsInfoFile) {
      struct aiMaterial *mat = scene->mMaterials[mesh->mMaterialIndex];
      fprintf(modelsInfoFile, "  Mesh %i: \"%s\" {\n", i, mesh->mName.data);
      fprintf(modelsInfoFile, "    Vertices: %i\n", mesh->mNumVertices);
      fprintf(modelsInfoFile, "    Faces: %i\n", mesh->mNumFaces);
      fprintf(modelsInfoFile, "    Shader Name: \"%s\"\n", shaderName);

      fprintf(modelsInfoFile, "    Material Properties (%i) {\n",
              mat->mNumProperties);
      for (int p = 0; p < (int)mat->mNumProperties; p++) {
        struct aiMaterialProperty *prop = mat->mProperties[p];
        fprintf(modelsInfoFile,
                "      [%2i] Key: '%-20s' Sem: %2i Idx: %i Type: %i Len: %i", p,
                prop->mKey.data, prop->mSemantic, prop->mIndex, prop->mType,
                prop->mDataLength);

        if (prop->mType == aiPTI_String && prop->mDataLength > 4) {
          struct aiString *str = (struct aiString *)prop->mData;
          fprintf(modelsInfoFile, " String: '%s'", str->data);
        }
        fprintf(modelsInfoFile, "\n");
      }
      fprintf(modelsInfoFile, "    }\n"); // End Properties

      // Textures
      fprintf(modelsInfoFile, "    Textures {\n");
      for (int t = 1; t <= 18; t++) {
        unsigned int count =
            aiGetMaterialTextureCount(mat, (enum aiTextureType)t);
        for (unsigned int texIdx = 0; texIdx < count; texIdx++) {
          struct aiString path;
          if (aiGetMaterialTexture(mat, (enum aiTextureType)t, texIdx, &path,
                                   NULL, NULL, NULL, NULL, NULL,
                                   NULL) == aiReturn_SUCCESS) {
            fprintf(modelsInfoFile, "      Type %i, Idx %i: \"%s\"\n", t,
                    texIdx, path.data);
          }
        }
      }
      fprintf(modelsInfoFile, "    }\n"); // End Textures
      fprintf(modelsInfoFile, "  }\n");   // End Mesh
    }

    ds->shaderInfo = ShaderInfoForShader(shaderName);

    ds->numVerts = mesh->mNumVertices;
    ds->verts = malloc(sizeof(drawVert_t) * ds->numVerts);

    ds->numIndexes = mesh->mNumFaces * 3;
    ds->indexes = malloc(sizeof(int) * ds->numIndexes);

    ds->lightmapNum = -1;
    ds->fogNum = -1;

    // Copy vertices and apply transformations
    c_triangleVertexes += ds->numVerts;
    for (j = 0; j < ds->numVerts; j++) {
      dv = &ds->verts[j];
      float mx, my, mz;

      // Position (Rotate and Translate)
      mx = mesh->mVertices[j].x;
      my = mesh->mVertices[j].y;
      mz = mesh->mVertices[j].z;

      // Axis Swap (Assimp Y-Up -> Quake Z-Up)
      temp[0] = mx;
      temp[1] = -mz;
      temp[2] = my;

      dv->xyz[0] = origin[0] + (temp[0] * angleCos - temp[1] * angleSin);
      dv->xyz[1] = origin[1] + (temp[0] * angleSin + temp[1] * angleCos);
      dv->xyz[2] = origin[2] + temp[2];

      // Normal (Rotate)
      if (mesh->mNormals) {
        mx = mesh->mNormals[j].x;
        my = mesh->mNormals[j].y;
        mz = mesh->mNormals[j].z;

        // Axis Swap (Assimp Y-Up -> Quake Z-Up)
        temp[0] = mx;
        temp[1] = -mz;
        temp[2] = my;

        dv->normal[0] = temp[0] * angleCos - temp[1] * angleSin;
        dv->normal[1] = temp[0] * angleSin + temp[1] * angleCos;
        dv->normal[2] = temp[2];
      }

      // Texture Coordinates
      if (mesh->mTextureCoords[0]) {
        dv->st[0] = mesh->mTextureCoords[0][j].x;
        dv->st[1] = mesh->mTextureCoords[0][j].y;
      }

      // Default values
      dv->color[0] = 255;
      dv->color[1] = 255;
      dv->color[2] = 255;
      dv->color[3] = 255;
    }

    // Copy indices
    c_triangleIndexes += ds->numIndexes;
    for (j = 0; j < mesh->mNumFaces; j++) {
      struct aiFace *face = &mesh->mFaces[j];
      if (face->mNumIndices != 3) {
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

  aiReleaseImport(scene);
}

/*
=====================
AddTriangleModels
=====================
*/
void AddTriangleModels(tree_t *tree) {
  int entity_num;
  entity_t *entity;
  char infoName[1024];
  char base[1024];

  qprintf("----- AddTriangleModels (Assimp) -----\n");

  ExtractFileBase(source, base);
  sprintf(infoName, "%s_modelsinfo.txt", base);
  modelsInfoFile = fopen(infoName, "w");
  if (modelsInfoFile) {
    fprintf(modelsInfoFile, "Output for map: %s\n\n", source);
  }

  for (entity_num = 1; entity_num < num_entities; entity_num++) {
    entity = &entities[entity_num];

    if (!Q_stricmp("misc_model", ValueForKey(entity, "classname"))) {
      const char *model;
      vec3_t origin;
      float angle;

      angle = FloatForKey(entity, "angle");
      GetVectorForKey(entity, "origin", origin);
      model = ValueForKey(entity, "model");

      if (!model[0]) {
        _printf("WARNING: misc_model at %i %i %i without a model key\n",
                (int)origin[0], (int)origin[1], (int)origin[2]);
        continue;
      }

      InsertAssimpModel(model, origin, angle, tree);
    }
  }

  if (modelsInfoFile) {
    fclose(modelsInfoFile);
    modelsInfoFile = NULL;
  }

  qprintf("%5i triangle models\n", c_triangleModels);
  qprintf("%5i triangle surfaces\n", c_triangleSurfaces);
  qprintf("%5i triangle vertexes\n", c_triangleVertexes);
  qprintf("%5i triangle indexes\n", c_triangleIndexes);
}
