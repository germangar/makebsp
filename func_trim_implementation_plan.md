# `func_trim` (Iterative Plane Trimming) Implementation Plan

## Goal
To implement `func_trim` (formerly `func_snip`) for `misc_model` slicing without relying on full boolean CSG operations. This avoids the massive dependencies of `MeshLib`'s boolean engine which were aggressively stripped from `MeshLib-Lite`.

The strategy leverages the user's brilliant workaround: using `mrTrimMeshWithPlane` (via a customized C++ wrapper `mrTrimMiscModelMesh`) to iteratively slice meshes based on the **drawable** planes of brushes within the `func_trim` entity.

## Actual Implementation Details (Working Pipeline)

### 1. `q3map/csg_mesh.h` & `q3map/csg_mesh.c` (Entity Parsing and Storage)
- Created `funcTrimOperator_t` struct to store up to 128 `plane_t` objects, the bounding box of all brushes, and a `target` string.
- `func_trim` entities are parsed in `q3map/map.c`. For each brush, drawable planes (those lacking `SURF_NODRAW` and `SURF_SKIP`) are inverted (so we keep the negative space behind the plane) and added to the operator.
- The `target` key is read from the entity epairs.
- The entity's brushes and epairs are freed immediately (`mapent->brushes = NULL`, `mapent->epairs = NULL`) ensuring the `func_trim` is destroyed and never enters the BSP tree.

### 2. Targeting Logic
- In `PerformMeshCSG` (`q3map/csg_mesh.c`), before processing a model, we retrieve its `targetname` directly from the entity that spawned it:
  `const char *instTargetName = ValueForKey(inst->creator, "targetname");`
- When evaluating each `funcTrimOperator_t`, if its `target` field is non-empty, we strictly compare it against `instTargetName`.
- This seamlessly supports multiple `misc_model` instances sharing the same `targetname` and being cut by a single `func_trim`. If a `func_trim` has no `target`, it defaults to acting globally on all intersecting `misc_model`s.

### 3. MeshLib Integration & Interpolation (`MRMeshTrimWithPlane.cpp`)
- `csg_mesh.c` calls `mrTrimMiscModelMesh`, passing all geometry buffers (positions, normals, UVs, colors) to MeshLib.
- We construct an `MR::Mesh` from the raw `miscModelMesh_t` buffers.
- During the `MR::trimWithPlane` operation, we use an `onEdgeSplitCallback`.
- **CRITICAL FIX**: Instead of zeroing out normals for new cut-vertices (which broke in-game lighting and produced black artifacts), we properly interpolate the normal vectors, UVs, and vertex colors between the origin and destination vertices of the split edge based on the edge split weight.
- Following the cut, `mesh.pack()` is executed to garbage-collect and remove all orphaned vertices (vertices belonging to the discarded side of the plane), resulting in a highly optimized "trisoup" without unused data.

### 4. Re-packing the Mesh
- The optimized `MR::Mesh` is exported back into standard C arrays for `miscModelMesh_t`.
- We re-assign the new vertex counts and index counts.
- `mm->wasCut` is flagged to `qtrue` and `mm->hasOriginalUVs` to `qfalse` to force `xatlas` to regenerate the lightmap UVs for the newly generated cut geometry.

## Files Modified / Created

| File | Action | Notes |
|------|--------|-------|
| `q3map/map.c` | MODIFY | Added `func_trim` case in `ProcessMapEntities` to build operators, extract `target`, and erase the entity. |
| `q3map/csg_mesh.c` | NEW | Added `StoreFuncTrimOperator` and `PerformMeshCSG`. Handles bounds-checking, target validation, and dispatch to MeshLib. |
| `q3map/csg_mesh.h` | NEW | Public declarations for `funcTrimOperator_t` and functions. |
| `libs/MeshLib-Lite/MRMeshC/MRMeshTrimWithPlane.cpp` | MODIFY | Advanced `mrTrimMiscModelMesh` to handle full buffer ingestion and edge-split interpolation for normals/UVs/colors. |
