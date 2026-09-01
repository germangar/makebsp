# Extended BSP Format Capabilities

This document outlines extended features, metadata conventions, and data packing formats introduced to evolve the BSP format beyond legacy Quake 3 constraints. It is intended as a technical reference for engine developers and renderer modders implementing extended support.

---

## 1. Lightmap Bit-Depth (`_lightmapbits`)

The compiler supports exporting the lightmap lump (`LUMP_LIGHTMAPS`) in standard 8-bit integer, 16-bit half-precision float, or 32-bit single-precision float formats.

### Identification & Worldspawn Keys

Engines should query the `worldspawn` (entity 0) epair keys upon loading the BSP:

| Key | Values | Default | Description |
| :--- | :--- | :--- | :--- |
| `_lightmapbits` | `"8"`, `"16"`, `"32"` | `"8"` | Specifies the lightmap data depth per channel in bits. |
| `_lightmapImageSize`| e.g. `"128"`, `"512"`, `"1024"` | `"128"` | Dimensions ($N \times N$) of each square lightmap page/atlas. |
| `_lightingIntensity`| float string (e.g. `"1.0"`, `"2.5"`) | `"1.0"` | Peak intensity scaling factor applied during compression. |

---

### Data Encoding & Memory Layout

Lightmap data in `LUMP_LIGHTMAPS` is stored sequentially as square texture pages of dimensions $N \times N$, with 3 color channels (RGB) per texel:

$$\text{Page Size (Bytes)} = N \times N \times 3 \times (\text{Bytes per Channel})$$

#### Mode 8: Standard 8-bit RGB (`_lightmapbits` = `"8"`)
- **Format**: Unsigned 8-bit integer (`uint8_t` / `byte`) per channel (3 bytes/texel).
- **Value Range**: `[0, 255]`.
- **Scaling / Compression**:
  - Light intensities have been normalized/scaled down to fit into the `[0, 255]` range:
    $$\text{EncodedByte} = \text{clamp}\left(\text{Irradiance} \times \frac{255}{\text{maxIntensity}}, 0, 255\right)$$
  - To reconstruct the original physical HDR light level, the engine multiplies the sampled color by `_lightingIntensity`:
    $$\text{LinearHDRColor} = \left(\frac{\text{SampledByte}}{255.0}\right) \times \text{\_lightingIntensity}$$

#### Mode 16: 16-bit Half-Float RGB (`_lightmapbits` = `"16"`)
- **Format**: IEEE 754 half-precision float (`float16` / `uint16_t` bit-pattern) per channel (6 bytes/texel).
- **Value Range**: Floating-point physical irradiance values.
- **Scaling**: **Unscaled (Raw)**. Lightmaps are written with raw floating-point values and are **not** multiplied/divided by `_lightingIntensity`.
- **Engine Handling**: Upload directly to a GPU texture format such as `DXGI_FORMAT_R16G16B16A16_FLOAT` or `GL_RGB16F` / `GL_RGBA16F`. Sample directly without applying `_lightingIntensity`.

#### Mode 32: 32-bit Single-Float RGB (`_lightmapbits` = `"32"`)
- **Format**: IEEE 754 single-precision float (`float32` / standard C `float`) per channel (12 bytes/texel).
- **Value Range**: Floating-point physical irradiance values.
- **Scaling**: **Unscaled (Raw)**.
- **Engine Handling**: Upload directly to `DXGI_FORMAT_R32G32B32_FLOAT` / `GL_RGB32F` (or RGBA padded). Sample directly without applying `_lightingIntensity`.

---

### External Image Export (`-exportlightmaps` / `externalLightmaps`)

When lightmaps are exported as external image files (to disk in the map output directory):
- **8-bit Mode**: Saved as 8-bit PNG images (`lm_%04d.png`) via `stbi_write_png`.
- **16-bit & 32-bit HDR Modes**: Saved as Radiance HDR floating-point images (`lm_%04d.hdr`) via `stbi_write_hdr`, preserving physical unscaled irradiance.
- Stale files from previous compiles (`.png` and `.hdr`) are automatically cleaned up.

