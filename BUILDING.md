# Building `makebsp`

This repository is configured to build seamlessly on both Windows and Linux using `make`. Pre-built versions of most required third-party libraries (Embree 4, TBB, Assimp, MeshLib-Lite, etc.) are included directly in the repository to minimize friction.

## Windows Build Instructions

### Prerequisites
1. Install [MSYS2](https://www.msys2.org/).
2. Open the **MSYS2 MinGW64** terminal.
3. Install the GCC toolchain and `make` by running:
   ```bash
   pacman -S mingw-w64-x86_64-gcc make
   ```

> **Note:**
> All necessary static libraries (including OpenCL stubs) are already bundled in the repository. You do not need to install OpenCL, Embree, or TBB system-wide on Windows.

### Compiling
Navigate to the repository directory in your MinGW64 terminal and run:
```bash
make
```

For a **release build** (which automatically embeds the OpenCL kernel files into the binary), run:
```bash
make RELEASE=1
```

---

## Linux Build Instructions

### Prerequisites
1. Install the GNU compiler collection (`build-essential`) and `make`.
2. Install the OpenCL development headers. Unlike Windows, the Linux build dynamically links against your system's OpenCL driver, so the headers must be installed.

On Ubuntu/Debian, run:
```bash
sudo apt-get update
sudo apt-get install build-essential ocl-icd-opencl-dev
```

### Compiling
Navigate to the repository directory in your terminal and run:
```bash
make
```

For a **release build** (which automatically embeds the OpenCL kernel files into the binary), run:
```bash
make RELEASE=1
```

