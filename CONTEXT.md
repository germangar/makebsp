# Q3Map & Light - Quake 3 BSP Compilers

This is a **modified compiler suite** based on the official map-to-BSP compiler from Quake III Arena (GPL licensed). It is used to compile `.map` files (text-based level geometry) into `.bsp` files (binary format used by the Quake 3 engine).

## Architecture & Split

The compiler has been split into multiple executables to separate concerns and facilitate the integration of modern acceleration structures.

### 1. `q3map/` (BSP & VIS Generation)
Produces **`q3map.exe`**. Handles parsing `.map` files, generating the BSP tree, performing CSG boolean operations, calculating the Potentially Visible Set (VIS), and generating automatic collision meshes for 3D models. It links against heavy libraries like Assimp, HACD, CoACD, and MeshLib-Lite.

| Core Files | Purpose |
|------|---------|
| `bsp.c`, `tree.c` | BSP generation and main entry dispatcher (`-bsp`, `-vis`, `-info`). |
| `map.c`, `brush.c` | Map parsing and brush geometry creation. |
| `csg_brush.c` | CSG operations - brush boolean operations (subtract, merge). |
| `vis.c`, `visflow.c` | Visibility - potentially-visible-set (PVS) calculation. |
| `surface.c`, `patch.c`| Surface generation - creates map draw surfaces. |
| `lightmaps.c` | **Lightmap Layout:** Allocates Atlas space and UVs (Does *not* render light). |
| `model_*.c` | Automatic collision hull generation (CoACD, HACD, Extrusion). |

### 2. `light_embree/` (Accelerated Lighting)
Produces **`light.exe`**. This is the primary lighting compiler. It is a modernized version of the original lighting code, linked with the **Intel Embree 4** library for high-performance raytracing and shadow generation.

| Core Files | Purpose |
|------|---------|
| `main.c` | Entry dispatcher (`-light`, `-vlight`). |
| `light.c` | Primary lightmap rendering, shadow raytracing, and radiosity. |
| `lightv.c` | Fast vertex-lighting and dynamic grid lighting. |
| `light_trace.c` | **Embree-accelerated** collision detection and occlusion testing. |

### 3. `light/` (Standard Lighting)
Produces **`q3light.exe`**. This is the original, unmodified lighting compiler code. it uses standard Q3 BSP-based tracing logic and serves as a reference or fallback.

| Core Files | Purpose |
|------|---------|
| `main.c` | Entry dispatcher (`-light`, `-vlight`). |
| `light.c` | Standard lightmap rendering. |
| `lightv.c` | Standard vertex and grid lighting. |
| `light_trace.c` | Standard Q3 BSP collision and occlusion testing. |

### 4. `shared/` (Common Utilities)
Contains code required by both the BSP generation and Lighting phases.

| Core Files | Purpose |
|------|---------|
| `globals.c/h` | Shared runtime variables (e.g., `samplesize`, `source`). |
| `mesh.c/h` | Math for subdividing curved Bezier patches into triangles. |
| `shaders.c/h` | Parsing `.shader` files for surface properties (translucency, light emission). |

## Key Enhancements (Fork-Specific)

### Automatic Collision Generation
This fork adds **automatic collision mesh generation** for `misc_model` entities during the BSP phase:
- **Assimp** is used to load complex 3D models (.obj, .ase, etc.).
- **MeshLib-Lite** cleans and heals model topologies.
- **HACD / CoACD** generates optimized convex collision hulls which are automatically converted into invisible structural BSP brushes.

### Intel Embree Acceleration
The `light.exe` executable is linked with **Intel Embree 4** to replace legacy BSP-based raytracing with high-performance BVH traversal. This significantly improves shadow calculation times, especially on complex maps with high-polygon models.

## Output

- `q3map.exe` - BSP and VIS compiler.
- `light.exe` - **Embree-accelerated** lighting compiler.
- `q3light.exe` - Original Q3 lighting compiler (reference).

## Libraries & Binaries

- **Embree 4.4.0**: Prebuilt binaries for Windows and Linux are stored in `libs/embree/prebuilt/`.
- **Runtime Dependencies**: `embree4.dll` and `tbb12.dll` are required in the executable directory for `light.exe`.

## License

GPL v2 (same as Quake III Arena source code)
