#include "light.h"

/*
=========================
SubdividePatchForLighting

Standardizes patch subdivision for both shadows (Embree) and lighting (TraceLtm).
This ensures that the lighting samples sit exactly on the triangle surfaces.
=========================
*/
mesh_t *SubdividePatchForLighting(dsurface_t *ds, float ssize) {
    mesh_t srcMesh, *mesh, *subdivided, *final;
    int widthtable[MAX_EXPANDED_AXIS], heighttable[MAX_EXPANDED_AXIS];

    srcMesh.width = ds->patchWidth;
    srcMesh.height = ds->patchHeight;
    srcMesh.verts = &drawVerts[ds->firstVert];

    // 1. Initial adaptive subdivision based on curvature (error 8.0)
    mesh = SubdivideMesh(srcMesh, 8.0f, 999.0f);
    PutMeshOnCurve(*mesh);
    MakeMeshNormals(*mesh);
    
    // 2. Optimization cleanup
    subdivided = RemoveLinearMeshColumnsRows(mesh);
    FreeMesh(mesh);

    // 3. Force to lightmap grid using ssize (min 2.0 as requested)
    float minSize = ssize;
    if (minSize < 2.0f) minSize = 2.0f;
    
    final = SubdivideMeshQuads(subdivided, minSize, LIGHTMAP_WIDTH, widthtable, heighttable);
    FreeMesh(subdivided);

    return final;
}
