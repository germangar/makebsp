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

## Conclusion
This approach elegantly turns a classic BSP compiler into a modern static-mesh generator. The collision and portal algorithms remain untouched and robust, while the visual output is optimized into chunked, single-draw-call Trisoups with automatically generated lightmap atlases via `xatlas`.
