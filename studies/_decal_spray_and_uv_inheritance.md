# Deep Study: Decal Inherited UVs

## 1. Goal
Provide a new entity key for `_decal` (e.g. `inherituvs "1"`) that forces the generated decal mesh to retain the `st` (texture) coordinates, normals, and colors of the underlying structural geometry it projects onto. This essentially allows decals to act as perfect 1:1 detail layers over existing textures, tiling exactly with the geometry beneath them.

## 2. Core Problem: The `winding_t` Bottleneck
Currently, the process of finding what geometry lives inside the decal projector box involves "clipping" the map geometry against 6 planes (the bounds of the projector). 
MakeBSP uses the standard Quake math library for this, which relies on the `winding_t` structure. 
A `winding_t` only stores `vec3_t points` (X, Y, Z). The moment geometry enters the clipping pipeline, all UVs (`st`), normals, and colors are thrown away.

## 3. Required Solution: `drawWinding_t`
To carry the UV data through the clipping process, we must build a parallel winding library that operates on full `drawVert_t` vertices instead of raw `vec3_t` points.

### Proposed Structure:
```c
typedef struct {
    int numpoints;
    int maxpoints;
    drawVert_t verts[8]; // Variable length array, like winding_t
} drawWinding_t;
```

### Required Math Functions:
We must write a new suite of clipping functions that perform linear interpolation on the UVs (and normals/colors) along the sliced edges. 
The core engine function required is `ClipDrawWindingEpsilonStrict`:

```c
// Pseudocode for the new interpolating clipper
drawWinding_t *ClipDrawWindingEpsilonStrict(drawWinding_t *in, vec3_t normal, vec_t dist) {
    // [Standard distances to plane logic...]
    
    drawWinding_t *neww = AllocDrawWinding(in->numpoints + 4);
    
    for (i = 0; i < in->numpoints; i++) {
        drawVert_t *p1 = &in->verts[i];
        drawVert_t *p2 = &in->verts[(i + 1) % in->numpoints];
        
        // Add p1 to neww if it's on the front of the plane...
        
        if (edges_cross_plane) {
            float dot = dist1 / (dist1 - dist2);
            drawVert_t mid;
            
            // Interpolate Position
            VectorLerp(p1->xyz, p2->xyz, dot, mid.xyz);
            
            // Interpolate Texture Coordinates (ST)
            mid.st[0] = p1->st[0] + dot * (p2->st[0] - p1->st[0]);
            mid.st[1] = p1->st[1] + dot * (p2->st[1] - p1->st[1]);
            
            // Interpolate Lightmap Coordinates
            mid.lightmap[0] = p1->lightmap[0] + dot * (p2->lightmap[0] - p1->lightmap[0]);
            mid.lightmap[1] = p1->lightmap[1] + dot * (p2->lightmap[1] - p1->lightmap[1]);
            
            // Interpolate Colors
            for (int c = 0; c < 4; c++) {
                mid.color[0][c] = (byte)(p1->color[0][c] + dot * (p2->color[0][c] - p1->color[0][c]));
            }
            
            // Interpolate Normals
            VectorLerp(p1->normal, p2->normal, dot, mid.normal);
            VectorNormalize(mid->normal, mid->normal);
            
            // Add 'mid' to neww...
        }
    }
    return neww;
}
```

## 4. Pipeline Modifications in `MakeEntityDecals`

Once we have `drawWinding_t` and `ClipDrawWindingEpsilonStrict`, the pipeline updates in `decals.c` are straightforward:

1. **Initial Winding Generation**:
   When reading patches (`SubdivideMesh` output), brushes, or `misc_model`s, instead of converting their vertices to `winding_t`, we copy them into our new `drawWinding_t`. The source geometry already has perfectly mapped ST coordinates, so we simply preserve them.

2. **The Clipping Funnel (`AddDrawWindingToDecalMesh`)**:
   We create a modified version of `AddWindingToDecalMesh` that accepts a `drawWinding_t`. It will loop over the projector planes, calling `ClipDrawWindingEpsilonStrict`.

3. **Skipping Projector UV Generation**:
   At the very end of `MakeEntityDecals`, the code loops over the final `decalTrisoup` vertices and explicitly calculates new ST coordinates based on the projector's scale and orientation matrix.
   We simply wrap this loop in an `if (!inherituvs)` check. If `inherituvs` is enabled, we completely bypass this step, allowing the interpolated ST coordinates that survived the clipping process to remain in the final mesh.

