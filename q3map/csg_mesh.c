#include "qbsp.h"
#include "csg_mesh.h"
#include "../libs/MeshLib-Lite/MRMeshC/MRMeshC.h"

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

void PerformMeshDecimation(int startInst, int endInst)
{
    for (int i = startInst; i < endInst; i++)
    {
        modelInstance_t *inst = &modelInstances[i];
        if (inst->maxtriangles <= 0)
            continue;

        // 1. Count total triangles across all sub-meshes
        int totalTris = 0;
        for (int j = 0; j < inst->numMeshes; j++)
        {
            miscModelMesh_t *mm = inst->meshes[j];
            if (mm && mm->numIndices > 0)
                totalTris += mm->numIndices / 3;
        }

        _printf("DEBUG misc_model '%s' has %d totalTris (maxtriangles=%d)\n",
                inst->modelName, totalTris, inst->maxtriangles);

        if (totalTris <= inst->maxtriangles)
            continue;

        _printf("misc_model '%s' exceeds maxtriangles (%d > %d), decimating...\n",
                inst->modelName, totalTris, inst->maxtriangles);

        int decimatedTotalTris = 0;

        for (int j = 0; j < inst->numMeshes; j++)
        {
            miscModelMesh_t *mm = inst->meshes[j];
            if (!mm || mm->numVerts < 3 || mm->numIndices < 9)
            {
                if (mm) decimatedTotalTris += mm->numIndices / 3;
                continue;
            }

            int currentTris = mm->numIndices / 3;
            // Proportional budget, minimum 4 tris (never degenerate)
            int targetTris = (int)((float)currentTris * ((float)inst->maxtriangles / (float)totalTris));
            if (targetTris < 4) targetTris = 4;
            if (targetTris >= currentTris)
            {
                decimatedTotalTris += currentTris;
                continue;
            }

            // --- A. Pack positions into MRVector3f ---
            MRVector3f *positions = malloc(mm->numVerts * sizeof(MRVector3f));
            for (int v = 0; v < mm->numVerts; v++)
            {
                positions[v].x = mm->positions[v * 3 + 0];
                positions[v].y = mm->positions[v * 3 + 1];
                positions[v].z = mm->positions[v * 3 + 2];
            }

            // --- B. Pack triangles ---
            MRThreeVertIds *triangles = malloc(currentTris * sizeof(MRThreeVertIds));
            for (int t = 0; t < currentTris; t++)
            {
                triangles[t][0].id = mm->indices[t * 3 + 0];
                triangles[t][1].id = mm->indices[t * 3 + 1];
                triangles[t][2].id = mm->indices[t * 3 + 2];
            }

            MRMesh *mesh = mrMeshFromTriangles(positions, mm->numVerts, triangles, currentTris);
            MRTriangulation *initialTri = mrMeshGetTriangulation(mesh);
            int validFaces = 0;
            for (size_t f = 0; f < initialTri->size; f++) {
                if (initialTri->data[f][0].id >= 0) validFaces++;
            }
            _printf("mrMeshFromTriangles: input tris: %d, valid faces: %d\n", currentTris, validFaces);
            free(positions);
            free(triangles);

            if (!mesh) { decimatedTotalTris += currentTris; continue; }

            // --- C. Pack UVs and colors BEFORE call (MeshLib frees them internally) ---
            // Always allocate even if mm->st is NULL, matching decals.c pattern.
            MRVector2f *uvs = malloc(mm->numVerts * sizeof(MRVector2f));
            for (int v = 0; v < mm->numVerts; v++)
            {
                uvs[v].x = mm->st ? mm->st[v * 2 + 0] : 0.0f;
                uvs[v].y = mm->st ? mm->st[v * 2 + 1] : 0.0f;
            }
            MRColor *colors = malloc(mm->numVerts * sizeof(MRColor));
            for (int v = 0; v < mm->numVerts; v++)
            {
                colors[v].r = mm->colors ? mm->colors[v * 4 + 0] : 255;
                colors[v].g = mm->colors ? mm->colors[v * 4 + 1] : 255;
                colors[v].b = mm->colors ? mm->colors[v * 4 + 2] : 255;
                colors[v].a = mm->colors ? mm->colors[v * 4 + 3] : 255;
            }

            MRMeshAttributes attrs;
            memset(&attrs, 0, sizeof(attrs));
            attrs.uvCoords   = uvs;    attrs.numUvs    = mm->numVerts;
            attrs.vertColors = colors; attrs.numColors = mm->numVerts;

            // --- D. Configure and execute decimation ---
            MRDecimateSettings settings = mrDecimateSettingsNew();
            settings.strategy        = MRDecimateStrategyMinimizeError;
            settings.maxDeletedFaces = currentTris - targetTris;
            settings.packMesh        = true;   // MUST defragment vertex/index arrays
            settings.stabilizer      = 1e-6f;
            settings.touchBdVerts    = false;  // Do not move/tear UV seams (boundaries)
            settings.touchNearBdEdges = false;
            settings.maxAngleChange  = 0.5f;   // Prevent triangle flipping/folding (radians)
            settings.maxTriangleAspectRatio = 20.0f; // Prevent extremely sliver triangles
            settings.optimizeVertexPos = false; // PREVENT UV SWIRLING! Collapses edges exactly into existing vertices

            MRDecimateResult result = mrMeshDecimateWithAttributes(mesh, &attrs, &settings);
            _printf("MeshLib decimated: removed %d verts, %d faces. Error introduced: %f\n", 
                result.vertsDeleted, result.facesDeleted, result.errorIntroduced);

            // --- E. Extract results ---
            const MRVector3f *newPositions = mrMeshPoints(mesh);
            size_t newNumVerts = mrMeshPointsNum(mesh);

            MRTriangulation *triangulation = mrMeshGetTriangulation(mesh);
            const MRThreeVertIds *newFaces = triangulation->data;
            size_t faceSlots = triangulation->size; // includes invalid slots if packMesh worked

            // Count valid faces (guard against any residual invalid slots)
            int activeFaces = 0;
            int skippedFaces = 0;
            for (size_t f = 0; f < faceSlots; f++)
            {
                if (newFaces[f][0].id < 0) continue; // Deleted face

                if (newFaces[f][0].id >= 0 && newFaces[f][0].id < (int)newNumVerts &&
                    newFaces[f][1].id >= 0 && newFaces[f][1].id < (int)newNumVerts &&
                    newFaces[f][2].id >= 0 && newFaces[f][2].id < (int)newNumVerts)
                    activeFaces++;
                else
                    skippedFaces++;
            }
            if (skippedFaces > 0)
                _printf("WARNING: skipped %d faces because VertId >= newNumVerts (%d)!\n", skippedFaces, (int)newNumVerts);

            if (newNumVerts > 0 && activeFaces > 0)
            {
                float *outPositions = malloc(newNumVerts * 3 * sizeof(float));
                float *outNormals   = calloc(newNumVerts * 3, sizeof(float));
                // Only allocate UVs/colors if the input had them
                float *outSt     = mm->st     ? malloc(newNumVerts * 2 * sizeof(float)) : NULL;
                byte  *outColors = mm->colors ? malloc(newNumVerts * 4 * sizeof(byte))  : NULL;
                int   *outIndices = malloc(activeFaces * 3 * sizeof(int));

                // Positions + Attributes — iterate exactly newNumVerts
                for (size_t v = 0; v < newNumVerts; v++)
                {
                    outPositions[v * 3 + 0] = newPositions[v].x;
                    outPositions[v * 3 + 1] = newPositions[v].y;
                    outPositions[v * 3 + 2] = newPositions[v].z;

                    if (outSt)
                    {
                        // attrs.uvCoords is the NEW packed buffer from MeshLib, indexed by new VertId
                        outSt[v * 2 + 0] = attrs.uvCoords[v].x;
                        outSt[v * 2 + 1] = attrs.uvCoords[v].y;
                    }
                    if (outColors)
                    {
                        outColors[v * 4 + 0] = attrs.vertColors[v].r;
                        outColors[v * 4 + 1] = attrs.vertColors[v].g;
                        outColors[v * 4 + 2] = attrs.vertColors[v].b;
                        outColors[v * 4 + 3] = attrs.vertColors[v].a;
                    }
                }

                // Reconstruct Indices and Compute Normals
                int fOut = 0;
                for (size_t f = 0; f < faceSlots; f++)
                {
                    if (newFaces[f][0].id < 0) continue; // Deleted face

                    if (newFaces[f][0].id >= 0 && newFaces[f][0].id < (int)newNumVerts &&
                        newFaces[f][1].id >= 0 && newFaces[f][1].id < (int)newNumVerts &&
                        newFaces[f][2].id >= 0 && newFaces[f][2].id < (int)newNumVerts)
                    {
                        int i0 = newFaces[f][0].id;
                        int i1 = newFaces[f][1].id;
                        int i2 = newFaces[f][2].id;
                        outIndices[fOut++] = i0;
                        outIndices[fOut++] = i1;
                        outIndices[fOut++] = i2;
                    }
                }

                // Area-weighted smooth normals (proven pattern from decals.c)
                vec3_t *smoothNormals = calloc(newNumVerts, sizeof(vec3_t));
                for (int k = 0; k < activeFaces * 3; k += 3)
                {
                    int i0 = outIndices[k], i1 = outIndices[k+1], i2 = outIndices[k+2];
                    vec3_t p0 = { outPositions[i0*3], outPositions[i0*3+1], outPositions[i0*3+2] };
                    vec3_t p1 = { outPositions[i1*3], outPositions[i1*3+1], outPositions[i1*3+2] };
                    vec3_t p2 = { outPositions[i2*3], outPositions[i2*3+1], outPositions[i2*3+2] };
                    vec3_t e1, e2, fn;
                    VectorSubtract(p1, p0, e1);
                    VectorSubtract(p2, p0, e2);
                    CrossProduct(e2, e1, fn); // e2 cross e1 points outward for CCW Quake winding
                    VectorAdd(smoothNormals[i0], fn, smoothNormals[i0]);
                    VectorAdd(smoothNormals[i1], fn, smoothNormals[i1]);
                    VectorAdd(smoothNormals[i2], fn, smoothNormals[i2]);
                }
                for (size_t v = 0; v < newNumVerts; v++)
                {
                    float len = VectorLength(smoothNormals[v]);
                    if (len > 1e-6f) {
                        VectorScale(smoothNormals[v], 1.0f / len, smoothNormals[v]);
                    }
                    else { 
                        smoothNormals[v][0] = 0; smoothNormals[v][1] = 0; smoothNormals[v][2] = 1; 
                    }
                    outNormals[v*3+0] = smoothNormals[v][0];
                    outNormals[v*3+1] = smoothNormals[v][1];
                    outNormals[v*3+2] = smoothNormals[v][2];
                }
                free(smoothNormals);

                // Recompute AABB
                ClearBounds(mm->mins, mm->maxs);
                for (size_t v = 0; v < newNumVerts; v++)
                {
                    vec3_t pt = { outPositions[v*3], outPositions[v*3+1], outPositions[v*3+2] };
                    AddPointToBounds(pt, mm->mins, mm->maxs);
                }

                // Swap buffers
                free(mm->positions); mm->positions = outPositions;
                free(mm->normals);   mm->normals   = outNormals;
                if (mm->st)   { free(mm->st);     mm->st     = outSt; }
                if (mm->colors){ free(mm->colors); mm->colors = outColors; }
                free(mm->indices);   mm->indices   = outIndices;

                mm->numVerts   = (int)newNumVerts;
                mm->numIndices = activeFaces * 3;
                mm->wasCut          = qtrue;
                mm->hasOriginalUVs  = qfalse;  // Force XAtlas lightmap regen
                decimatedTotalTris += activeFaces;
            }
            else
            {
                decimatedTotalTris += currentTris;
            }

            // Always free triangulation and mesh
            mrTriangulationFree(triangulation);
            // Always free attrs buffers (MeshLib replaced them; these are the NEW ones)
            if (attrs.uvCoords)   free(attrs.uvCoords);
            if (attrs.vertColors) free(attrs.vertColors);
            mrMeshFree(mesh);
        }

        _printf("  misc_model '%s' decimated: %d -> %d triangles\n",
                inst->modelName, totalTris, decimatedTotalTris);
    }
}
