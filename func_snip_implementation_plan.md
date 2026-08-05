# func_snip Implementation Plan

## Decisions Finalized
- **Cap Texturing**: Caps on cut faces will have `(0,0)` texture UVs. Accepted, not a concern for initial implementation.
- **Brush Union**: The `MRBooleanOperationUnion` step in `ProcessMapEntities` is confirmed. It is scoped strictly to the `func_snip` entity being processed — one independent union per entity.

---

## Critical Findings from Final Audit

| # | Issue | Fix |
|---|-------|-----|
| 1 | `miscModelMesh_t`, `mesh->numMeshes`, `mesh->wasCut` **do not exist** in `modelInstance_t` (see `qbsp.h:530`). The current struct only holds `drawSurfs[]` and `collision_meshes[]`. | Must add `miscModelMesh_t` struct definition and fields `meshes[]`, `numMeshes`, `wasCut` to `qbsp.h` and `modelInstance_t`. (COMPLETED IN PREVIOUS PHASE) |
| 2 | `mrMeshTopologyGetTriVerts` **does not exist** in the MeshLib C API (`MRMeshTopology.h`). The triangulation is accessed via `mrMeshTopologyGetTriangulation(top)` which returns a `MRTriangulation*` (a vector of `MRThreeVertIds`). | Replace the per-face call in the reconstruction loop with a single `mrMeshTopologyGetTriangulation` call, then index into `triangulation->data[f]` for valid faces. |
| 3 | **FreeBrush ordering bug**: The loop calls `FreeBrush(b)` at the end of each iteration, but `b->next` has already been captured as `next` at the top. This is actually **correct** — no bug. The loop advances to `next` before `FreeBrush(b)` is called. ✅ Verified. |
| 4 | The `decals.c` cleanup pattern does **not** free `epairs`. In `ProcessMapEntities`, other entities like `func_group` and `func_terrain` call `FreeEpairs` and set `epairs = NULL` to suppress entity from BSP output. `_decal` does NOT do this because it is processed separately (not in `ProcessMapEntities`). For `func_snip`, which IS in `ProcessMapEntities`, we must follow the `func_group`/`func_terrain` pattern: call `FreeEpairs` and set `epairs = NULL`. |
| 5 | `mrBitSetTest` takes `(const MRBitSet*, size_t)`. `mrMeshTopologyGetValidFaces` returns `const MRFaceBitSet*`. `MRFaceBitSet` is an opaque struct. The cast `(const MRBitSet*)validF` is **correct** by convention (all typed BitSets are layout-compatible). ✅ Verified. |
| 6 | `MRFaceId` and `MRVertId` are `struct {int id;}` (see `MRId.h:12-14`). Designator init `{.id = f}` is correct C99. ✅ Verified. |
| 7 | `mrBooleanWithAttributes` signature: `(meshA, attrsA, meshB, attrsB, operation, params, outAttrs)`. The operator mesh (meshB) has no attributes, so `NULL` is correct for `attrsB`. ✅ Verified. |
| 8 | `mrMeshPoints` returns `const MRVector3f*` indexed by `VertId`. `mrMeshPointsNum` returns `size_t`. Both confirmed in `MRMesh.h:28,34`. ✅ Verified. |
| 9 | `mrMeshNormalFromVert` exists at `MRMesh.h:114`. Takes `(const MRMesh*, MRVertId)`. ✅ Verified. |
| 10| **CRITICAL LINKER FAILURE**: `MeshLib-Lite` lacks the `MRMeshLogic` module which provides the actual C++ implementation of `MR::boolean`. The `MRMeshC` wrapper declares `mrBoolean` but linking fails with undefined references. | **Pivot approach**: Since `func_snip` brushes are purely convex (composed of planes), intersecting a mesh with a brush is mathematically identical to sequentially trimming the mesh with each plane of the brush. We will use `MR::trimWithPlane` instead of `mrBoolean`, exposing it via a tiny C wrapper in `csg_mesh.cpp` (renamed from `.c` to allow C++ calls). |

---

## Rollout: `func_snip` Integration

### Step 1. Compile `csg_mesh.c` as C++