## 5. Potential Complications

1. **Precision Errors**: `ClipWindingEpsilon` can sometimes introduce tiny float inaccuracies. When interpolating UVs, this might result in very subtle pixel-shifts at the seams of the decal. `0.01f` epsilon welding might also merge vertices with *slightly* different UVs, so we must ensure `WeldDecalMesh` only merges vertices if both their XYZ *and* ST coordinates match closely.
2. **Backface Culling Normals**: Patches sometimes have inverted normals. We must ensure that when we reverse a `drawWinding_t` for backface culling, we correctly swap the vertex order without messing up the ST mapping.
3. **Memory/Performance**: `drawWinding_t` is significantly larger in memory than `winding_t` because it carries full vertex payloads. However, since the clipping is done locally one surface at a time and freed immediately, the memory footprint impact is negligible.

## Conclusion
The implementation is highly feasible. The vast majority of the work is simply duplicating the Quake 3 `polylib.c` winding clipping logic, but expanding it to run linear interpolations on the extra `drawVert_t` fields. The rest of the decal pipeline is already perfectly set up to accept this data.

---

# Deep Study: `_decal_spray` Entity (Oriented Multi-Decal Generation)

## 1. Goal
Create a new entity (e.g., `_decal_spray`) that generates multiple decals jittered around a central target in a Monte Carlo pattern. To prevent severe distortion when hitting angled geometry or highly curved patches, each generated decal must be individually aligned and oriented to perfectly match the surface normal at its specific point of impact.

## 2. Core Problem: Projection Distortion
Traditional decals in Quake 3 operate as orthographic projections. If you displace the target position of an orthographic projector along a flat wall, the projection works fine. However, if the jittered target hits an angled wall, a floor, or the side of a curved cylinder, projecting from the original origin's angle will cause the decal to "smear" across the geometry, stretching uncontrollably. 

## 3. Required Solution: Virtual Projector Spawning via Raytracing
Instead of generating the geometric surfaces directly, the optimal architectural solution is to dynamically spawn "virtual" `_decal` projectors *before* the decal compilation phase begins.

### Proposed Pipeline:
1. **Pre-Processing Phase (`ProcessDecalSprays`)**: 
   We introduce a new function that runs immediately before `MakeEntityDecals(e)` in `bsp.c`.
2. **Ray Generation**: 
   For each `_decal_spray` entity, we read its origin, target, spray radius, and count. We calculate a base vector towards the target, and then generate `N` jittered rays in a conical or cylindrical pattern.
3. **Geometry Intersection**:
   We cast each ray against the map's structural geometry. Since Embree (the primary raytracer) is not initialized during the BSP phase, we must write a simple, localized Ray-Triangle intersection function. 
   - *Optimization*: To avoid checking every triangle in the map, we first test the ray against the AABB (Axis-Aligned Bounding Box) of each `mapDrawSurf`. If the ray intersects the bounding box, we then test against the surface's triangles.
4. **Hit Detection & Normal Extraction**:
   Upon hitting a surface, we record the exact world coordinate (`hit_point`) and the interpolated surface normal at that spot (`hit_normal`).
5. **Virtual Projector Instantiation**:
   We dynamically construct a standard `decalProjector_t` bounding box.
   - We center it at the `hit_point`.
   - We orient the projector matrix so that its primary projection axis aligns perfectly with the inverse of the `hit_normal`. This completely eliminates distortion/smearing, as the decal is now "looking" directly at the surface it hit.
   - We append this new virtual projector to the global `decalProjectors` array.
6. **Standard Compilation**:
   Once all sprays have been converted into virtual projectors, we simply let the existing `MakeEntityDecals` function run completely unmodified! It will process our virtual projectors identically to hand-placed brushes, clipping the geometry, generating UVs, and welding the trisoups perfectly.

## 4. Required Implementation Work

1. **Ray-Triangle Math**:
   A lightweight Möller–Trumbore intersection algorithm (or similar) needs to be added to `mathlib.c` for rapid ray-triangle testing during the BSP phase.
2. **Entity Parsing**:
   Logic to parse the new `_decal_spray` entity, extracting keys like `spray_count`, `spray_radius`, `spray_jitter_type` (e.g., Gaussian vs Uniform).
