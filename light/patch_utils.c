#include "light.h"

/*
=================
UnifyMeshNormals

Averages the normals of all vertices in the mesh that share the same physical world position.
This fixes shading singularities (e.g., the "tip" of a fan-shaped Bezier patch).
Rewritten for maximum accuracy: uses equivalence classes to ensure all vertices
in a singularity get the exact same final normal, and checks dot products to
prevent averaging opposite-facing normals on thin geometry.
=================
*/
static void UnifyMeshNormals(mesh_t *mesh) {
    int i, j;
    int numVerts = mesh->width * mesh->height;
    int *group = malloc(numVerts * sizeof(int));
    vec3_t *groupNormals = malloc(numVerts * sizeof(vec3_t));
    
    // Initialize each vertex to its own group
    for (i = 0; i < numVerts; i++) {
        group[i] = i;
        VectorCopy(mesh->verts[i].normal, groupNormals[i]);
    }
    
    // Pass 1: Build equivalence classes (find all vertices that share a position and face the same way)
    for (i = 0; i < numVerts; i++) {
        for (j = i + 1; j < numVerts; j++) {
            // Already in the same group?
            if (group[i] == group[j]) continue;

            vec3_t delta;
            VectorSubtract(mesh->verts[i].xyz, mesh->verts[j].xyz, delta);
            
            // Check if they are physically co-located (distance < 0.1)
            // Using 0.01 for squared distance (0.1 * 0.1)
            if (DotProduct(delta, delta) < 0.01f) {
                // Check if they are facing the same general hemisphere
                // This prevents averaging the front and back of a thin patch
                if (DotProduct(mesh->verts[i].normal, mesh->verts[j].normal) > 0.0f) {
                    
                    // Merge group j into group i
                    int oldGroup = group[j];
                    int targetGroup = group[i];
                    for (int k = 0; k < numVerts; k++) {
                        if (group[k] == oldGroup) {
                            group[k] = targetGroup;
                        }
                    }
                }
            }
        }
    }
    
    // Pass 2: Accumulate normals for each group
    // Reset accumulators
    for (i = 0; i < numVerts; i++) {
        VectorClear(groupNormals[i]);
    }
    
    // Sum normals
    for (i = 0; i < numVerts; i++) {
        int g = group[i];
        VectorAdd(groupNormals[g], mesh->verts[i].normal, groupNormals[g]);
    }
    
    // Pass 3: Normalize and apply back to mesh
    for (i = 0; i < numVerts; i++) {
        int g = group[i];
        if (VectorLength(groupNormals[g]) > 0.001f) {
            vec3_t finalNormal;
            VectorCopy(groupNormals[g], finalNormal);
            VectorNormalize(finalNormal, finalNormal);
            VectorCopy(finalNormal, mesh->verts[i].normal);
        }
    }
    
    free(group);
    free(groupNormals);
}

/*
=========================
OverrideOpenEdgeNormals


Kept for reference but no longer called from the main tessellation path.
The two-pass MakeMeshNormals in DrawSurfaceForMesh now produces correct
edge normals without needing this post-hoc correction.
=========================
*/
static void OverrideOpenEdgeNormals(mesh_t *mesh) {
    int i;
    int w = mesh->width;
    int h = mesh->height;

    if (w < 2 || h < 2) return;

    // Detect wrapping per axis (same logic as MakeMeshNormals)
    qboolean wrapU = qtrue;
    for (i = 0; i < h; i++) {
        vec3_t delta;
        VectorSubtract(mesh->verts[i * w].xyz, mesh->verts[i * w + w - 1].xyz, delta);
        if (VectorLength(delta) > 1.0f) { wrapU = qfalse; break; }
    }

    qboolean wrapV = qtrue;
    for (i = 0; i < w; i++) {
        vec3_t delta;
        VectorSubtract(mesh->verts[i].xyz, mesh->verts[i + (h - 1) * w].xyz, delta);
        if (VectorLength(delta) > 1.0f) { wrapV = qfalse; break; }
    }

    // Left edge (column 0) — open when U doesn't wrap
    if (!wrapU) {
        vec3_t edgeTan, perpTan, edgeNormal;
        VectorSubtract(mesh->verts[(h - 1) * w].xyz, mesh->verts[0].xyz, edgeTan);
        VectorSubtract(mesh->verts[1].xyz, mesh->verts[0].xyz, perpTan);

        if (VectorNormalize(edgeTan, edgeTan) > 0.001f &&
            VectorNormalize(perpTan, perpTan) > 0.001f) {
            CrossProduct(edgeTan, perpTan, edgeNormal);
            if (VectorNormalize(edgeNormal, edgeNormal) > 0.001f) {
                if (DotProduct(edgeNormal, mesh->verts[0].normal) < 0)
                    VectorScale(edgeNormal, -1.0f, edgeNormal);
                for (i = 0; i < h; i++)
                    VectorCopy(edgeNormal, mesh->verts[i * w].normal);
            }
        }
    }

    // Right edge (column w-1) — open when U doesn't wrap
    if (!wrapU) {
        vec3_t edgeTan, perpTan, edgeNormal;
        VectorSubtract(mesh->verts[(h - 1) * w + w - 1].xyz, mesh->verts[w - 1].xyz, edgeTan);
        VectorSubtract(mesh->verts[w - 2].xyz, mesh->verts[w - 1].xyz, perpTan);

        if (VectorNormalize(edgeTan, edgeTan) > 0.001f &&
            VectorNormalize(perpTan, perpTan) > 0.001f) {
            CrossProduct(edgeTan, perpTan, edgeNormal);
            if (VectorNormalize(edgeNormal, edgeNormal) > 0.001f) {
                if (DotProduct(edgeNormal, mesh->verts[w - 1].normal) < 0)
                    VectorScale(edgeNormal, -1.0f, edgeNormal);
                for (i = 0; i < h; i++)
                    VectorCopy(edgeNormal, mesh->verts[i * w + w - 1].normal);
            }
        }
    }

    // Bottom edge (row 0) — open when V doesn't wrap
    if (!wrapV) {
        vec3_t edgeTan, perpTan, edgeNormal;
        VectorSubtract(mesh->verts[w - 1].xyz, mesh->verts[0].xyz, edgeTan);
        VectorSubtract(mesh->verts[w].xyz, mesh->verts[0].xyz, perpTan);

        if (VectorNormalize(edgeTan, edgeTan) > 0.001f &&
            VectorNormalize(perpTan, perpTan) > 0.001f) {
            CrossProduct(edgeTan, perpTan, edgeNormal);
            if (VectorNormalize(edgeNormal, edgeNormal) > 0.001f) {
                if (DotProduct(edgeNormal, mesh->verts[0].normal) < 0)
                    VectorScale(edgeNormal, -1.0f, edgeNormal);
                for (i = 0; i < w; i++)
                    VectorCopy(edgeNormal, mesh->verts[i].normal);
            }
        }
    }

    // Top edge (row h-1) — open when V doesn't wrap
    if (!wrapV) {
        vec3_t edgeTan, perpTan, edgeNormal;
        VectorSubtract(mesh->verts[(h - 1) * w + w - 1].xyz, mesh->verts[(h - 1) * w].xyz, edgeTan);
        VectorSubtract(mesh->verts[(h - 2) * w].xyz, mesh->verts[(h - 1) * w].xyz, perpTan);

        if (VectorNormalize(edgeTan, edgeTan) > 0.001f &&
            VectorNormalize(perpTan, perpTan) > 0.001f) {
            CrossProduct(edgeTan, perpTan, edgeNormal);
            if (VectorNormalize(edgeNormal, edgeNormal) > 0.001f) {
                if (DotProduct(edgeNormal, mesh->verts[(h - 1) * w].normal) < 0)
                    VectorScale(edgeNormal, -1.0f, edgeNormal);
                for (i = 0; i < w; i++)
                    VectorCopy(edgeNormal, mesh->verts[(h - 1) * w + i].normal);
            }
        }
    }
}

