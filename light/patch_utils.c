#include "light.h"

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
        
        localSurfaces[(int)(ds - drawSurfaces)].surfaceIsPlanar = qtrue;

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
    localSurfaces[(int)(ds - drawSurfaces)].surfaceIsPlanar = qfalse;

    /* Step 4: Remove co-linear rows/columns to keep the mesh lean.
       Matches q3map2's TessellatedMesh exactly:
         SubdivideMesh2 -> PutMeshOnCurve -> RemoveLinearMeshColumnsRows
         (MakeMeshNormals is commented out in q3map2's TessellatedMesh) */
    final = RemoveLinearMeshColumnsRows(mesh);
    FreeMesh(mesh);

    return final;
}
