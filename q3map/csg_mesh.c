#include "qbsp.h"
#include "csg_mesh.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRMesh.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRMeshTrimWithPlane.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRMeshTopology.h"

static funcTrimOperator_t g_csgOps[MAX_CSG_OPERATORS];
static int                g_numCsgOps = 0;

void StoreFuncTrimOperator(funcTrimOperator_t *op) {
    if (g_numCsgOps >= MAX_CSG_OPERATORS) {
        _printf("WARNING: Too many func_trim entities (max %d). Ignoring.\n", MAX_CSG_OPERATORS);
        return;
    }
    g_csgOps[g_numCsgOps++] = *op;
}

void FreeFuncTrimOperators(void) {
    // funcTrimOperator_t contains only plain plane_t structs — no heap resources.
    g_numCsgOps = 0;
}

void PerformMeshCSG(int startInst, int endInst) {
    if (g_numCsgOps == 0)
        return;

    _printf("--- PerformMeshCSG ---\n");

    for (int i = startInst; i < endInst; i++) {
        modelInstance_t *inst = &modelInstances[i];

        for (int j = 0; j < inst->numMeshes; j++) {
            miscModelMesh_t *mm = inst->meshes[j];
            if (!mm || mm->numVerts <= 0 || mm->numIndices <= 0)
                continue;

            const char *instTargetName = "";
            if (inst->creator) {
                instTargetName = ValueForKey(inst->creator, "targetname");
            }

            // Fast AABB pre-reject against all operators
            qboolean meshOverlaps = qfalse;
            for (int o = 0; o < g_numCsgOps && !meshOverlaps; o++) {
                funcTrimOperator_t *op = &g_csgOps[o];
                if (op->target[0] != '\0' && Q_stricmp(op->target, instTargetName) != 0)
                    continue;
                if (!(op->mins[0] > mm->maxs[0] || op->maxs[0] < mm->mins[0] ||
                      op->mins[1] > mm->maxs[1] || op->maxs[1] < mm->mins[1] ||
                      op->mins[2] > mm->maxs[2] || op->maxs[2] < mm->mins[2]))
                    meshOverlaps = qtrue;
            }
            if (!meshOverlaps)
                continue;



            // Step 1: Trim with C++ helper
            float* newPositions = NULL;
            float* newNormals = NULL;
            float* newSt = NULL;
            unsigned char* newColors = NULL;
            int* newIndices = NULL;
            int newNumVerts = 0;
            int newNumIndices = 0;

            // Collect planes
            int totalPlanes = 0;
            for (int o = 0; o < g_numCsgOps; o++) {
                funcTrimOperator_t *op = &g_csgOps[o];
                if (op->target[0] != '\0' && Q_stricmp(op->target, instTargetName) != 0)
                    continue;
                if (op->mins[0] > mm->maxs[0] || op->maxs[0] < mm->mins[0] ||
                    op->mins[1] > mm->maxs[1] || op->maxs[1] < mm->mins[1] ||
                    op->mins[2] > mm->maxs[2] || op->maxs[2] < mm->mins[2])
                    continue;
                totalPlanes += op->numPlanes;
            }

            if (totalPlanes == 0)
                continue;

            MRPlane3f *mrPlanes = malloc(totalPlanes * sizeof(MRPlane3f));
            int pIdx = 0;
            for (int o = 0; o < g_numCsgOps; o++) {
                funcTrimOperator_t *op = &g_csgOps[o];
                if (op->target[0] != '\0' && Q_stricmp(op->target, instTargetName) != 0)
                    continue;
                if (op->mins[0] > mm->maxs[0] || op->maxs[0] < mm->mins[0] ||
                    op->mins[1] > mm->maxs[1] || op->maxs[1] < mm->mins[1] ||
                    op->mins[2] > mm->maxs[2] || op->maxs[2] < mm->mins[2])
                    continue;
                for (int p = 0; p < op->numPlanes; p++) {
                    mrPlanes[pIdx].n.x = op->planes[p].normal[0];
                    mrPlanes[pIdx].n.y = op->planes[p].normal[1];
                    mrPlanes[pIdx].n.z = op->planes[p].normal[2];
                    mrPlanes[pIdx].d   = op->planes[p].dist;
                    pIdx++;
                }
            }

            mrTrimMiscModelMesh(
                mm->positions, mm->numVerts,
                mm->normals,
                mm->st, mm->colors,
                mm->indices, mm->numIndices,
                mrPlanes, totalPlanes,
                &newPositions, &newNormals, &newSt, &newColors, &newIndices,
                &newNumVerts, &newNumIndices
            );
            free(mrPlanes);

            if (newNumVerts == 0 || newNumIndices == 0) {
                free(newPositions);
                free(newNormals);
                free(newSt);
                free(newColors);
                free(newIndices);
                free(mm->positions); mm->positions = NULL;
                free(mm->normals);   mm->normals   = NULL;
                free(mm->st);        mm->st        = NULL;
                free(mm->colors);    mm->colors    = NULL;
                free(mm->indices);   mm->indices   = NULL;
                mm->numVerts       = 0;
                mm->numIndices     = 0;
                mm->wasCut         = qtrue;
                mm->hasOriginalUVs = qfalse;
                _printf("  func_trim: mesh %d of %s completely trimmed away\n", j, inst->modelName);
                continue;
            }

            int oldNumIndices = mm->numIndices;
            if (newNumVerts == mm->numVerts) {
                // No change
                free(newPositions);
                free(newNormals);
                free(newSt);
                free(newColors);
                free(newIndices);
                continue;
            }
            
            // Step 10: Recompute AABB for updated mesh
            vec3_t newMins, newMaxs;
            ClearBounds(newMins, newMaxs);
            for (int v = 0; v < newNumVerts; v++) {
                vec3_t p = { newPositions[v*3+0], newPositions[v*3+1], newPositions[v*3+2] };
                AddPointToBounds(p, newMins, newMaxs);
            }

            // Step 12: Swap old buffers for new, mark as cut
            free(mm->positions); mm->positions = newPositions;
            free(mm->normals);   mm->normals   = newNormals;
            if (mm->st) { free(mm->st); mm->st = newSt; }
            if (mm->colors) { free(mm->colors); mm->colors = newColors; }
            free(mm->indices);   mm->indices   = newIndices;
            mm->numVerts       = newNumVerts;
            mm->numIndices     = newNumIndices;
            VectorCopy(newMins, mm->mins);
            VectorCopy(newMaxs, mm->maxs);
            mm->wasCut         = qtrue;
            mm->hasOriginalUVs = qfalse;  // Force xatlas lightmap regeneration

            if (mm->numIndices != newNumIndices || mm->numVerts != newNumVerts) {
            _printf("  func_trim: mesh %d/%d of %s (%d tris -> %d tris)\n",
                j+1, inst->numMeshes, inst->modelName,
                oldNumIndices / 3, newNumIndices / 3);
            }
        }
    }
}
