# Extended BSP Format Capabilities

This document outlines extended features, metadata conventions, and data packing formats introduced to evolve the BSP format beyond legacy Quake 3 constraints. It is intended as a technical reference for engine developers and renderer modders implementing extended support.

---

## 1. Lightmap Bit-Depth (`-hdr` in `_makebsp_profile`)

The compiler supports exporting the lightmap lump (`LUMP_LIGHTMAPS`) in standard 8-bit integer, 16-bit half-precision float, or 32-bit single-precision float formats.

### Identification & Worldspawn Keys

Engines should query the `worldspawn` (entity 0) epair keys upon loading the BSP:

| Key | Values | Default | Description |
| :--- | :--- | :--- | :--- |
| `_makebsp_profile` | see below | — | Single-line compilation configuration: `-<field> <value>` tokens (booleans as `1`/`0`, floats with 2 decimals). The `-hdr` field selects the lightmap format. |
| `_q3map2_cmdline` | e.g. `"-samplesize 4 -lightmapsize 512 -deluxe"` | — | Legacy-compatibility provenance: a q3map2-style command line (flat switch list) simulating the compilation configuration, for engines that read the q3map2 setup. |
| `_lightmapImageSize`| e.g. `"128"`, `"512"`, `"1024"` | `"128"` | Dimensions ($N \times N$) of each square lightmap page/atlas. |
| `_lightingIntensity`| float string (e.g. `"1.0"`, `"2.5"`) | `"1.0"` | Peak intensity scaling factor applied during compression. |

The `_makebsp_profile` value is one line of space-separated `-<field> <value>` pairs mirroring the JSON game profile field names:

```
-lightmapSize 2048 -externalLightmaps 0 -keepLights 1 -sampleSize 4
-hdr rgb8 -hdr8BitScale 3.00 -lightmapsRGB 1 -lightgridRGB 0
-texturesRGB 1 -colorsRGB 1 -shading halflambert -sunShading lambert
-attenuation standard -cutoff 1.00 -fadeout 0.05 -deluxeMap 1
```

- `-hdr` selects the lightmap storage format: `rgb8`, `rgba16f`, `rgba32f`, or `off`.
- `-hdr8BitScale` is the source value `_lightingIntensity` was derived from (identical when the lighting stage is run).
- The RGB flags describe the compiled data's colorspace convention (`1` = sRGB data), and `-deluxeMap` reports whether deluxemaps are interleaved in the lightmap lump.
- The rest of the fields (`shading`, `sunShading`, `attenuation`, `cutoff`, `fadeout`, ...) are informational provenance of the lighting configuration.
- The key is written by the BSP stage with the fully resolved configuration and refreshed by the lighting stage at the end of its pass, so it always reflects what the final BSP was actually built with. Engines should ignore unknown fields for forward compatibility.

---

### Data Encoding & Memory Layout

Lightmap data in `LUMP_LIGHTMAPS` is stored sequentially as square texture pages of dimensions $N \times N$, with 3 color channels (RGB) per texel:

$$\text{Page Size (Bytes)} = N \times N \times 3 \times (\text{Bytes per Channel})$$

#### Mode 8: Standard 8-bit RGB (`-hdr rgb8`)
- **Format**: Unsigned 8-bit integer (`uint8_t` / `byte`) per channel (3 bytes/texel).
- **Value Range**: `[0, 255]`.
- **Scaling / Compression**:
  - Light intensities have been normalized/scaled down to fit into the `[0, 255]` range:
    $$\text{EncodedByte} = \text{clamp}\left(\text{Irradiance} \times \frac{255}{\text{maxIntensity}}, 0, 255\right)$$
  - To reconstruct the original physical HDR light level, the engine multiplies the sampled color by `_lightingIntensity`:
    $$\text{LinearHDRColor} = \left(\frac{\text{SampledByte}}{255.0}\right) \times \text{\_lightingIntensity}$$

#### Mode 16: 16-bit Half-Float RGB (`-hdr rgba16f`)
- **Format**: IEEE 754 half-precision float (`float16` / `uint16_t` bit-pattern) per channel (6 bytes/texel).
- **Value Range**: Floating-point physical irradiance values.
- **Scaling**: **Unscaled (Raw)**. Lightmaps are written with raw floating-point values and are **not** multiplied/divided by `_lightingIntensity`.
- **Engine Handling**: Upload directly to a GPU texture format such as `DXGI_FORMAT_R16G16B16A16_FLOAT` or `GL_RGB16F` / `GL_RGBA16F`. Sample directly without applying `_lightingIntensity`.

#### Mode 32: 32-bit Single-Float RGB (`-hdr rgba32f`)
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

The lightgrid structure in `LUMP_LIGHTGRID` **always remains 8 bits per color channel** (`uint8_t` / `byte`), regardless of the `-hdr` lightmap format setting chosen for surface lightmaps. It uses the standard binary struct:

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

