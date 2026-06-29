# Deep Study: Post-TJunction Planar Surface Merging (Trisoup Decoupling)

## 1. Goal
The primary objective of this architecture is to decouple the **physical collision tree (CSG)** from the **visual rendering meshes (`mapDrawSurfs`)**. 

Classic BSP engines render every single planar face as an individual draw call. By hooking into the pipeline right after the T-Junctions have been sealed, we can algorithmically weld adjacent planar surfaces (walls, floors, ceilings) into massive contiguous `MST_TRIANGLE_SOUP` structures. This dramatically slashes draw calls while maintaining 100% physically accurate collision and visibility calculations.

## 2. Pipeline Timing (The Injection Point)
This pass must execute in `q3map/bsp.c` during `ProcessWorldModel` and `ProcessSubModel`, strictly at this exact phase:

```c
    if (!notjunc) {
        FixTJunctions(e);
    }

    // [INJECTION POINT] 
    // MergePlanarToTrisoups(e);

    MakeEntityDecals(e);
    GenerateHalos(e);
    AllocateLightmaps(e);
    FilterDrawsurfsIntoTree(e, tree);
```

### Why this exact moment?
`FixTJunctions` forces long, contiguous walls to be split into matching line segments that perfectly align with the adjacent floor/ceiling vertices. This creates a perfect **1:1 topological matching of vertices**. Tracing the edges and welding the geometry becomes mathematically trivial because you just connect identical coordinates, bypassing the need for complex polygon intersection math.

## 3. The Merging Algorithm

A theoretical `MergePlanarToTrisoups(entity_t *e)` function would execute the following steps:

1. **Shader Grouping**: A single `mapDrawSurface_t` can only hold one shader. We must first bucket all `MST_PLANAR` surfaces in the entity by their `shaderInfo`. (A brick wall and a concrete floor cannot be merged into the same drawsurf).
2. **Topological Welding**: Within a shader bucket, iterate the surfaces. Because `FixTJunctions` has already run, two adjacent surfaces will share two or more vertices with exact (or epsilon-close) XYZ coordinates. 
3. **Triangulation**: `MST_PLANAR` surfaces are simple N-gons (windings). To merge them into a Trisoup, each planar polygon must be triangulated. Since they are guaranteed convex, a simple triangle fan from vertex 0 (`0, 1, 2`, `0, 2, 3`, etc.) is sufficient.
4. **Surface Allocation**: Allocate a new `mapDrawSurface_t`. Set `ds->isDecal = qfalse` and critically, set `ds->miscModel = qtrue`. Copy the welded vertices and the generated triangle indexes.
5. **Garbage Collection**: Delete the original `MST_PLANAR` surfaces by setting their `numVerts = 0`, ensuring they are skipped by the rest of the compilation pipeline.

## 4. Lightmap UV Generation (`AllocateLightmaps`)

Usually, merging planar surfaces at 90-degree angles destroys lightmaps because simple orthographic projection stretches across the corners. However, `makebsp` has a built-in fallback that makes this effortless.

When `AllocateLightmaps()` runs next, it will observe `ds->miscModel == qtrue` (identifying our merged room as a 3D model). It will naturally route this surface to `q3map/misc_model.c`. Because this surface has no predefined lightmap UVs, the code will automatically trigger:
```c
xatlasUVs = GenerateXAtlasUVsFromScratch(mesh, ssize, inst->lightmapScale, scale_vec);
```
**xatlas** will take the 3D topology of the entire merged room, algorithmically cut seams along sharp angles, unfold the walls into flat 2D charts, and tightly pack them into a square lightmap atlas. **Perfect UV generation happens completely autonomously.**

## 5. BSP Tree Visibility & PVS

One of the largest concerns with decoupling rendering is breaking visibility (the PVS leaves). 

If we merged the surfaces, how do we link them to the BSP leaves? We don't have to.
When `FilterDrawsurfsIntoTree()` executes at the end of the pipeline, it uses the following logic:
```c
        if (ds->miscModel) {
            refs = FilterMiscModelSurfIntoTree(ds, tree);
            // ...
        }
```
Because we flagged our merged mesh as `miscModel`, `makebsp` will use its robust arbitrary-mesh filtering logic. It will push every individual triangle of our massive merged room down through the BSP nodes, dynamically assigning them to the exact leaves they touch. Visibility and culling will continue to function flawlessly.

