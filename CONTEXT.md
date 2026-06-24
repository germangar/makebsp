# AI Architecture Context: Modernized BSP Toolchain

> [!IMPORTANT]
> **Purpose of this Document**: This is NOT a log, a status report, or a change history. It is a high-level technical summary of the project's architecture, characteristics, and non-obvious logic. It is designed to provide AI coding assistants with immediate, high-fidelity context about the codebase before starting work.

## 1. Project Overview
This project is a heavily modernized fork of the Quake III Arena BSP toolchain (`makebsp.exe` and `makelight.exe`) which primary target is the FBSP format used by the QFusion engine. The primary goal is to achieve high-fidelity, cinema-grade lighting for legacy engines (QFusion, etc.) by replacing 1990s integer-based arithmetic with modern 32-bit floating-point ray tracing and advanced geometry libraries.

## 2. Core Architecture: High-Precision Lighting
The most significant architectural change is the transition from the legacy 8-bit integer lighting pipeline to a full **32-bit Floating Point Pipeline**.

- **Internal Buffers**: All light data is processed using float32. High-precision replicas of BSP structures are maintained in:
    - `internalDrawVerts`: 32-bit color per vertex.
    - `lightFloats`: High-precision RGB lightmap data.
    - `gridData32`: High-precision ambient/directed light grid.
- **Up/Down Conversion**: A synchronization layer (`UpConvertLightingData` and `DownConvertLightingData` in `common/lightdata.c`) ensures binary compatibility with the final 8-bit BSP format while allowing infinite additive passes without precision loss.
- **Additive Multi-Pass**: The system supports stacking multiple light passes (Direct, Radiosity, Bounce) additively using `+=` arithmetic on the float buffers.

## 3. Intersection Testing: Intel Embree 4
Legacy BSP facet testing has been completely replaced with **Intel Embree 4.4.0**.
- **Scene Construction**: Map brushes, subdivided patches, and triangle models (misc_models) are all flattened into an Embree BVH.
- **Selective Shadowing**: `AlphaFilter` logic in `light/light_trace.c` allows complex geometry (TriSoups) to shadow themselves (Self-Shadowing) while preventing artifacts on planar map geometry using the `MST_PLANAR` check.
- **Ray Nudging**: A unified `tnear` (nudge) of `0.0001f` is used to prevent self-intersection artifacts.

## 4. Asset Discovery & Virtual File System (VFS)
The toolchain implements a Quake-standard VFS with modernized search and shadowing rules.
- **Executable-Relative Paths**: To ensure stability when launched from external editors (like NetRadiant), the tools locate their own binary directory via Windows API. Global assets (game JSON profiles and OpenCL kernels) are searched for in the `makebsp/` namespace folder sibling to the executables.
- **Shader-Level Shadowing**: Unlike legacy tools that shadow at the file level, `makebsp` parses every discovered `.shader` file across the VFS. Redundancy is handled at the individual shader block level: the first definition found (in the highest priority path) is preserved, while subsequent definitions of the same shader name are skipped.
- **Embedded Kernels**: For release builds, OpenCL `.cl` source files are stringified and baked directly into the binary, making the toolchain self-contained and portable.

## 5. Radiosity Pipeline (Global Illumination)
The project implements a custom three-phase radiosity system:
1.  **Phase 1 (Emit)**: Luxels from the direct pass spawn virtual emitters.
2.  **Phase 2 (Integrate)**: Analytical area form-factors are used to simulate light bounce.
3.  **Phase 4 (Merge)**: Bounced light is added back into the floating-point lightmap buffers.