> [!WARNING]
> **16-bit External Export Notice**: While 16-bit mode is stored natively as half-precision floats (`float16`) inside the `.bsp` lump, external image export writes them out as 32-bit Radiance `.hdr` files. If your workflow relies on external image files rather than internal BSP lumps, using 16-bit mode offers no disk storage savings over 32-bit mode for the exported image files.

---

### Deluxe Mapping (Directional Lightmaps)

When deluxemapping is enabled:
- Deluxe pages are interleaved with base lightmaps in the lump:
  - **Even index ($2k$)**: Base color lightmap.
  - **Odd index ($2k + 1$)**: Deluxe directional vector map.
- Deluxe texels encode a normalized 3D direction vector mapped from $[-1, +1]$ to $[0, 255]$:
  $$\text{VectorComponent} = (\text{dir} \times 127.5) + 127.5$$
- For **16-bit** and **32-bit** modes, these directional values are encoded as `float16` or `float32` respectively to maintain uniform lump alignment and stride.

---

## 2. Lightgrid Data Format & Intensity Scaling

The lightgrid structure in `LUMP_LIGHTGRID` **always remains 8 bits per color channel** (`uint8_t` / `byte`), regardless of the `_lightmapbits` setting chosen for surface lightmaps. It uses the standard binary struct:

```c
typedef struct {
    byte    ambient[3];   // 8-bit RGB [0, 255]
    byte    directed[3];  // 8-bit RGB [0, 255]
    byte    latLong[2];   // Spherical direction angles [latitude, longitude]
} bspGridPoint_t;
```

### Depth & Bit-Count
- **Data Depth**: **Strictly 8 bits per color channel** (3 bytes for ambient, 3 bytes for directed).
- **Direction Depth**: 2 bytes total (`latLong[2]`), compressed via `NormalToLatLong`.
- **Struct Size**: Fixed at 8 bytes per sample point (or 22 bytes per multi-style sample group, depending on BSP version format).

---

### Mandatory `_lightingIntensity` Scaling

Because the lightgrid is always stored as 8-bit integer bytes, **it is always normalized/compressed during compilation using `_lightingIntensity`**.

1. **Compression (Compiler-Side)**:
   - Floating-point radiance values for both `ambient` and `directed` colors are scaled down into the `[0, 255]` byte range:
     $$\text{ambient}[c] = \text{clamp}\left(\frac{\text{AmbientFloat}[c]}{\text{\_lightingIntensity}}, 0.0, 255.0\right)$$
     $$\text{directed}[c] = \text{clamp}\left(\frac{\text{DirectedFloat}[c]}{\text{\_lightingIntensity}}, 0.0, 255.0\right)$$

2. **Reconstruction (Engine / Shader-Side)**:
   - The engine **must always** multiply sampled lightgrid color components by `_lightingIntensity` to reconstruct the true linear physical radiance:
     $$\text{LinearAmbient}[c] = \left(\frac{\text{ambient}[c]}{255.0}\right) \times \text{\_lightingIntensity}$$
     $$\text{LinearDirected}[c] = \left(\frac{\text{directed}[c]}{255.0}\right) \times \text{\_lightingIntensity}$$

3. **Behavior across `_lightmapbits` Modes**:
   - **Mode 8 (`_lightmapbits "8"`)**: Both lightmaps and lightgrid are 8-bit and share the `_lightingIntensity` scalar for expansion.
   - **Modes 16 & 32 (`_lightmapbits "16" | "32"`)**: Lightmaps are stored as raw, unscaled floating-point data (`float16` or `float32`). However, `_lightingIntensity` **is still injected into worldspawn** specifically so the engine can reconstruct the 8-bit lightgrid up to the exact same physical scale as the unscaled 16/32-bit lightmaps.