## 6. Engine Limitations to Engineer Around

While the architecture perfectly supports this, legacy engine constraints must be respected during the merging loop:

> [!WARNING]
> **Drawsurf Vertex Limits**
> Legacy Quake 3 engines (and derived engines like QFusion) have hard limits on the number of vertices and indexes a single `mapDrawSurface_t` can hold (often bounded by `MAX_SURFACE_VERTS`, typically 1000 to 4096).
> 
> The `MergePlanarToTrisoups` algorithm **must** track its accumulated vertex/index count. If `current_verts + next_polygon_verts >= ENGINE_MAX_LIMIT`, it must close the current Trisoup, emit it, and start a fresh Trisoup for the remaining surfaces in the room.

> [!WARNING]
> **Lightmap Texture Space (Sample Size constraints)**
> We must track the physical world-space area of the surfaces being merged relative to their `samplesize`. If the accumulated area requires a lightmap that exceeds the maximum atlas texture size (e.g., 512x512) to maintain the desired luxel density, the merging must be halted for the current Trisoup and a new one started.

## Conclusion
This approach elegantly turns a classic BSP compiler into a modern static-mesh generator. The collision and portal algorithms remain untouched and robust, while the visual output is optimized into chunked, single-draw-call Trisoups with automatically generated lightmap atlases via `xatlas`.

## 7. Corner Smoothing Geometry (Support Edges / Edge Chamfering)

During the merging process, sharp intersections between planar surfaces (like a 90-degree wall/floor corner) create harsh lighting divisions. The fix is to automatically generate **Support Edges** (also called "Holding Edges" or "Edge Chamfering with custom normals"). This is the same technique used in hard-surface 3D modelling to make edges catch light smoothly without high-poly subdivision.

### 7.1 State of Surfaces at the Injection Point

Understanding the exact data state is critical. By the time `MergePlanarToTrisoups` would run, the surfaces have progressed through the following stages:

1. **`DrawSurfaceForSide()`** converts each brush winding into a `mapDrawSurface_t`. At this point, `ds->verts[i].xyz` are explicit 3D coordinates, and `ds->verts[i].normal` is set to a copy of the flat face plane normal. The surface is a simple convex N-gon loop. **No index buffer yet** (`ds->numIndexes` is 0).

2. **`MergeSides()`** may join coplanar same-shader surfaces into larger convex polygons via convex hull, but they remain flat N-gon vertex loops.

3. **`FixTJunctions()`** calls `FixSurfaceJunctions()` on every planar surface, which walks each edge and inserts extra colinear vertices where neighboring surfaces required splits. After this, a wall polygon is no longer just 4 vertices; it may be many more, with clusters of colinear vertices running along any edge shared with the floor or ceiling.

**Conclusion:** At the injection point, each `mapDrawSurface_t` is a flat convex polygon stored as an ordered vertex loop in `ds->verts[0..numVerts-1]`. There is no index buffer. Triangulation happens later, inside `AllocateLightmaps` when the surface is routed to the `miscModel` (xatlas) path.

> [!IMPORTANT]
> This is exactly why we must do edge chamfering **before** triangulation. After xatlas triangulates the mesh, inserting a support edge would slice through diagonal triangle edges, creating degenerate slivers. Before triangulation, we only have a flat convex polygon loop — and inserting a parallel offset edge into a convex polygon is clean, well-defined geometry.

### 7.2 The Critical Problem: T-Junction Vertices on Corner Edges

The corner edge between a wall and a floor is no longer a simple edge from vertex A to vertex B. After `FixTJunctions`, that logical edge may contain 10+ colinear vertices, each inserted to match T-junction splits from adjacent surfaces.

This means a **"corner edge" is not one edge — it is a chain of collinear sub-edges**, all sharing the same boundary of the adjacent face. This is the core geometric challenge.