3. **Projector Matrix Generation**:
   A function to build the 6 orthographic clipping planes and the 2D texture projection matrix (`texMat`) from a given `hit_point` and `hit_normal`. This requires picking an arbitrary "up" vector (like `0 0 1`) and doing cross-products to build an orthogonal basis, then constructing the 6 planes of the box.
4. **Integration**:
   Injecting `ProcessDecalSprays(e)` into `bsp.c` right before `MakeEntityDecals(e)`.

## 5. Conclusion
Generating virtual `_decal` projectors via raycasting is drastically easier and cleaner than trying to generate the surface geometry manually. It perfectly leverages the existing, highly robust `MakeEntityDecals` pipeline. The only real technical hurdle is ensuring the orientation math (building the 6 projector planes from a single hit normal) handles gimbal lock (e.g., when the hit normal is perfectly vertical) gracefully.

---

## 6. Specific Features of `misc_decal_spray`

Based on the requested characteristics, here is how the specific behaviors of `misc_decal_spray` would be implemented mathematically and structurally.

### A. Entity Definition & Properties
The entity will parse the following keys:
- **`origin` & `angles`**: Defines the source point and the vector direction of the spray.
- **`distance` (range)**: Max ray trace distance. If omitted or `0`, the ray will cast infinitely (bounded by the map size) until it hits a surface matching its `decalgroup`.
- **`radius`**: The spread or width of the spray cylinder/cone.
- **`width` & `height`**: The physical dimensions of the individual decal projectors it spawns.
- **`shader`**: The shader applied to the generated `decalTrisoup`.
- **`random_rotation`**: Boolean. If true, the spawned decal is randomly spun around its surface normal. If false, it maintains a consistent "Up" vector aligned with the world (or spray axis).
- **`seed`**: Integer. Used to initialize the random number generator.
- **`pattern`**: Integer/String. Selects the distribution algorithm.

### B. Stable Randomization (Determinism)
To ensure the map compiles exactly the same way every time, we cannot rely on unseeded random numbers. 
- **Implementation**: Before processing the entity's rays, we call `srand(seed)`. 
- **Failsafe**: If the mapper doesn't explicitly provide a `seed` key, we can automatically generate a stable, unique seed by hashing the entity's `origin` coordinates `(X * 73856093 ^ Y * 19349663 ^ Z * 83492791)`. This guarantees the pattern never changes between compiles, but two different sprays placed in the map won't look like identical clones.

### C. Distribution Patterns
Generating points in a 2D circle (the spray cross-section) before firing the ray down the spray vector requires different probability distributions:

1. **Center-Heavy (Gaussian/Normal Distribution)**:
   - We generate random polar coordinates. For the radius from the center, we use a Gaussian random function (like Box-Muller transform) clustered around `0`. The vast majority of rays will fire near the center of the spray radius, fading out smoothly towards the edges.

2. **Uniform Distribution**:
   - To achieve a truly even spread across the circle, we select a random angle `theta = rand() * 2PI` and a random radius `r = max_radius * sqrt(rand())`. The square root ensures points don't artificially cluster in the center, giving a perfectly uniform density across the destination surface.

3. **Edge-Heavy (Destiny Surface Bounds)**:
   - *Challenge*: The user requested density near the edges of the "whole group of surfaces" (the destination geometry), not just the edges of the spray radius.
   - *Implementation*: Before firing rays, we iterate through all `mapDrawSurfs` that match the entity's `decalgroup` and calculate their collective 3D Bounding Box (AABB). We find the geometric center of this AABB.
   - We use rejection sampling: we fire uniform rays, find the hit point, and calculate the distance from the hit point to the center of the surface group's AABB. We only "accept" the decal placement if a random roll passes a probability curve that heavily favors longer distances. This forces the decals to cluster at the physical outer perimeters of the targeted room/object!

### D. Decal Projection Instantiation
For each accepted ray hit:
1. We capture the `hit_normal`.
2. **Forward Vector** = `-hit_normal`.
3. **Up Vector**: If `random_rotation`, pick a random orthogonal vector. If not, use the global Z-axis (or the spray's original Up vector) and orthogonalize it against the normal.
4. **Right Vector** = `CrossProduct(Forward, Up)`.
5. We use the `width` and `height` parameters to scale the Up and Right vectors, instantly giving us the 4 side planes of the projector box. We set the front/back planes a small distance (`depth`) from the hit point.
6. The virtual projector is added to the queue, and `MakeEntityDecals` seamlessly takes over.
