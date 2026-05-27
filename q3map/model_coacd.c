/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2024-2026 Antigravity Implementation

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "qbsp.h"
#include "model_collision.h"
#include "../libs/coacd_api.h"
#include <stdio.h>

/*
====================
ConvertCoACDToCollisionHull

Adapter: converts CoACD's double-precision mesh to our float colHull_t.
====================
*/
static colHull_t *ConvertCoACDToCollisionHull(CoACD_Mesh *mesh)
{
    int i;
    colHull_t *hull = malloc(sizeof(colHull_t));
    hull->numVerts = (int)mesh->vertices_count;
    hull->verts = malloc(hull->numVerts * sizeof(vec3_t));
    for (i = 0; i < hull->numVerts; i++)
    {
        hull->verts[i][0] = (float)mesh->vertices_ptr[i * 3 + 0];
        hull->verts[i][1] = (float)mesh->vertices_ptr[i * 3 + 1];
        hull->verts[i][2] = (float)mesh->vertices_ptr[i * 3 + 2];
    }

    hull->numTris = (int)mesh->triangles_count;
    hull->tris = malloc(hull->numTris * sizeof(colTri_t));
    for (i = 0; i < hull->numTris; i++)
    {
        hull->tris[i][0] = mesh->triangles_ptr[i * 3 + 0];
        hull->tris[i][1] = mesh->triangles_ptr[i * 3 + 1];
        hull->tris[i][2] = mesh->triangles_ptr[i * 3 + 2];
    }
    return hull;
}

/*
====================
BrushesFromHullsCoACD

Adapter: bridge for CoACD.
====================
*/
static bspbrush_t *BrushesFromHullsCoACD(CoACD_MeshArray hulls, shaderInfo_t *si)
{
    int i;
    bspbrush_t *brushes;
    colHull_t **tempHulls = malloc(hulls.meshes_count * sizeof(colHull_t *));

    for (i = 0; i < (int)hulls.meshes_count; i++)
    {
        tempHulls[i] = ConvertCoACDToCollisionHull(&hulls.meshes_ptr[i]);
    }

    brushes = BrushesFromHulls(tempHulls, (int)hulls.meshes_count, si);

    for (i = 0; i < (int)hulls.meshes_count; i++)
    {
        FreeCollisionHull(tempHulls[i]);
    }
    free(tempHulls);

    return brushes;
}