### Core Concepts
- **Surface types**: There are 3 surface types which are treated diferently MST_PLANAR (always planar surfaces from brushes), MST_PATCH (bezier curve surfaces), MST_TRIANGLE_SOUP (triangle meshes from 3D models).
- **UV map coordinates**: MST_PLANAR UV coordinates are rectangles and adjust to texel centers. MST_PATCH coordinates fall on texel centers too. MST_TRIANGLE_SOUP coordinates adhere to texel edges as close as they can, since they are not rectangles.
- **Sparse Sampling**: To optimize performance, the system can sample emitters at a configurable interval (Sparse Grid) and then interpolate the results across the full lightmap.
- **Radiosity Ambient Blending**: If `rad_color_ratio` is less than 1.0 and ambient color is present, the system uses the ambient color as a replacement for the bounced color. This reduces color bleeding while preserving overall energy. Ambient is added after radiosity to prevent overblowing.
- **Singularity Guarding**: Implements distance clamping and fade-out gradients to prevent infinite energy accumulation ("Nuclear Glow") when emitters are too close to geometry.

## 5. Geometry Processing: xatlas, MeshLib & Convex Decompositions
The BSP compiler (`makebsp.exe`) leverages modern libraries for texture and collision:
- **xatlas Integration**: Handles automatic lightmap UV unwrapping and atlas packing for complex 3D models and subdivided geometry, ensuring unique mappings for all surfaces.
- **MeshLib**: Performs geometric healing, decimation, and cleanup of complex triangle soup models to prepare them for physical collision hulls.
- **CoACD & HACD**: Performs Approximate Convex Decomposition to convert meshes into optimized convex collision brushes. While CoACD handles general shape approximation, HACD is leveraged for `MC_WRAP` and `MC_OBJECT` profiles where tighter, non-voxelized wrapping is required.

## 7. Technical Stack
- **Language**: C (some C++ wrappers for libraries).
- **Parallelism**: OpenMP is used extensively for multi-core scaling. All light filtering is GPU-accelerated via OpenCL.
- **File Formats**: Primary support for **FBSP** (QFusion/Xonotic) and **IBSP** (Quake 3).

## 8. Developer Conventions
- **Nomenclature**: Prefix `rad_` for radiosity, `lm_` for lightmap post-processing.
- **Architecture**: Global settings are derived from `game_t` templates in `shared/globals.c`, while runtime overrides are handled via CLI switches in `main.c`.

## 9. Lightmap Post-Processing
The final stage of the lighting tool (`light/lm_postprocess.c`) applies image-space and world-space filters to the high-precision `lightFloats` buffer. The architecture provides three decoupled choices for the mapper: **Trace-time Supersampling**, **Post-process Anti-Aliasing**, and **Gaussian Smoothing**.

- **Geometric Adjacency**: For planar world surfaces, the system builds a world-space index of partners by hashing snapped vertex coordinates (128-unit precision) to detect shared physical edges. A universal `GetFilteredTexel` helper allows filters to cross these boundaries seamlessly.
- **Volumetric Filtering (VPPS)**: For complex `misc_models` (`MST_TRIANGLE_SOUP`), the tool uses a per-surface **3D Spatial Hash**. This bypasses fragmented UV islands by performing a 3D neighborhood search in world-space.
- **Dynamic Density Scaling**: The system automatically calculates the physical "World Units per Texel" ratio for every individual model. This ensures that the filter radius and 3D spatial hash scale physically correctly regardless of editor scaling or `_lightmapscale`.
- **Volumetric RGSS**: Trisoup Anti-Aliasing utilizes an 8-point Volumetric Super-Sampling pattern, querying the 3D spatial hash multiple times per pixel to match the crispness of the world floor's RGSS.
- **Mathematical Parity**: To unify the "feel" between surface types, Trisoup smoothing uses true 3D Gaussian weights and a radius "cheat factor" (1.25x) to compensate for the volume difference between spherical (3D) and square (2D) kernels.
- **Multi-threaded Performance**: The per-surface spatial hashes are lock-free and allocated on-the-fly, allowing for perfect parallel scaling and minimal memory usage.

## 10. Cross-Tool Metadata (Sidecar Pipeline)
Because the standard BSP format (dsurface_t) is binary-frozen and cannot be easily extended, the toolchain uses a **Binary Sidecar Pipeline** to transfer per-surface metadata between the compiler (makebsp.exe) and the lighting tool (makelight.exe).

