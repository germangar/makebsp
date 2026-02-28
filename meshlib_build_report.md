# MeshLib DLL Build Technical Report

This report documents the final configuration and implementation details for the successfully built MeshLib DLL with C bindings on Windows 10 using MinGW/GCC 10.3.0.

## Project Structure
- **Root**: `libs/MeshLib`
- **Build Directory**: `libs/MeshLib/build`
- **Third-party**: `libs/MeshLib/thirdparty`

## Dependencies Management
To bypass environment limitations and avoid external package managers, the following dependencies were manually integrated as header-only libraries in `libs/MeshLib/thirdparty`:
- **Boost** (headers only)
- **Eigen**
- **fmt** (8.x)
- **spdlog** (1.9.x)
- **tl-expected**
- **parallel-hashmap**
- **JsonCpp**: headers in `thirdparty/JsonCpp/json` and source files (`json_reader.cpp`, `json_value.cpp`, `json_writer.cpp`) added directly to the `MRMesh` target.

### manual_deps.cmake
A custom `manual_deps.cmake` was created and included in `MRMesh/CMakeLists.txt`. It:
- Defines include paths for the above libraries.
- Provides dummy targets for `libzip`, `ZLIB`, and `MbedTLS` to resolve CMake `find_package` dependencies that were otherwise blocked.

## Shims and Stubs
Several shims were implemented to resolve environment-specific compilation issues:
- **TBB Shim**: A header-only replacement for Threading Building Blocks, providing stubs for `task_arena`, `task_group`, `parallel_for`, `parallel_reduce`, etc. Located in `libs/MeshLib/thirdparty/tbb`.
- **`std::bit_cast` Shim**: Located in `libs/MeshLib/thirdparty/bit_cast_shim.h`. It provides a fallback for the `std::bit_cast` function which was missing in the available MinGW GCC version. It is forcibly included in `MRMesh` via `-include`.
- **MRZip Stub**: `libs/MeshLib/source/MRMesh/MRZipStub.cpp` provides empty implementations of MeshLib's zip compression/decompression functions, allowing the library to link without `libzip`.

## CMake Configuration & Flags
The project was configured with the following key options:
- `BUILD_SHARED_LIBS=ON`
- `MESHLIB_EXPERIMENTAL_BUILD_C_BINDING=ON`
- `MESHLIB_USE_VCPKG=OFF`
- `MESHLIB_BUILD_MRVIEWER=OFF`
- `MESHLIB_BUILD_PYTHON_MODULES=OFF`
- `MESHLIB_BUILD_VOXELS=OFF`
- `MESHLIB_BUILD_EXTRA_IO_FORMATS=OFF`

### Global Definitions
Added to `MRMesh/CMakeLists.txt`:
- `-DUNICODE -D_UNICODE` (Enables Wide character API)
- `-D_WIN32_WINNT=0x0601` (Targets Windows 7+)

### Linker Fixes
- Disabled `-Wl,-z,defs` in `cmake/Modules/CompilerOptions.cmake` as it is not supported by the MinGW linker.

## Specific Source Fixes
- **MRICP.h**: Removed `MRMESH_API` from an inline constructor of the `ICP` class to resolve a `dllimport` alignment/linkage error during `MRMeshC` compilation.
- **MRMeshC/CMakeLists.txt**: Updated to conditionally link `MRVoxels` and `MRIOExtras` only if they are enabled, and excluded voxel-related headers/sources when building without `MRVoxels`.

## Final Output (DLLs)
Located in `libs/MeshLib/build/bin`:
- `libMRMesh.dll` / `libMRMesh.dll.a` (Core C++)
- `libMRMeshC.dll` / `libMRMeshC.dll.a` (C Bindings)

The `libMRMeshC.dll.a` is the primary import library for use in C projects.
