# Q3Map & Light - Modernized Quake 3 BSP Compilers

This project is a high-performance compiler suite for Quake III Arena (GPL licensed) level geometry. It modernizes the original id Software source code by integrating modern ray-tracing and geometry processing libraries.

## Core Components

### q3map.exe
The primary map-to-BSP compiler. It processes `.map` files into `.bsp` files, handling:
- **CSG & Portals:** Traditional BSP tree construction.
- **Advanced Collision:** Integrates **CoACD** (Approximate Convex Decomposition) and **MeshLib-Lite** for generating high-fidelity collision brushes from complex `.obj` map models.
- **Model Support:** Converts `misc_model` entities into `MST_TRIANGLE_SOUP` surfaces within the BSP.

### light.exe (light_embree)
The modernized lighting back-end. Unlike the legacy compiler, this version utilizes **Intel Embree 4.4.0** for hardware-accelerated ray tracing.
- **Acceleration:** Uses a Bounding Volume Hierarchy (BVH) to replace legacy BSP-based facet testing.
- **Geometry Types:** Supports planar faces, subdivided patches (curves), and triangle soups (models) within the Embree scene.
- **Self-Shadowing:** Implements selective surface filtering to allow complex models to shadow themselves while preventing "acne" artifacts on flat map geometry.
- **Decay Models:** Supports multiple falloff models (Lambert, Half-Lambert, Quadratic) with a tunable intensity cutoff (`MIN_LIGHT_ADD`).

## Key Technologies
- **Intel Embree 4.4.0:** High-performance ray-tracing kernels for CPU.
- **CoACD:** Used for convex decomposition of complex geometry into collision brushes.
- **MeshLib-Lite:** Provides geometric operations for model-to-brush conversion.
- **OpenMP:** Used for multi-threaded lighting and geometry processing.

## Project Structure
- `q3map/`: Source code for the BSP compiler.
- `light_embree/`: Source code for the modernized, Embree-integrated lighting compiler.
- `common/`: Shared Q3 BSP data structures and math libraries.
- `libs/`: Third-party integrations (Embree, CoACD, Assimp, MeshLib).
- `shared/`: Global configuration, shader parsing, and shared mesh logic.

## Runtime Dependencies (Windows)
- `embree4.dll` & `tbb12.dll`: Required in the executable directory for `light.exe`.
- `assimp-vc143-mt.dll`: Required for model importing.