- **Mechanism**: makebsp serializes per-surface overrides into a lightweight binary array (extraSurface_t) at maps/[mapname]/cache/[mapname].srf, which is then re-loaded by light during surface initialization.
- **Precedence**: Sidecar data acts as a selective override for global game_t defaults.

## 11. Unified Distance Attenuation
The lighting math utilizes a centralized `CalculateAttenuation` pipeline designed to prevent hotspots while maintaining aggressive culling:
- **Singularity Offsets**: The system utilizes a `prestep` (alias `rampoffset`) parameter to shift the inverse-square curve, ensuring $1 / d^2$ never explodes to infinity near the source. This is now mapper-configurable per-light entity, defaulting to `16.0`.
- **Decoupled Soft Fades**: The visual fade of a light is decoupled from its broad-phase culling. `MIN_LIGHT_ADD` acts as the strict, high-performance geometry cutoff. However, a soft fade is applied *before* this cutoff, allowing the light energy to smoothly slope toward zero visually, without bloating the broad-phase culling bounding spheres.
- **Early-Out Optimizations**: Spotlights perform expensive vector operations. The distance attenuation is explicitly calculated *first*; if distance alone culls the texel, the system bypasses the spotlight math entirely.

## 12. Entity Parsing & Nomenclature
The toolchain features a highly modernized entity parsing system for mappers:
- **Agnostic Keys**: Entity keys are entirely case-insensitive and completely ignore the legacy Quake `_` prefix (e.g., `_color` and `Color` are treated identically).
- **Nomenclature Shift**: The term `falloff` has been explicitly replaced with `shading` in the codebase and CLI to clarify that it refers to angle/surface shading, distinguishing it from distance-based "attenuation".
- **Dynamic Overrides**: `misc_model` and `func_*` entities support powerful per-entity overrides, such as `upscale`, `smooth`, `collisiontype` (for tweaking convex hull/trisoup generation), and `haloshader` for overriding automatically generated light halos.

## 13. Bezier Patch Mesh Lighting Pipeline
The toolchain implements a mathematically exact `q3map2`-style 2-pass tessellation and sampling architecture for curved Bezier patches to eliminate lighting seams and artifacts without relying on heuristic normal smoothing:
- **Two-Pass Surface Generation**: Curved patch normals are calculated via a double pass of `PutMeshOnCurve` (one constrained, one unconstrained wrapping) and blended using spherical interpolation to perfectly resolve boundary normals directly during BSP generation.
- **Accurate UV Measurement**: `AllocateLightmapForPatch` scales texture boundaries by summing the `maxLength` of individual edge segments instead of bounding dimensions.
- **Planar Patch Isolation**: Because `q3map2` mathematical corrections disrupt pixel-perfect atlas grid-snapping on perfectly flat geometry, the system globally forks logic based on `IsMeshPlanar()`. Planar patches fallback to legacy geometry subdivision (`SubdivideMeshQuads`) and purely linear UV distribution, guaranteeing zero-tolerance alignment with surrounding BSP structural brushes.

## 14. Robust Lightmap Sample Point Nudging (Centroid Nudge)
To completely prevent light leaking at boundaries and corners (such as sunlight bypassing thick walls), the lighting pipeline employs a dynamic "Centroid Nudge" for all sample points prior to ray tracing:
- **Surface Centroids**: The geometric centroid of every surface (or subdivided polygon) is calculated during the sampling phase.
- **Inward Bias**: Instead of strictly nudging the sample point out along the normal vector, which can push corner texels into the void outside the map, the origin is first nudged *inward* toward the 3D surface centroid.
- **Distance Limiting**: The centroid nudge distance is clamped to half the distance between the texel and the centroid (to prevent overshooting the center on small faces). After the centroid nudge, the standard `SAMPLE_NUDGE` is applied along the normal.
- **Out-of-Bounds Texels**: This eliminates the need for strict bounding box culling. Lightmap texels whose centers fall slightly outside the strict 2D geometry bounds are safely extrapolated and automatically pulled inside the volume by the centroid nudge, inherently solving the "black sawtooth" bilinear filtering artifact without discarding samples.