To directly use `MR::trimWithPlane`, we must rename `csg_mesh.c` to `csg_mesh.cpp` and update the `Makefile` or just add `extern "C"` to its functions.
To minimize `Makefile` changes, we will keep `csg_mesh.c` as C, and add a C-wrapper `MRMeshTrimWithPlane.cpp` in `libs/MeshLib-Lite/MRMeshC/` which automatically gets compiled by the existing Makefile wildcard rule.

Alternatively (and preferentially), we can implement `PerformMeshCSG` using a custom attributed winding structure `awinding_t` to perform pure C intersection clipping of `misc_model` triangles against the planes of `func_snip` brushes, eliminating the need for `MeshLib` C++ dependencies entirely.
- Convert each mesh triangle into an `awinding_t` (which carries positions, normals, STs, and colors).
- Sequentially clip against every plane of a brush using a custom `ClipAWindingEpsilon`. We keep the `SIDE_BACK` (inside) polygons.
- Do this for every brush in a `func_snip`.
- Triangulate the surviving convex polygons (windings) via a simple fan triangulation.
- Rebuild the `miscModelMesh_t` from the triangulated result.
- Flag the mesh with `wasCut = qtrue` to trigger `xatlas` UV regeneration in `IntegrateTriangleModels`.

### Step 2. `funcSnipOperator_t` struct definition

We just need the planes of the `func_snip` brushes. We will store all brushes in `funcSnipOperator_t`.

```c
typedef struct {
    int numPlanes;
    plane_t planes[MAX_BUILD_SIDES];
} snipBrush_t;

typedef struct {
    snipBrush_t brushes[MAX_BUILD_SIDES];
    int numBrushes;
    vec3_t mins, maxs;
} funcSnipOperator_t;
```

### Step 3. Intercept & Store in `map.c` (`ProcessMapEntities`)

```c
if (!strcmp("func_snip", classname))
{
    funcSnipOperator_t op = {0};
    ClearBounds(op.mins, op.maxs);

    bspbrush_t *b, *next;
    for (b = mapent->brushes; b; b = next) {
        next = b->next;

        if (op.numBrushes < MAX_BUILD_SIDES) {
            snipBrush_t *sb = &op.brushes[op.numBrushes++];
            sb->numPlanes = b->numsides;
            for (int i = 0; i < b->numsides; i++) {
                sb->planes[i] = mapplanes[b->sides[i].planenum];
            }
            AddPointToBounds(b->mins, op.mins, op.maxs);
            AddPointToBounds(b->maxs, op.mins, op.maxs);
        }
        FreeBrush(b);
    }
    mapent->brushes = NULL;

    if (op.numBrushes > 0)
        StoreFuncSnipOperator(&op);

    // Free patches
    // Suppress entity
    continue;
}
```

### Step 4. `PerformMeshCSG(startInst, endInst)`

Iterate through instances and meshes, testing against the `funcSnipOperators`.
When a collision occurs, perform the cutting, replacing the original mesh data with the trimmed data and flagging `wasCut = qtrue`.

### Step 5. BSP Hook for Integration (`bsp.c`)

Insert `PerformMeshCSG` between the load and integrate stages:

```c
int startInst, endInst;
LoadTriangleModels(e, &startInst, &endInst);
PerformMeshCSG(startInst, endInst);      // no-op if no func_snip operators
IntegrateTriangleModels(startInst, endInst, e);
```

### Step 6. `AddTriangleModels(tree)` — Unchanged

`AddTriangleModels` only stats and inserts pre-built `drawSurfs` into the BSP tree. It loops over **all** `numModelInstances` (the global counter) which is correct because at the point it is called, `LoadTriangleModels` and `IntegrateTriangleModels` have already fully populated `drawSurfs[]`. No change needed.

---

## Files Modified / Created

| File | Action | Notes |
|------|--------|-------|
| `q3map/bsp.c` | MODIFY | Update call sites in `ProcessWorldModel` and `ProcessSubModel` to insert `PerformMeshCSG` |
| `q3map/map.c` | MODIFY | Add `func_snip` case in `ProcessMapEntities` |
| `q3map/csg_mesh.c` | NEW | `StoreFuncSnipOperator`, `PerformMeshCSG` |
| `q3map/csg_mesh.h` | NEW | Public declarations |
