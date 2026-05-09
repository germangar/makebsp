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
    int widthtable[MAX_EXPANDED_AXIS], heighttable[MAX_EXPANDED_AXIS];

    srcMesh.width = ds->patchWidth;
    srcMesh.height = ds->patchHeight;
    srcMesh.verts = &drawVerts[ds->firstVert];

    // Step 1: Adaptive Bezier subdivision (always produces an odd-sized grid)
    mesh = SubdivideMesh(srcMesh, 8.0f, 999.0f);

    // Step 2: Push approximating points onto the Bezier curve
    //         Safe here because SubdivideMesh always produces an odd grid
    PutMeshOnCurve(*mesh);

    // Step 3: Compute smooth normals from the curved CCW-wound mesh
    //         This gives correct outward-facing normals regardless of patch orientation
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

    return final;
}


