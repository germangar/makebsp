/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "qbsp.h"
#include <assimp/cimport.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>


int c_triangleModels;
int c_triangleSurfaces;
int c_triangleVertexes;
int c_triangleIndexes;

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
  int i, j, k;
  float angleRad;
  float angleCos, angleSin;
  mapDrawSurface_t *ds;
  drawVert_t *dv;
  struct aiMesh *mesh;
  struct aiMaterial *mat;
  struct aiString matName;
  vec3_t temp;

  sprintf(filename, "%s%s", gamedir, modelName);

  // Import the file with triangulation and standard Q3-friendly flags
  scene = aiImportFile(filename, aiProcess_Triangulate |
                                     aiProcess_JoinIdenticalVertices |
                                     aiProcess_SortByPType | aiProcess_FlipUVs);

  if (!scene) {
    _printf("WARNING: Assimp failed to load model %s: %s\n", filename,
            aiGetErrorString());
    return;
  }

  angleRad = angle / 180.0f * Q_PI;
  angleCos = cos(angleRad);
  angleSin = sin(angleRad);

  c_triangleModels++;

  for (i = 0; i < scene->mNumMeshes; i++) {
    mesh = scene->mMeshes[i];

    if (mesh->mNumVertices <= 0 || mesh->mNumFaces <= 0) {
      continue;
    }

    c_triangleSurfaces++;

    // Allocate a new draw surface
    ds = AllocDrawSurf();
    ds->miscModel = qtrue;

    // Get material info
    mat = scene->mMaterials[mesh->mMaterialIndex];
    if (aiGetMaterialString(mat, AI_MATKEY_NAME, &matName) ==
        aiReturn_SUCCESS) {
      ds->shaderInfo = ShaderInfoForShader(matName.data);
    } else {
      ds->shaderInfo = ShaderInfoForShader("default"); // Fallback
    }

    ds->numVerts = mesh->mNumVertices;
    ds->verts = malloc(ds->numVerts * sizeof(ds->verts[0]));
    memset(ds->verts, 0, ds->numVerts * sizeof(ds->verts[0]));

    ds->numIndexes = mesh->mNumFaces * 3;
    ds->indexes = malloc(ds->numIndexes * sizeof(ds->indexes[0]));

    ds->lightmapNum = -1;
    ds->fogNum = -1;

    // Copy vertices and apply transformations
    c_triangleVertexes += ds->numVerts;
    for (j = 0; j < ds->numVerts; j++) {
      dv = &ds->verts[j];

      // Position (Rotate and Translate)
      temp[0] = mesh->mVertices[j].x;
      temp[1] = mesh->mVertices[j].y;
      temp[2] = mesh->mVertices[j].z;

      dv->xyz[0] = origin[0] + (temp[0] * angleCos - temp[1] * angleSin);
      dv->xyz[1] = origin[1] + (temp[0] * angleSin + temp[1] * angleCos);
      dv->xyz[2] = origin[2] + temp[2];

      // Normal (Rotate)
      if (mesh->mNormals) {
        temp[0] = mesh->mNormals[j].x;
        temp[1] = mesh->mNormals[j].y;
        temp[2] = mesh->mNormals[j].z;

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
        // This shouldn't happen with aiProcess_Triangulate
        continue;
      }
      ds->indexes[j * 3 + 0] = face->mIndices[0];
      ds->indexes[j * 3 + 1] = face->mIndices[1];
      ds->indexes[j * 3 + 2] = face->mIndices[2];
    }
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

  qprintf("----- AddTriangleModels (Assimp) -----\n");

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

  qprintf("%5i triangle models\n", c_triangleModels);
  qprintf("%5i triangle surfaces\n", c_triangleSurfaces);
  qprintf("%5i triangle vertexes\n", c_triangleVertexes);
  qprintf("%5i triangle indexes\n", c_triangleIndexes);
}
