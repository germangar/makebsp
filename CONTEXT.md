# Q3Map - Quake 3 BSP Compiler

This is a **modified Q3Map compiler** - the official map-to-BSP compiler from Quake III Arena (GPL licensed). It's used to compile `.map` files (text-based level geometry) into `.bsp` files (binary format used by the Quake 3 engine).

## Core Functionality

The compiler performs standard BSP compilation stages:

| File | Purpose |
|------|---------|
| `map.c` | Map parsing - reads .map files, parses entities, brushes, patches |
| `csg_brush.c` | CSG operations - brush boolean operations (subtract, merge) |
| `bsp.c`, `tree.c` | BSP generation - builds the BSP tree structure |
| `vis.c`, `visflow.c`, `portals.c` | Visibility - potentially-visible-set calculation |
| `light.c`, `lightmaps.c`, `lightv.c` | Lightmapping - calculates light maps |
| `surface.c`, `patch.c`, `facebsp.c` | Surface generation - creates draw surfaces |
| `misc_model.c`, `glfile.c` | Model loading - loads .ase/.obj models |

## Key Enhancements (Fork-Specific)

This fork adds **automatic collision mesh generation** for `misc_model` entities:

| File | Purpose |
|------|---------|
| `model_coacd.c` | Uses CoACD library for convex decomposition |
| `model_hacd.c` | Alternative convex decomposition via HACD library |
| `model_collision.c` | Converts collision hulls to brush geometry and exports to map files |
| `model_extrude.c` | Extrudes collision geometry |

## Libraries

- **stb_image** - Image loading (PNG, JPEG, BMP, TGA, GIF, HDR, and more)
- **assimp** - Model format loading (ASE, OBJ, etc.)
- **hacd/coacd** - Convex hull decomposition algorithms
- **MeshLib-Lite** - Mesh processing from MeshLab

## Output

- `q3map.exe` - The compiled Windows executable

## License

GPL v2 (same as Quake III Arena source code)