/*
=========================
SubdividePatchForLighting

q3map2-matching tessellation pipeline:
  SubdivideMesh -> PutMeshOnCurve -> RemoveLinearMeshColumnsRows

Normals are NOT recalculated after tessellation. They propagate from the coarse
control point normals (set by DrawSurfaceForMesh's two-pass strategy) through
spherical interpolation in the fixed LerpDrawVert. PutMeshOnCurve never touches
normals — only xyz, st, and lightmap coordinates are curve-fitted.
=========================
*/
mesh_t *SubdividePatchForLighting(dsurface_t *ds, float ssize) {
    mesh_t srcMesh, *mesh, *final;

    srcMesh.width  = ds->patchWidth;
    srcMesh.height = ds->patchHeight;
    srcMesh.verts  = &drawVerts[ds->firstVert];

    if (IsMeshPlanar(&srcMesh)) {
        /* Planar: Use strictly the geometry logic that matches the old BSP path 
           to guarantee lightmap texel alignment. */
        mesh_t *subdivided;
        int widthtable[MAX_EXPANDED_AXIS], heighttable[MAX_EXPANDED_AXIS];

        mesh = SubdivideMesh(srcMesh, 8.0f, 999.0f);
        PutMeshOnCurve(*mesh);
        
        localSurfaces[(int)(ds - drawSurfaces)].isPlanarPatch = qtrue;

        subdivided = RemoveLinearMeshColumnsRows(mesh);
        FreeMesh(mesh);

        /* Align to the lightmap atlas grid using the same ssize as the BSP phase */
        final = SubdivideMeshQuads(subdivided, ssize, LIGHTMAP_WIDTH - 2, widthtable, heighttable);
        FreeMesh(subdivided);

        return final;
    }

    /* Step 1: Adaptive Bezier subdivision.
       Normals are correctly spherically interpolated via the fixed LerpDrawVert,
       so no MakeMeshNormals call is needed here. The coarse control point normals
       (calculated in DrawSurfaceForMesh with the two-pass strategy) propagate
       seamlessly into the dense mesh through subdivision. */
    mesh = SubdivideMesh(srcMesh, 8.0f, 999.0f);

    /* Step 2: Push xyz, st, and lightmap onto the Bezier curve.
       Normals are explicitly NOT touched here — this is the core invariant:
       normals come from spherical interpolation, positions from the curve. */
    PutMeshOnCurve(*mesh);

    /* Step 3: Record whether this is a planar patch for the lighting system. */
    localSurfaces[(int)(ds - drawSurfaces)].isPlanarPatch = qfalse;

    /* Step 4: Remove co-linear rows/columns to keep the mesh lean.
       Matches q3map2's TessellatedMesh exactly:
         SubdivideMesh2 -> PutMeshOnCurve -> RemoveLinearMeshColumnsRows
         (MakeMeshNormals is commented out in q3map2's TessellatedMesh) */
    final = RemoveLinearMeshColumnsRows(mesh);
    FreeMesh(mesh);

    return final;
}