/*
====================
GenerateCoACDCollision

Generates collision brushes for a given model instance using CoACD.
====================
*/
bspbrush_t *GenerateCoACDCollision(modelInstance_t *inst, qboolean mergeMeshes, shaderInfo_t *shader)
{
    int j, k;
    mapDrawSurface_t *ds;
    int totalVerts = 0;
    int totalIndexes = 0;
    double *allVerts;
    int *allIndexes;
    int currentVert = 0;
    int currentIndex = 0;
    bspbrush_t *hulls_list = NULL;

    float threshold, resolution, prep_resolution;
    int mcts_max_depth, preprocess_mode = COACD_PREPROCESS_AUTO;
    qboolean decimate;
    modelCategory_t category = inst->category;
    switch (category)
    {
    case MC_WALKABLE:
        threshold = 0.05f;
        resolution = 500;
        prep_resolution = 50;
        mcts_max_depth = 12;
        decimate = qfalse;
        preprocess_mode = COACD_PREPROCESS_OFF;
        break;
    case MC_WRAP:
        threshold = 0.2f;
        resolution = 1000;
        prep_resolution = 30;
        mcts_max_depth = 3;
        decimate = qtrue;
        break;
    case MC_SHELL:
        threshold = 0.2f;
        resolution = 80;
        prep_resolution = 50;
        mcts_max_depth = 3;
        decimate = qtrue;
        break;
    case MC_OBJECT:
    default:
        threshold = 0.025f;
        resolution = 1000;
        prep_resolution = 100;
        mcts_max_depth = 10;
        decimate = qfalse;
        break;
    case MC_NONE:
        return NULL;
    }

    if (mergeMeshes)
    {
        // ALL model vertexes callculated at once (meshes merged)
        for (j = 0; j < inst->numDrawSurfs; j++)
        {
            ds = inst->drawSurfs[j];
            if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID))
            {
                continue;
            }
            totalVerts += ds->numVerts;
            totalIndexes += ds->numIndexes;
        }

        if (totalVerts == 0)
            return NULL;

        allVerts = malloc(totalVerts * 3 * sizeof(double));
        allIndexes = malloc(totalIndexes * sizeof(int));

        for (j = 0; j < inst->numDrawSurfs; j++)
        {
            ds = inst->drawSurfs[j];
            if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID))
            {
                continue;
            }

            int startVert = currentVert;
            #define ENABLE_SNAP_GRID 0
            #define SNAP_GRID 0.125f
            for (k = 0; k < ds->numVerts; k++)
            {
#if ENABLE_SNAP_GRID
                allVerts[currentVert * 3 + 0] = round(ds->verts[k].xyz[0] / SNAP_GRID) * SNAP_GRID;
                allVerts[currentVert * 3 + 1] = round(ds->verts[k].xyz[1] / SNAP_GRID) * SNAP_GRID;
                allVerts[currentVert * 3 + 2] = round(ds->verts[k].xyz[2] / SNAP_GRID) * SNAP_GRID;
#else
                allVerts[currentVert * 3 + 0] = ds->verts[k].xyz[0];
                allVerts[currentVert * 3 + 1] = ds->verts[k].xyz[1];
                allVerts[currentVert * 3 + 2] = ds->verts[k].xyz[2];
#endif
                currentVert++;
            }

            for (k = 0; k < ds->numIndexes; k++)
            {
                allIndexes[currentIndex++] = startVert + ds->indexes[k];
            }
        }

        CoACD_Mesh input;
        input.vertices_ptr = allVerts;
        input.vertices_count = totalVerts;
        input.triangles_ptr = allIndexes;
        input.triangles_count = totalIndexes / 3;

        CoACD_MeshArray hulls = CoACD_run(
            &input,
            threshold,             // threshold
            -1,                    // max_convex_hull (no limit)
            preprocess_mode,       // preprocess_mode
            prep_resolution,       // prep_resolution (voxel grid)
            resolution,            // sample_resolution (ACTUAL accuracy)
            20,                    // mcts_nodes
            100,                   // mcts_iterations
            mcts_max_depth,        // mcts_max_depth
            false,                 // pca
            false,                 // merge (DISABLED FOR FIDELITY)
            decimate,              // decimate
            MAX_POINTS_ON_WINDING, // max_ch_vertex
            false,                 // extrude
            0.01,                  // extrude_margin
            COACD_APX_CH,          // apx_mode
            1234                   // seed
        );

        shaderInfo_t *si = shader; // default to caulk
        for (int surfaceIdx = 0; surfaceIdx < inst->numDrawSurfs; surfaceIdx++)
        {
            if (inst->drawSurfs[surfaceIdx]->shaderInfo && (inst->drawSurfs[surfaceIdx]->shaderInfo->contents & CONTENTS_SOLID))
            {
                si = inst->drawSurfs[surfaceIdx]->shaderInfo;
                break;
            }
        }
        bspbrush_t *brushes = BrushesFromHullsCoACD(hulls, si);
        hulls_list = CombineBrushes(hulls_list, brushes);

        CoACD_freeMeshArray(hulls);
        free(allVerts);
        free(allIndexes);
    }
    else
    {
        // Calculate Collision Per Mesh
        for (j = 0; j < inst->numDrawSurfs; j++)
        {
            ds = inst->drawSurfs[j];
            if (!ds->shaderInfo || !(ds->shaderInfo->contents & CONTENTS_SOLID))
            {
                continue;
            }

            if (ds->numVerts == 0 || ds->numIndexes == 0)
            {
                continue;
            }

            double *meshVerts = malloc(ds->numVerts * 3 * sizeof(double));
            int *meshIndexes = malloc(ds->numIndexes * sizeof(int));

            for (k = 0; k < ds->numVerts; k++)
            {
                meshVerts[k * 3 + 0] = ds->verts[k].xyz[0];
                meshVerts[k * 3 + 1] = ds->verts[k].xyz[1];
                meshVerts[k * 3 + 2] = ds->verts[k].xyz[2];
            }
            for (k = 0; k < ds->numIndexes; k++)
            {
                meshIndexes[k] = ds->indexes[k];
            }

            CoACD_Mesh input;
            input.vertices_ptr = meshVerts;
            input.vertices_count = ds->numVerts;
            input.triangles_ptr = meshIndexes;
            input.triangles_count = ds->numIndexes / 3;

            CoACD_MeshArray hulls = CoACD_run(
                &input,
                threshold,             // threshold
                -1,                    // max_convex_hull (no limit)
                preprocess_mode, // preprocess_mode
                prep_resolution,       // prep_resolution (voxel grid)
                resolution,            // sample_resolution (ACTUAL accuracy)
                20,                    // mcts_nodes
                100,                   // mcts_iterations
                mcts_max_depth,        // mcts_max_depth
                false,                 // pca
                false,                 // merge (DISABLED FOR FIDELITY)
                decimate,              // decimate
                MAX_POINTS_ON_WINDING, // max_ch_vertex
                false,                 // extrude
                0.01,                  // extrude_margin
                COACD_APX_CH,          // apx_mode
                1234                   // seed
            );

            bspbrush_t *brushes = BrushesFromHullsCoACD(hulls, ds->shaderInfo);
            hulls_list = CombineBrushes(hulls_list, brushes);

            CoACD_freeMeshArray(hulls);
            free(meshVerts);
            free(meshIndexes);
        }
    }

    return hulls_list;
}
