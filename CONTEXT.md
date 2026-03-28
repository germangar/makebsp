# Q3Map & Light - Modernized Quake 3 BSP Compilers

This project is a high-performance compiler suite for Quake III Arena (GPL licensed) level geometry. It modernizes the original id Software source code by integrating modern ray-tracing, geometry processing libraries, and a high-precision lighting pipeline.

## Core Components

### q3map.exe
The primary map-to-BSP compiler. It processes `.map` files into `.bsp` files, handling:
- **CSG & Portals:** Traditional BSP tree construction and visibility calculation.
- **Advanced Collision:** Integrates **CoACD** (Approximate Convex Decomposition) and **MeshLib-Lite** for generating high-fidelity collision brushes from complex `.obj` map models.
- **Model Support:** Converts `misc_model` entities into `MST_TRIANGLE_SOUP` surfaces within the BSP.

### light.exe (Modernized Lighting)
Directly integrates **Intel Embree 4.4.0** for hardware-accelerated ray tracing and features a full 32-bit floating-point internal pipeline.
- **32-bit Float Pipeline:** All internal calculations (vertex lighting, lightmaps, grid data) use high-precision floats via `internalDrawVerts`, `lightFloats`, and `gridData32`.
- **Binary Compatibility:** Uses a synchronization layer (`UpConvertLightingData` / `DownConvertLightingData`) to maintain compatibility with legacy 8-bit IBSP/FBSP formats.
- **Acceleration:** Uses Embree's Bounding Volume Hierarchy (BVH) for high-speed intersection testing, replacing legacy BSP facet testing.
- **Self-Shadowing:** Implements selective surface filtering in `AlphaFilter` to allow non-planar surfaces (trisoups, patches) to shadow themselves while preventing artifacts on planar map geometry.
- **Additives Passes:** The pipeline is designed for fully additive multi-pass lighting (e.g., direct + radiosity passes) without precision loss.

## Key Technologies
- **Intel Embree 4.4.0:** High-performance ray-tracing kernels for CPU.
- **xatlas:** Integrated for efficient UV packing during model lightmap generation.
- **CoACD:** Convex decomposition of complex geometry into collision brushes.
- **MeshLib-Lite:** Geometric operations and decimation for model-to-brush conversion.
- **OpenMP:** Multi-threaded execution for lighting and geometry processing.

## Project Structure
- `q3map/`: Source code for the BSP compiler.
- `light/`: Source code for the modernized, Embree-integrated lighting compiler.
- `common/`: Shared Q3 BSP data structures, math libraries, and 32-bit internal lighting buffers (`lightdata.c/h`).
- `libs/`: Third-party integrations (Embree, CoACD, Assimp, MeshLib, xatlas).
- `shared/`: Global configuration, shader parsing, and shared mesh logic.

## Runtime Dependencies (Windows)
- `embree4.dll` & `tbb12.dll`: Required for hardware-accelerated tracing.
- `assimp-vc143-mt.dll`: Required for model importing via `assimp`.
- `tbbmalloc.dll`: Optional optimization for high-thread-count environments.