3. **Behavior across `-hdr` Modes**:
   - **Mode 8 (`-hdr rgb8`)**: Both lightmaps and lightgrid are 8-bit and share the `_lightingIntensity` scalar for expansion.
   - **Modes 16 & 32 (`-hdr rgba16f | rgba32f`)**: Lightmaps are stored as raw, unscaled floating-point data (`float16` or `float32`). However, `_lightingIntensity` **is still injected into worldspawn** specifically so the engine can reconstruct the 8-bit lightgrid up to the exact same physical scale as the unscaled 16/32-bit lightmaps.

---

## 3. Lightgrid Invalid Cell Marking (`_lightgridFlagInvalid`)

The two-lump lightgrid storage (palette in `LUMP_LIGHTGRID` + dense 16-bit index map in `LUMP_LIGHTARRAY`, as written for FBSP-flavoured output) allows lattice positions that hold no real light — cells buried inside solid geometry — to be **explicitly marked as invalid** instead of storing a black cell. This lets engines exclude wall-embedded cells from the trilinear light blend, which otherwise drags sampled light toward black when an entity stands near a wall or the floor.

### Identification

The authoritative gate is the **BSP version field in the file header**: the sentinel convention is structural to **FBSP version 2** — every version 2 BSP carries it, and no version 1 BSP ever does.

- **FBSP version 2**: the `LIGHTARRAY` always uses the `0xFFFF` sentinel for buried cells, and the `_lightgridFlagInvalid` worldspawn key (value `"1"`, compiler-injected, **not** a mapper key) is written informationally for loaders that prefer checking entities over the header. Either check is sufficient.
- **FBSP version 1**: never carries the sentinel; re-writes of older experimental files that contained it strip the key. The supported alternative for version 1 is lightgrid dilation (below).
- **IBSP (Q3-style)**: unaffected in every way — dense grid, no index map, no marker, no key.

### The Sentinel Contract

- **`0xFFFF` is reserved**: a `LUMP_LIGHTARRAY` entry of `0xFFFF` means *"no cell here — this lattice position is invalid"* (unreachable / inside solid geometry). It is not a palette index.
- **The map stays dense**: every lattice position of the map bounding box always has exactly one `LIGHTARRAY` entry. Invalid positions are marked, never omitted.
- Valid entries therefore use palette indices `0..65534`, capping the palette at 65535 unique cells (the compiler fails with a clean error instead of overflowing).
- Cells whose lattice point merely touches geometry but were successfully relocated by the compiler's nudge search keep their sampled light and are **not** marked — thin-wall regions do not lose valid light.

### Engine Implementation Guidance

- **Loader**: when `_lightgridFlagInvalid` is `"1"`, turn `0xFFFF` entries into NULL slots instead of dereferencing the palette. Without the key, no `0xFFFF` entry ever exists in the file.
- **Sampler**: skip NULL corners of the trilinear blend and renormalize the weights over the remaining valid corners.
- **Fallback**: when all surrounding grid sites are invalid, the engine reports the same result as a map with no light grid (average directional vector, zero world ambient/diffuse).

> [!WARNING]
> **Compatibility**: the version gate is the protection. An engine that only knows FBSP version 1 rejects a version 2 file cleanly at load time (version mismatch) and never reaches the palette dereference. An engine with version 2 support loading a version 1 BSP simply never sees a sentinel. Engines must never treat `0xFFFF` as a palette index.

### Legacy-Compatibility Alternative: Lightgrid Dilation (`lightgridDilation`)

For engines that cannot be updated to the sentinel convention, the compiler can instead **fill** fully buried cells with the average light of their valid neighbors (a single-layer dilation over the 26-cell neighborhood), so legacy trilinear sampling near walls blends meaningful light instead of black.

- **Identification**: none. The output is a plain legacy grid — no worldspawn key, no marker, no engine support required. Enabled per game profile (`"lightgridDilation": true`); it is a tool-side decision.
- **Semantics**: runs last, after all other lightgrid filters, copying final `ambient`/`directed` values and an averaged direction from the valid neighbors. Dilated cells are never used as donors (single fill layer). Cells with no valid neighbor (deep interior) stay black — they are never sampled by a legitimate probe anyway.
- **Version interplay**: dilation is the version 1 mechanism. The FBSP version 2 writer skips the pass entirely, since the sentinel owns those cells and the filled data would never be written. Version 1 profiles enable it via `"lightgridDilation": true` in the game profile.
- **Trade-off**: dilation fabricates data, so borrowed light can bleed across thin walls at grid resolution (a buried cell shared between a bright and a dark room brightens the dark side's probe). The sentinel has no such artifact — it discards corners and renormalizes over real samples — and remains the exact mechanism for engines that support it.