### 7.3 The Correct Algorithm: Per-Face 2D Polygon Inset

The cleanest approach is to operate entirely within the 2D coordinate space of each face's plane, independently, before any merging into a Trisoup. Here is the full step-by-step:

**Step 1 — Identify Corner Edges**

Walk all surface pairs after `FixTJunctions`. For each pair `(dsA, dsB)` where at least one surface borders the other (they share colinear edge vertices), compute the dot product of their plane normals:
```c
float dot = DotProduct(normalA, normalB);
// dot < cos(30°) → angle > 30° → mark as a sharp corner
if (dot < CORNER_SMOOTH_THRESHOLD) {
    // mark the shared edge chain on dsA and dsB for chamfering
}
```
The shared edge detection reuses the same principle as `WindingsShareEdge()` in `surface.c`, but extended to work on collinear vertex chains (sequences of vertices lying on the same line).

**Step 2 — Compute the Smoothed Normal at the Corner**

For the corner's shared edge, compute the blended normal. It is the normalized sum of the two face normals:
```c
vec3_t blendedNormal;
VectorAdd(normalA, normalB, blendedNormal);
VectorNormalize(blendedNormal, blendedNormal);
```
This is the normal that will be assigned to the real outer corner vertices.

**Step 3 — Compute the 2D Inset Vertices**

For each vertex `V` in the shared corner edge chain on surface `dsA`:

1. Find the **edge direction** of the shared boundary: `edgeDir = normalize(nextVertex - V)`.
2. Find the **inset direction**: the vector perpendicular to `edgeDir` and lying in the plane of face `dsA`, pointing inward. This is: `insetDir = cross(planeNormal, edgeDir)`.
3. The new **inner support vertex** is: `V_inner = V + insetDir * CHAMFER_WIDTH` (e.g., `CHAMFER_WIDTH = 1.0` unit).

Note: At corners between two chamfered edges (i.e., at the endpoint of the chain, where two edges meet), the inset vertex is found by intersecting the two inset lines in the plane, not by simple offset. This avoids gaps or overlaps at polygon corners.

**Step 4 — Rebuild the Polygon**

Replace the original polygon vertex loop with the new topology. For surface `dsA`:

- Original: `[... V_prev, V0, V1, V2 ..., V_last, V_next ...]` (where V0..V_last is the shared edge chain)
- New: `[... V_prev, V0, V1, V2 ..., V_last, V_next ..., V_last_inner, ..., V1_inner, V0_inner ]`

That is: insert the inset support vertices as a new run of vertices at the end of the chain, creating a thin "strip" polygon around the original corner edge. The body of the polygon is now the inner face with slightly smaller dimensions.

**Step 5 — Assign Normals**

- All original `V_prev`, `V_next`, and inner body vertices: **retain the flat face normal** unchanged.
- The original corner vertices `V0..V_last`: assign the **blended normal** from Step 2.
- The new inner support vertices `V0_inner..V_last_inner`: assign the **flat face normal** (they mark the "start" of the flat region).

**Step 6 — Triangulate Cleanly**

Because we split the polygon *before* triangulation, both the inner face polygon and the border strip polygon are still simple convex (or near-convex) polygons. A simple triangle fan from vertex 0 on each sub-polygon produces clean geometry with no degenerate triangles.

### 7.4 Cross-Shader Applicability

This chamfer technique applies even when the two intersecting surfaces have **different shaders** and will not be merged into the same Trisoup. The normals at the corner vertices are modified independently on each face, but are set to the **exact same blended normal vector**. Since both surfaces share the exact same 3D position and normal at that corner, the lighting calculation produces a seamless, matching gradient from both sides of the texture seam, creating the visual illusion of a smooth bevel even across a shader change.

### 7.5 Choosing `CHAMFER_WIDTH`

The width of the chamfer strip should be very small relative to the face size — typically **1 to 4 units** in Quake world units. It should be narrow enough to be invisible as geometry, but wide enough that the normal interpolation from `blendedNormal` to `flatNormal` happens over enough screen pixels to produce a smooth gradient. This value could be exposed as a global compiler option or a per-shader parameter.

