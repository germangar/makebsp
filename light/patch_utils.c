#include "light.h"

/*
=================
UnifyMeshNormals

Averages the normals of all vertices in the mesh that share the same physical world position.
This fixes shading singularities (e.g., the "tip" of a fan-shaped Bezier patch).
=================
*/
static void UnifyMeshNormals(mesh_t *mesh) {
    int i, j;
    int numVerts = mesh->width * mesh->height;
    vec3_t *accumNormals = malloc(numVerts * sizeof(vec3_t));
    
    for (i = 0; i < numVerts; i++) {
        VectorCopy(mesh->verts[i].normal, accumNormals[i]);
    }
    
    for (i = 0; i < numVerts; i++) {
        for (j = i + 1; j < numVerts; j++) {
            vec3_t delta;
            VectorSubtract(mesh->verts[i].xyz, mesh->verts[j].xyz, delta);
            if (VectorLength(delta) < 0.1f) {
                VectorAdd(accumNormals[i], mesh->verts[j].normal, accumNormals[i]);
                VectorAdd(accumNormals[j], mesh->verts[i].normal, accumNormals[j]);
            }
        }
    }
    
    for (i = 0; i < numVerts; i++) {
        VectorNormalize(accumNormals[i], mesh->verts[i].normal);
    }
    
    free(accumNormals);
}

/*
================
CheckPatchPlanar

Returns qtrue if all generated vertices of the patch mesh lie on a single plane.
If planar, outNormal contains the uniform outward-facing normal.
================
*/
static qboolean CheckPatchPlanar(mesh_t *mesh, int surfIdx, vec3_t outNormal) {
    int numVerts = mesh->width * mesh->height;
    if (numVerts < 3) return qfalse;

    vec3_t p0, p1, p2, n;
    VectorCopy(mesh->verts[0].xyz, p0);

    // Find a valid normal from the first three non-collinear points
    qboolean found = qfalse;
    for (int i = 1; i < numVerts - 1; i++) {
        for (int j = i + 1; j < numVerts; j++) {
            VectorSubtract(mesh->verts[i].xyz, p0, p1);
            VectorSubtract(mesh->verts[j].xyz, p0, p2);
            CrossProduct(p1, p2, n);
            if (VectorNormalize(n, n) > 0.001f) {
                found = qtrue;
                break;
            }
        }
        if (found) break;
    }

    if (!found) return qfalse; // Degenerate patch

    float dist = DotProduct(p0, n);
    float maxDist = 0.0f;
    for (int i = 0; i < numVerts; i++) {
        float d = DotProduct(mesh->verts[i].xyz, n);
        float dev = fabs(d - dist);
        if (dev > maxDist) maxDist = dev;
    }

    if (maxDist > 0.1f) return qfalse;

    // Ensure normal points outward (matches general direction of original control points)
    dsurface_t *ds = &drawSurfaces[surfIdx];
    if (DotProduct(n, drawVerts[ds->firstVert].normal) < 0.0f) {
        VectorSubtract(vec3_origin, n, n);
    }

    VectorCopy(n, outNormal);
    return qtrue;
}

/*
=========================
SubdividePatchForLighting

Replicates the exact subdivision pipeline used by the BSP phase
(AllocateLightmapForPatch in q3map/lightmaps.c) to guarantee:
  1. Matching lightmap dimensions (no miscount errors)
  2. Consistent outward-facing normals via MakeMeshNormals
     on the actual CCW-wound triangle mesh
=========================
*/
mesh_t *SubdividePatchForLighting(dsurface_t *ds, float ssize) {
    mesh_t srcMesh, *mesh, *subdivided, *final;
    vec3_t planarNormal;
    int widthtable[MAX_EXPANDED_AXIS], heighttable[MAX_EXPANDED_AXIS];

    srcMesh.width = ds->patchWidth;
    srcMesh.height = ds->patchHeight;
    srcMesh.verts = &drawVerts[ds->firstVert];

    // Step 1: Adaptive Bezier subdivision (always produces an odd-sized grid)
    mesh = SubdivideMesh(srcMesh, 8.0f, 999.0f);

    // Step 2: Push approximating points onto the Bezier curve
    //         Safe here because SubdivideMesh always produces an odd grid
    PutMeshOnCurve(*mesh);

    // Step 3: Compute normals
    qboolean isPlanar = CheckPatchPlanar(mesh, (int)(ds - drawSurfaces), planarNormal);
    
    // Compute smooth normals from the curved CCW-wound mesh
    // This gives correct outward-facing normals regardless of patch orientation
    MakeMeshNormals(*mesh);

    // Step 4: Remove co-linear rows/columns to keep the mesh lean
    subdivided = RemoveLinearMeshColumnsRows(mesh);
    FreeMesh(mesh);

    // Step 5: Align to the lightmap atlas grid using the same ssize as the BSP phase
    //         This guarantees lightmapWidth/Height match exactly
    final = SubdivideMeshQuads(subdivided, ssize, LIGHTMAP_WIDTH, widthtable, heighttable);
    FreeMesh(subdivided);

    // Step 7: Unify normals at singularities (collapsed edges / fan shapes)
    UnifyMeshNormals(final);

    if (isPlanar) {
        // Enforce perfectly uniform normal to prevent shading artifacts on flat patches
        int numVerts = final->width * final->height;
        for (int i = 0; i < numVerts; i++) {
            VectorCopy(planarNormal, final->verts[i].normal);
        }
    }

    return final;
}


