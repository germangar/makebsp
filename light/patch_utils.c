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

    // Step 6: Recompute normals for the denser final grid
    //         Do NOT call PutMeshOnCurve here - final grid may be even-sized
    MakeMeshNormals(*final);

    // Step 7: Unify normals at singularities (collapsed edges / fan shapes)
    UnifyMeshNormals(final);

    return final;
}

/*
=========================
DilatePatchSurface

Fills the 1-texel padding around the patch lightmap block by copying colors
from the nearest valid edge luxels.
Also marks the dilated texels in lightAlphaMask as ALPHA_SURF_WORLD.
=========================
*/
void DilatePatchSurface(dsurface_t *ds, float *buffer) {
    if (!buffer) return;

    int w = ds->lightmapWidth;
    int h = ds->lightmapHeight;
    int x = ds->lightmapOffset[0][0];
    int y = ds->lightmapOffset[0][1];
    int lmNum = ds->lightmapNum[0];

    // Padding is at x-1, y-1, etc.
    // The allocated block in q3map was (w+2) x (h+2) starting at (x-1, y-1)
    for (int dy = -1; dy <= h; dy++) {
        for (int dx = -1; dx <= w; dx++) {
            if (dx >= 0 && dx < w && dy >= 0 && dy < h) continue;

            int curX = x + dx;
            int curY = y + dy;

            // Safety check for lightmap bounds
            if (curX < 0 || curX >= LIGHTMAP_WIDTH || curY < 0 || curY >= LIGHTMAP_HEIGHT) continue;

            int k_dst = (lmNum * LIGHTMAP_HEIGHT + curY) * LIGHTMAP_WIDTH + curX;
            
            // Dilate from nearest hit luxel in the w x h block
            int nearest_x = dx;
            if (nearest_x < 0) nearest_x = 0;
            if (nearest_x >= w) nearest_x = w - 1;
            int nearest_y = dy;
            if (nearest_y < 0) nearest_y = 0;
            if (nearest_y >= h) nearest_y = h - 1;

            int k_src = (lmNum * LIGHTMAP_HEIGHT + y + nearest_y) * LIGHTMAP_WIDTH + x + nearest_x;

            VectorCopy(&buffer[k_src * 3], &buffer[k_dst * 3]);
            if (lightAlphaMask) {
                lightAlphaMask[k_dst] = ALPHA_SURF_WORLD;
            }
        }
    }
}
