# Q3Map & Light - Quake 3 BSP Compilers

This is a **modified compiler suite** based on the official map-to-BSP compiler from Quake III Arena (GPL licensed). It is used to compile `.map` files (text-based level geometry) into `.bsp` files (binary format used by the Quake 3 engine).

## Architecture & Split

The compiler has been split into two distinct executables to improve build times and separate concerns. `q3map.exe` handles the heavy geometry and collision processing, while `light.exe` focuses purely on high-performance raytracing and shadow generation without dragging in heavy 3D geometry libraries.

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

### 2. `light/` (Lighting Generation)
Produces **`light.exe`**. Loads a compiled `.bsp` file and performs raytracing to fill the lightmap data allocated during the BSP phase. It is a lean, fast executable that only depends on standard math and raytracing logic.

| Core Files | Purpose |
|------|---------|
| `main.c` | Entry dispatcher (`-light`, `-vlight`, `-vsound`). |
| `light.c` | Primary lightmap rendering, shadow raytracing, and radiosity. |
| `lightv.c` | Fast vertex-lighting and dynamic grid lighting. |
| `light_trace.c` | Collision detection and occlusion testing for light rays. |

### 3. `shared/` (Common Utilities)
Contains code required by both the BSP generation and Lighting phases.

| Core Files | Purpose |
|------|---------|
| `globals.c/h` | Shared runtime variables (e.g., `samplesize`, `source`). |
| `mesh.c/h` | Math for subdividing curved Bezier patches into triangles. |
| `shaders.c/h` | Parsing `.shader` files for surface properties (translucency, light emission). |

## Key Enhancements (Fork-Specific)

This fork adds **automatic collision mesh generation** for `misc_model` entities during the BSP phase:
- **Assimp** is used to load complex 3D models (.obj, .ase, etc.).
- **MeshLib-Lite** cleans and heals model topologies.
- **HACD / CoACD** generates optimized convex collision hulls which are automatically converted into invisible structural BSP brushes.

## Output

- `q3map.exe` - BSP and VIS compiler.
- `light.exe` - Lighting compiler.

## License

GPL v2 (same as Quake III Arena source code)
