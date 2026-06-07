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
================
CheckPatchPlanar

Returns qtrue if all generated vertices of the patch mesh lie on a single plane.
If planar, outNormal contains the uniform outward-facing normal.
================
*/
static qboolean CheckPatchPlanar(mesh_t *mesh) {
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

    return (maxDist <= 0.1f);
}

/*
=========================
OverrideOpenEdgeNormals

For open (non-wrapping) patch edges, computes the exact analytical plane normal
from the control net geometry and overrides all edge vertex normals with it.
This must run on the pre-PutMeshOnCurve control net where tangent vectors
are mathematically exact Bezier derivatives.

MakeMeshNormals uses 8-directional neighbors including diagonals, which skews
edge normals because diagonal control points are already bending away from the
edge plane. This function bypasses that by computing the plane directly from:
  - The edge tangent (vector along the edge)
  - The perpendicular tangent (vector from edge into the surface interior)
The cross product of these two gives the exact plane normal of the adjacent brush.
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

    // Compute smooth normals on the control net BEFORE dropping to the curve.
    // This utilizes a property of Bezier curves: the vectors between subdivided control
    // points exactly match the analytical tangents at the vertices.
    MakeMeshNormals(*mesh);

    // Override edge normals with exact analytical plane normals computed from
    // the control net tangent vectors. This eliminates the diagonal skew from
    // MakeMeshNormals and ensures edge normals exactly match adjacent brush faces.
    OverrideOpenEdgeNormals(mesh);

    // Step 2: Push approximating points onto the Bezier curve
    PutMeshOnCurve(*mesh);

    // Step 3: Check planar
    localSurface_t *localSurface = &localSurfaces[(int)(ds - drawSurfaces)];
    localSurface->isPlanarPatch = CheckPatchPlanar(mesh);

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


