# Makebsp

Makebsp is a high-performance idTech 3 BSP compiler modernization based on the original id Software `q3map` source code.

## Table of Contents
- [💡 Key Features](#-key-features)
    - [1. High-Performance Ray Tracing (Intel Embree)](#1-high-performance-ray-tracing-intel-embree)
    - [2. 32-bit Floating Point Pipeline](#2-32-bit-floating-point-pipeline)
    - [3. Improved Model Support (misc_model)](#3-improved-model-support-misc_model)
    - [4. Automatic Edge Chamfering](#4-automatic-edge-chamfering)
    - [5. Deluxe Mapping V2](#5-deluxe-mapping-v2)
    - [6. Macro Ambient Occlusion (MAO)](#6-macro-ambient-occlusion-mao)
    - [7. Brush-to-Light Generation (func_light)](#7-brush-to-light-generation-func_light)
    - [8. In-Editor Configurable Surface Colors](#8-in-editor-configurable-surface-colors)
    - [9. Modernized Color & Lighting Models](#9-modernized-color--lighting-models)
    - [10. High-Quality 2D Filtering](#10-high-quality-2d-filtering)
    - [11. Customizable Game Profiles (JSON)](#11-customizable-game-profiles-json)
    - [12. Automatic Light Halos](#12-automatic-light-halos)
- [🛠 Recommended Workflow](#-recommended-workflow)
- [⚙️ Game Profiles (makebspdata)](#-game-profiles-makebspdata)
- [🎨 Shader Modifications](#-shader-modifications)
- [📦 Entity Keys Reference](#-entity-keys-reference)
    - [worldspawn](#entity-worldspawn)
    - [misc_model](#entity-misc_model)
    - [func_group](#entity-func_group)
    - [func_trisoup](#entity-func_trisoup)
    - [func_trim](#entity-func_trim)
    - [func_light](#entity-func_light)
    - [light](#entity-light)
    - [_decal](#entity-_decal)
    - [misc_decal](#entity-misc_decal)
- [💻 CLI Command Reference](#-cli-command-reference)

---

## 💡 Key Features

### 1. High-Performance Ray Tracing (Intel Embree)
The legacy BSP-traversal ray caster has been replaced with the industry-standard **Intel Embree 4.4.0** BVH builder and intersection kernels.
- **Unified Data Cache:** To maximize performance, the toolchain pre-calculates and caches the world-space origin and normal of every lightmap texel and volumetric voxel. This cached geometry is shared across all lighting stages (Direct, Radiosity, and Ambient), eliminating redundant coordinate reconstruction.
- **Intelligent Light Culling:** The tool uses an advanced culling system that calculates a light's physical "reach" based on energy intensity and a configurable `cutoff` threshold. This allows the ray tracer to ignore surfaces outside a light's influence radius with zero overhead.
- **Performance Gain:** The combination of Intel's highly optimized BVH kernels and our internal geometric caching results in a 10x performance gain during the ray tracing phase compared to traditional tools.

### 2. 32-bit Floating Point Pipeline
- **Internal Buffers:** All lighting data (luxels, light grid) is accumulated in float32 buffers to prevent rounding artifacts and light clipping.
- **Dynamic Normalization:** Final 8-bit output conversion is deferred allowing for superior tonemapping and highlight compression.

### 3. Improved Model Support (misc_model)
`misc_model` entities are no longer second class citizens. They are fully integrated into the world geometry.
- **Omnidirectional Lightmapping:** Triangle soups receive high-quality, seamless lightmaps. The UV-to-world rasterizer preserves the integrity of the mesh regardless of its topology.
- **Automatic Collision (HACD):** Every model placed into the map is solid by default. Optimized convex collision hulls are automatically generated using the **HACD (Hierarchical Approximate Convex Decomposition)** algorithm.
- **Solid and Lightmapped by Default:** Unless explicitly disabled in the shader, all models are solid and lightmapped just like world brushes.
- **Modelgroups:** `misc_model`s can be part of `func_*` entities through them, bundling their visuals and collision seamlessly.
- **Supported Formats:**:
    - **Wavefront:** `.obj` (+ `.mtl`)
    - **Autodesk:** `.fbx`, `.ase`
    - **glTF:** `.gltf`, `.glb`
    - **Quake:** `.md3`, `.md5`
    - **LightWave:** `.lwo`
    - **Inter-Quake Model:** `.iqm`

### 4. Automatic Edge Chamfering
A highly requested feature that visually rounds the sharp edges of structural and detail brushes, creating a bevel effect.
- **Natural Highlights:** Prevents the classic "razor sharp" look of early 3D engines and naturally catches specular highlights on corners.
- **Deep Control Hierarchy:** Chamfer width and concave/convex joint separation can be controlled globally via CLI or JSON profiles, overridden per-material in `.shader` files, or finely tuned on a per-entity basis using entity keys.
- **T-Junction Preservation:** Integrates perfectly with the engine's T-junction fixing logic to guarantee watertight, crack-free meshes even after subdivision and corner contraction.
- **Geometry Optimization (Trisoup Conversion):** As a byproduct of the chamfering pass, fragmented planar surfaces are automatically promoted and merged into larger, continuous triangle soups. This significantly reduces draw calls and ensures seamless lightmap filtering across complex brushwork that would otherwise be split into hundreds of individual surfaces.

### 5. Deluxe Mapping V2
An improved, irradiance-preserving deluxe mapping system.
- **Iterative Accumulation:** Light directions are resolved during each contribution, preventing "rim bloom" artifacts[1] and ensuring stable directional highlights even in areas with opposing light sources.
- [1] It's not possible to fully eliminate them. They're part of the nature of deluxemapping, but they happen less often and the smooth filter can hide them when they happen.

### 6. Macro Ambient Occlusion (MAO)
A new volumetric ambient occlusion pass computes spatial "openness" for both the light grid and lightmaps.
- **Atmospheric Depth:** Large-scale features (caves, corridors, plazas) naturally darken or brighten based on their enclosure, grounding objects in the environment without requiring thousands of point lights.
- **Bent Normals:** Per-texel ambient energy includes a directional hint (Bent Normal), ensuring normal maps respond correctly to the primary direction of incoming ambient light.

### 7. Brush-to-Light Generation (func_light)
The `func_light` entity allows mappers to create complex light setups directly from brush geometry without any shader scripting.
- **Surface & Spot Lights:** Any brush surface can be instantly turned into an emissive light source or a spotlight emitter.
- **Ease of Use:** Mappers can control light intensity, color, and attenuation directly via entity keys, making high-quality emissive lighting accessible without modifying assets.


### 8. In-Editor Configurable Surface Colors
Mappers can now tint and recolor materials directly within the map editor without having to author duplicate shader files (the shader must have been created for this purpose, tho).
- **Entity Level Tinting:** By applying the `vertexcolor` key to entities like `misc_model` or `func_group`, the compiler automatically overrides the vertex colors of all associated surfaces.
- **Workflow Efficiency:** This allows for rapid iteration and material reuse. For example, a single "car" model or "crate" brushwork group can be placed multiple times in a map, each tinted a different color exclusively via entity keys.


### 9. Modernized Color & Lighting Models
- **Global Color Pipeline:** A unified color parser supports floating-point RGB (0..1), integer RGB (0..255), and Hexadecimal codes (`#RRGGBB`).
- **Artistic Integrity:** The compiler no longer automatically normalizes light colors. This preserves the original brightness and "mood" intended by the mapper.
- **Advanced Shading Models:** Support for industry-standard attenuation models (Inverse-Square, Unreal, Smoothstep) and shading kernels (Half-Lambert, Quadratic).

### 10. High-Quality 2D Filtering
Post-process lightmap filtering has been rebuilt for maximum quality and seamlessness across geometry charts.
- **Stitch Filtering:** The toolchain automatically identifies adjacent surfaces sharing world-space edges (partners) and performs cross-surface bilinear sampling to eliminate visible seams.
- **Volumetric Filtering:** Specialized world-space filtering for complex "triangle soup" (models) ensures smooth lighting gradients even on meshes with disconnected UV islands.
- **Per-Surface Customization:** Mappers can override the global smoothing settings on a per-entity basis using the `smooth` key, allowing for sharper shadows on some objects and softer, more diffuse lighting on others.
- **GPU Acceleration:** All filtering and anti-aliasing passes are fully GPU-accelerated via OpenCL, allowing for high-quality multi-pass smoothing without significant compile-time penalties.

### 11. Customizable Game Profiles (JSON)
Externalized game-specific configurations into customizable JSON profiles.
- **Engine Adaptability:** Mappers can easily define new profiles for different game engines, specifying unique BSP versions, lump counts, and hard limits (max verts/indexes).
- **Global Defaults:** Every lighting parameter—including default lightmap sizes, shading models, and attenuation curves—can be tuned per game profile to ensure consistent results across different projects.

### 12. Automatic Light Halos
Spotlights and directed light sources can now automatically generate volumetric "halos" (billboards) to simulate atmospheric scattering.
- **Dynamic Sizing:** Halo dimensions are automatically calculated based on the light's intensity and radius, ensuring the visual effect matches the physical light cone.
- **Shader Control:** Mappers can override the default halo effect using the `haloshader` key or disable it entirely for specific lights. The halo size can also be controlled with the `haloscale` key.
- **Vertex Color Integration:** Halos inherit their color from the light source.



---

## 🛠 Recommended Workflow

To achieve the best results and maintain organized projects, it is recommended to follow the configuration hierarchy of the toolchain:

### 1. Game Profile (Global Defaults)
The JSON files in the `makebspdata/` directory should define the **baseline standards** for your project. This includes engine-specific limits and the general "look" of the lighting that should apply to all maps in that game. 

### 2. Worldspawn (Map-Specific Setup)
The **Worldspawn entity** should be used to define map-specific lighting and surface treatments. Any key set in the Worldspawn (e.g., `samplesize`, `ambient`, `shading`, `radiosity`,  `deluxe`) will override the defaults in the game profile. This allows each map to have its own unique atmospheric character without changing the compiler's global configuration.

### 3. CLI Arguments (Build Modifiers)
Command-line arguments should be treated as **build-specific modifiers**. Use them to toggle between "fast" development builds and "high-quality" final bakes. For example:
- **Dev Build:** Use `-fast` (which ignores high-resolution lightmap requests in `makebsp` and forces fast rasterized voxelization in `makelight`) or a larger `-samplesize` via CLI to get quick feedback.
- **Final Build:** Use `-upscale` or smaller `-samplesize` to maximize fidelity for the release version.

By following this hierarchy, your map source files (`.map`) remain portable and consistent, while you retain the flexibility to control the compile-time/quality tradeoff on a per-build basis.

> **A Note on Sky Shaders and Lighting**<br>
> It is **highly recommended not to configure lighting directly inside your sky shaders** (e.g., using `q3map_surfacelight` or `q3map_sun`). 
> - **Ambient Overlap:** The tool automatically calculates directional ambient irradiance from surfaces exposed to the sky (via the `ambient_sky` worldspawn key). If your sky shader also emits surface light, these two systems will overlap and blow out your lighting.
> - **Sun Entities:** For sunlight, use a standard `light` entity and set the `sun` key to `1` (or add `-sun` to the entity name/class depending on your editor's setup). This produces the exact same result as a shader-based sun but allows you to control the sun's direction, color, and intensity on a per-map basis without needing to duplicate and modify shader files for every new map.

---

## ⚙️ Game Profiles (makebspdata)

The compiler uses a flexible, data-driven profile system powered by `.json` files located in the `makebspdata/` directory. By default, the compiler provides two core profiles: `qfusion.json` and `quake3.json`. 

You can load a specific game profile at compile time using the `-game <name>` CLI argument.

### 1. Atomic Overrides (Sparse Loading)
The game profile parser is designed around **atomic loading**. When a `.json` profile is loaded, the compiler establishes the default QFusion settings in memory as a baseline. 

If your `.json` file only contains a few keys (e.g., just `"flareShader": "my_flare"`), the compiler will safely load your specific overrides while keeping all other default settings intact. A game profile does not need to list every single property to be valid.

### 2. Profile Inheritance (`"template"`)
To maximize modularity, profiles can inherit from one another using the `"template"` key. 
If your profile specifies `"template": "quake3"`, the compiler will automatically pause, load the entire `quake3.json` profile to establish a new baseline, and *then* apply your profile's specific keys on top.

```json
{
  "game": "my_mod",
  "template": "quake3",
  "bspVersion": 47,
  "flareShader": "textures/my_mod/custom_flare"
}
```
*Note: The compiler has built-in infinite loop protection. If two profiles try to inherit each other in a circle, the compiler will cleanly break the loop (at a depth of 4) and throw a warning.*

### 3. Custom Surface Parameters (`customSurfaceParms`)
The compiler no longer relies strictly on hardcoded C arrays for surface flags. Modders can inject custom engine-specific `surfaceparm` flags directly into the compiler through the game profile!

These parameters will be automatically recognized by the shader parser and mapped into the BSP surfaces.

```json
  "customSurfaceParms": [
    { "name": "nowalljump", "clearSolid": false, "surfaceFlags": "0x400000", "contentFlags": 0 },
    { "name": "forcefield", "clearSolid": true,  "surfaceFlags": 0, "contentFlags": "0x40000" }
  ]
```
- **Base Support**: Supports both standard decimal integers (e.g. `4194304`) and hexadecimal strings (e.g. `"0x400000"`) for bit flags.
- **clearSolid**: If set to `true`, surfaces using this parameter will have the `CONTENTS_SOLID` flag automatically stripped from them.

---

## 🎨 Shader Modifications

List of additions and modifications made to shader parsing and features compared to the original q3map.

### New Shader Directives
- **q3map_vertexcolor <R G B>**: Overrides the vertex color for the surface.
- **q3map_surfacelight_glow <value>**: Sets the backface glow fraction for surface lights (enabled by default in CONTENTS_LAVA and CONTENTS_SLIME).
- **q3map_surfacelight_cutoff <value>**: Minimum energy threshold before the surface light is completely culled.
- **q3map_surfacelight_fadeout <value>**: Percentage of the surface light's reach to use for a softness fade at the cutoff (0.0 to 1.0).
- **q3map_surfacelight_nodeluxe**: Prevents the surface light from influencing the deluxe map's directionality. Instead it will only contribute color/energy (to prevent bumpmap distortions caused by trim lights).
- **q3map_backsplash_nodeluxe**: Prevents the surface light's backsplash from influencing the deluxe map's directionality.
- **q3map_deluxe_minangle <value>**: Alias: `q3map_deluxeminangle`. Overrides the minimum incidence angle threshold (in degrees, 0.0 to 89.0) for deluxemap directionality on this material. Useful for softening or clamping deluxemap angles on specific surfaces without changing global defaults.
- **q3map_lightColor <R G B>**: Alias for `q3map_lightRGB`. Sets the light emission color for the surface.
- **q3map_maxsamplesize <value>**: Enforces a minimum lightmap resolution (quality floor) by establishing a maximum limit on the surface's `samplesize` value. Ignored during `-fast` compilations.
- **q3map_minsmooth <value>**: Enforces a minimum lightmap smooth filter radius. It only acts if the global setting has a smaller smooth value than the requested minimum. Can be overridden by an entity's `smooth` key.
- **q3map_chamfer_convexwidth <value>**: Overrides the width of the chamfered strip generated on convex edges for this material.
- **q3map_chamfer_concavewidth <value>**: Overrides the width of the chamfered strip generated on concave (inner) joints for this material. Set to 0 to disable concave chamfering.

### Color Handling
- **Global Application:** The new color processing pipeline applies globally. It works for shader commands (e.g., `q3map_lightRGB`, `q3map_lightColor`, `q3map_vertexcolor`) as well as entity keys (e.g., `color`, `_color`).
- **Format Autodetection:** The compiler automatically detects and parses colors provided in three formats:
  - Standard floating-point RGB (0.0 to 1.0)
  - Integer RGB (0 to 255)
  - Hexadecimal color codes (e.g., `#RRGGBB`)
- **No Color Normalization:** The compiler no longer automatically normalizes color vectors. The color values you specify are used exactly as intended, preserving the original brightness and artistic intent rather than artificially brightening the light emission.

### General Changes
- **Default Backsplash:** The default light backsplash for surface lights is now disabled (0.0) unless explicitly requested by the game profile, via the `q3map_backsplash` directive or the entity key 'backsplash'. Backsplash is enabled by default for spotlights.

### Stage / Pass Directives
- **material <image>**: Scanned inside rendering passes. This QFusion-specific keyword is recognized and its image will be used as a fallback to derive average surface colors and light colors if `qer_editorimage` or `q3map_lightimage` are not specified.

### Surface Parameters (`surfaceparm <parameter>`)
- **nosolid**: Acts as an alias for `nonsolid`. Clears the solid flag from the surface.
- **nowalljump** [to be moved to custominfoparms]: Disables wall jumping on this surface (specifically for the QFusion engine).

---

## 📦 Entity Keys Reference

> **Note on Underscore Prefixes:** This compiler ignores single underscore prefixes (`_`) on entity keys. For example, using `_color` is treated exactly the same as `color`, and `_shading` is treated the same as `shading`. This allows mappers to use standard Quake 3 convention keys interchangeably with our new parameters.

### Entity: worldspawn

**Lighting / Global Ambiance**
- **ambient**: Global uniform ambient light intensity. Default 0.
- **color**: Sets the global ambient color. Default 1 1 1.
- **ambient_sky**: The RGB color vector for the sky ambient light. Used for surfaces facing upwards, overriding the global color.
- **shading**: Global light shading mode. Valid modes are: halflambert, lambert, quadratic, doublequadratic, unreal. Default lambert.
- **attenuation**: Global default distance falloff model for lights. Valid modes are: standard, soft, linear, unreal, smoothstep.
- **exposurefilter**: Global tonemapping exposure filter. Valid modes are: softknee, reinhard, filmic, linear (or off). Default reinhard.
- **saturation**: Global lightmap saturation multiplier (1.0 = normal, 1.5 = +50%, 0.0 = greyscale).
- **saturationramp**: Saturation contrast curve (prevents clipping in highlights/shadows). Valid modes are: filmic, power, halfpower, midtone, off.
- **cutoff**: Minimum energy threshold before any light is completely culled. Defaults to the global game.json minLightAdd value.
- **fadeout**: Percentage of a light's reach to use for a softness fade (0.0 to 1.0). Defaults to 0.0 (hard cut).
- **backsplashspot**: Default entity spotlight backsplash fraction (0.0 to 1.0).
- **backsplashsurface**: Default surface light backsplash fraction (0.0 to 1.0).
- **haloshader**: Global default shader to use for light halos. Set to "none" or "0" to disable them.
- **ambient_testradius**: Radius in world units to test for solid in macro-ambient occlusion (default: 512).
- **ambient_gatheradius**: Gather radius of light probes for spherical interpolation (default: 256).
- **grid_ambientbias**: Non-linear gamma bias applied to the ambient component of the light grid. Default 1.5.
- **grid_directbias**: Non-linear gamma bias applied per-light to the directional component of the light grid. Default 1.5.
- **grid_smoothambient**: Radius in world units for smoothing the ambient component of the light grid. Set to 0 to disable. Default 256.0.
- **grid_smoothambient_passes**: Number of iterative smoothing passes to apply to the ambient light grid. Default 4.
- **grid_smoothdirect**: Radius in world units for smoothing the directional component of the light grid. Default 128.0 (qfusion only) or 0.0 (disabled).
- **grid_smoothdirect_passes**: Number of iterative smoothing passes to apply to the directional light grid. Default 3.
- **grid_minambient**: Minimum hard light floor applied to the ambient light grid (in 8-bit scale). Set to 0 to disable.
- **grid_maxambient**: Hard clamp applied to the maximum ambient light the grid can accumulate, preserving color luminance. Set to 0 to disable.
- **_lightingIntensity**: [qfusion engine key] Custom fixed normalization scale for 8-bit LDR lightmap output.Defaults to 3.0

**Lightmaps & Rendering Passes**
- **samplesize**: Global default lightmap sample size in game units (e.g., 16). Default depends on game profile (4 or 8).
- **deluxe**: Enable (1) or disable (0) deluxe mapping globally (direction maps). Default depends on game profile (1 or 0).
- **deluxe_minangle**: Minimum angle (in degrees) to blend deluxemaps (0 to 90). Higher value = softer bumpmapping. Default depends on game profile (15.0 or 40.0). Can be overridden per-material using `q3map_deluxe_minangle`.
- **supersample**: Global supersampling trace radius (e.g., 0.5 or 2.0). Defaults to 0 (disabled). Exceeding 8.0 is not recommended.
- **smooth**: Global lightmap smooth filter radius. Defaults to 0.35. Set to 0.0 to disable global smoothing.
- **smoothpasses**: Number of smoothing passes applied to the lightmaps. Defaults to 4.
- **antialiasing**: Number of global antialiasing passes. Defaults to 0 (disabled)

**Radiosity (Bounce Light)**
- **radiosity**: Global radiosity (bounced light) intensity. Defaults to 1.0. Set to 0 to disable radiosity.
- **rad_color_ratio**: Radiosity color transfer (how much the texture color tints the bounce light). Default 1.0. When using ambient lighting reducing 'rad_color_ratio' transfers the ambient sky color into the radiosity light.
- **rad_interval**: Radiosity surface voxelization interval/grid size. Default 4. Must be a factor of 2. The bigger the faster the radiosity passes, but it impacts the intensity.
- **rad_ao_intensity**: Ambient Occlusion intensity multiplier (0.0 to 1.0) applied during the first radiosity pass. The bigger, the darker the occlusion shadows. Default 0.5.
- **rad_ao_min**: Minimum ambient occlusion fade distance. Default 0 world units. The ambient occlusion will fade from rad_ao_intensity at rad_ao_min to 0.0 at rad_ao_max.
- **rad_ao_max**: Maximum ambient occlusion fade distance. Default 24 world units.

**Geometry & BSP**
- **blocksize**: Global size of BSP map splitting blocks (e.g., 1024).
- **enforcesamplesize**: Forces makebsp to subdivide brushes to match the requested lightmap sample size. Integer boolean (1 or 0). Default 1.
- **chamfer_convexwidth**: Global override for the width of the convex chamfer strips on worldspawn brushes.
- **chamfer_concavewidth**: Global override for the width of the concave chamfer strips on worldspawn brushes.

### Entity: misc_model

**User keys**
- **smooth**: lightmap smooth filter radius to use on this model.
- **vertexcolor**: Overrides the vertex color for all surfaces of this model instance.
- **upscale**: Enable or disable raytracing at 2x lightmap resolution.
- **supersample**: Supersampling radius override for the model's lightmaps.
- **lightmapscale**: Entity-level scaling factor for lightmap resolution on the model (clamped between 0.01 and 16.0).
- **forceuvgen**: Enable (default) or disable to force generating new lightmap UVs from scratch. Disabled uses the model UVs.
- **collisiontype**: Overrides how the model's collision mesh is generated. Valid working values are: object, wrap, extrude (buggy), none (alias nosolid / nonsolid). More to come.
- **castshadows**: Enable (1) or disable (0) the entity's geometry from casting shadows into the lightmap. Default 1.
- **modelgroup**: Links this `misc_model` to a brush model entity (like `func_plat` or `func_door`). When set to the same `modelgroup` name as a parent brush entity, the model's visuals and automatically generated collision hulls are bundled with the brush model and move seamlessly with it.
- **decalgroup**: Used by `_decal` entities. If the `_decal` entity specifies a `decalgroup`, its projection will only be applied to brushes, patches, and models that share the exact same `decalgroup` name.

**Editor keys**
- **model**: The path to the 3D model file to load.
- **origin**: The base translation/position of the model in the world (X Y Z).
- **angles**: The rotation of the model (Pitch Yaw Roll).
- **modelscale**: A uniform scaling factor applied to all axes (defaults to 1.0).
- **modelscale_vec**: A non-uniform scaling vector (X Y Z). If set to 0 0 0, it falls back to modelscale.

### Entity: func_group

**Brushes**
- **smooth**: lightmap smooth filter radius to use on this group's surfaces.
- **vertexcolor**: Overrides the vertex color for all surfaces of this group.
- **upscale**: Enable or disable raytracing at 2x lightmap resolution.
- **supersample**: Supersampling radius override for the group's lightmaps.
- **samplesize**: (Alias: `lightmapsamplesize`). Overrides the lightmap sample size for this entity's surfaces.
- **enforcesamplesize**: Subividide the surfaces if they can't match the samplesize. Integer boolean (1 or 0).
- **castshadows**: Enable (1) or disable (0) the entity's brushes from casting shadows into the lightmap. Default 1.
- **modelgroup**: Links `misc_model`s to this entity. Models with the matching `modelgroup` name will be bundled with it.
- **decalgroup**: Used by `_decal` entities. If the `_decal` entity specifies a `decalgroup` key, its projection will only be applied to brushes, patches, and models that share the exact same `decalgroup` name.
- **chamfer_convexwidth**: Overrides the chamfer width for convex edges on this group's surfaces.
- **chamfer_concavewidth**: Overrides the chamfer width for concave edges on this group's surfaces.

**Terrain** *(This is the original untouched and unverified q3map terrain.)*
- **terrain**: If set to "1", converts the brushes in this group into a blended terrain surface using an alphamap.
- **shader**: Specifies the base shader to use for terrain generation (required if terrain is "1").
- **alphamap**: Path to the image file used to blend terrain layers (required if terrain is "1").
- **layers**: Number of terrain layers to blend from the alphamap (required if terrain is "1").

### Entity: func_trisoup

Converts standard map brushes into a continuous, smoothed triangle soup (mesh). This is useful for creating complex organic shapes out of brushes and applying smooth vertex normal shading across them, similar to a 3D model.

**Trisoup set up**
- **shadeangle**: The angle threshold (in degrees) used to calculate smooth vertex normals across the mesh. Edges with an angle less than this value will have their normals blended for smooth lighting. Defaults to 46.0.

**Brushes**
- **smooth**: Lightmap smooth filter radius to use on this entity's surfaces.
- **vertexcolor**: Overrides the vertex color for all surfaces of this group.
- **upscale**:  Enable or disable raytracing at 2x lightmap resolution.
- **supersample**: Supersampling radius override for the entity's lightmaps.
- **samplesize**: (Alias: `lightmapsamplesize`). Overrides the lightmap sample size for this entity's surfaces.
- **enforcesamplesize**: Subdivide the surfaces if they can't match the samplesize. Integer boolean (1 or 0).
- **decalgroup**: Used by `_decal` entities. If the `_decal` entity specifies a `decalgroup` key, its projection will only be applied to brushes, patches, and models that share the exact same `decalgroup` name.
- **chamfer_convexwidth**: To do (Currently inactive because brushes are converted to a trisoup prior to the chamfering pass).
- **chamfer_concavewidth**: To do (Currently inactive because brushes are converted to a trisoup prior to the chamfering pass).

### Entity: func_trim

An iterative plane-trimming CSG operator for `misc_model` entities. It uses the drawable planes of its brushes to slice and trim away portions of any intersecting `misc_model` (turning standard brushes into an invisible cutting tool). The entity and its brushes are completely suppressed from the final BSP.

**Targeting**
- **target**: If specified, the `func_trim` will only cut `misc_model` entities that have a matching `targetname`. If left blank, it acts globally and cuts any intersecting `misc_model`.

### Entity: func_light

**Light set up**
- **type**: Can be "point" (alias:"pointlight"), "spot" (alias:"spotlight" or default), or "surface" (alias:"surfacelight"). Determines whether to generate point lights, spotlights, or emissive surfaces from the brushes.
- **nudge**: Distance to nudge the generated light entities away from the brush surfaces. Defaults to 1.0 for spotlights (ignored for surface lights).
- **light**: The emission strength or intensity of the light.
- **color**: The color of the light. If not specified, it will attempt to derive it from the surface texture (lightimage).
- **backsplash**: Backsplash percentage for surface lights and spotlights (how much light bounces back). Default: surface 0.0/spot 0.1.
- **grid_ambientscale**: Multiplier for this light's contribution to the volumetric ambient light grid. Default 1.0. Set to 0 to skip grid ambient contribution.
- **grid_directscale**: Multiplier for this light's contribution to the volumetric direct light grid. Default 1.0. Set to 0 to skip grid direct contribution.
- **nodeluxe**: If set to 1, the light will not influence the deluxe map's directionality.
- **backsplash_nodeluxe**: If set to 1, the backsplash generated by this light will not influence the deluxe map's directionality.
- **attenuation**: Distance falloff model. Valid modes are: standard, soft, linear, unreal, smoothstep.
- **cutoff**: Minimum energy threshold before the light is completely culled. Defaults to the global game.json minLightAdd value.
- **fadeout**: Percentage of the light's reach to use for a softness fade at the cutoff (0.0 to 1.0). Defaults to 0.0 (hard cut).
- **prestep**: (Aliases: `rampoffset`, `extradist`). Distance offset applied to the core of the light to prevent infinite brightness at the origin. Defaults to 16.0. (Ignored for surface lights).

**Surfacelights**
- **subdivide**: Controls how finely surface lights are subdivided.

**Spotlights**
- **radius**: Radius of the spotlight cone at the target distance (defaults to 64).
- **softness**: Spotlight cone softness multiplier (defaults to 1.0).
- **target**: Target entity name to aim the spotlight at.
- **dir**: Explicit direction vector (X Y Z) for the spotlight (when not using a target).
- **angles**: Rotation angles (Pitch Yaw Roll) for the spotlight (when not using a target).
- **haloshader**: Specific shader to use for the volumetric halo. Set to "none" or "0" to disable the halo for this light.
- **haloscale**: Scales the size of the generated halo surface. Defaults to 1.0.

**Brushes**
- **smooth**: Lightmap smooth filter radius to use on this entity's surfaces.
- **vertexcolor**: Overrides the vertex color for all surfaces of this group.
- **upscale**:  Enable or disable raytracing at 2x lightmap resolution.
- **supersample**: Supersampling radius override for the entity's lightmaps.
- **samplesize**: (Alias: `lightmapsamplesize`). Overrides the lightmap sample size for this entity's surfaces.
- **enforcesamplesize**: Subdivide the surfaces if they can't match the samplesize. Integer boolean (1 or 0).
- **castshadows**: Enable (1) or disable (0) the entity's brushes from casting shadows into the lightmap. Default 1.
- **modelgroup**: Links `misc_model`s to this entity. Models with the matching `modelgroup` name will be bundled with it.
- **decalgroup**: Used by `_decal` entities. If the `_decal` entity specifies a `decalgroup` key, its projection will only be applied to brushes, patches, and models that share the exact same `decalgroup` name.
- **chamfer_convexwidth**: Overrides the chamfer width for convex edges on this entity's surfaces.
- **chamfer_concavewidth**: Overrides the chamfer width for concave edges on this entity's surfaces.

### Entity: light

**Light set up**
- **light**: The emission strength or intensity of the light.
- **color**: The color of the light.
- **grid_ambientscale**: Multiplier for this light's contribution to the volumetric ambient light grid. Default 1.0. Set to 0 to skip grid ambient contribution.
- **grid_directscale**: Multiplier for this light's contribution to the volumetric direct light grid. Default 1.0. Set to 0 to skip grid direct contribution.
- **nodeluxe**: If set to 1, the light will not influence the deluxe map's directionality.
- **backsplash_nodeluxe**: If set to 1, the backsplash generated by this light will not influence the deluxe map's directionality.
- **attenuation**: Distance falloff model. Valid modes are: standard, soft, linear, unreal, smoothstep.
- **cutoff**: Minimum energy threshold before the light is completely culled. Defaults to the global game minLightAdd value (0.1).
- **fadeout**: Percentage of the light's reach to use for a softness fade (0.0 to 1.0). Defaults to 0.0 (hard cut).
- **prestep**: (Aliases: `rampoffset`, `extradist`). Distance offset applied to the core of the light to prevent infinite brightness at the origin. Defaults to 16.0.
- **style**: [currently broken] Light style index for dynamic lighting (e.g. flickering, pulsing).
- **lightimage**: If color is not specified, uses the average color of this shader.

**Spotlights**
- **radius**: Radius of the spotlight cone at the target distance.
- **softness**: Spotlight cone softness multiplier (defaults to 1.0).
- **backsplash**: Backsplash percentage (how much light bounces back). Default 0.1.
- **target**: Target entity name to aim the spotlight at.
- **dir**: Explicit direction vector (X Y Z) for the spotlight (when not using a target).
- **angles**: Rotation angles (Pitch Yaw Roll) for the spotlight (when not using a target).
- **haloshader**: Specific shader to use for the volumetric halo. Set to "none" or "0" to disable the halo for this light.
- **haloscale**: Scales the size of the generated halo surface. Defaults to 1.0.

### Entity: _decal

Projects a 2D surface (defined by patches or brushes) onto map geometry. This is a classic q3map2 feature.
- **Surface Types**: Can be casted on all surface types (brushes, patches, and `misc_model`s).
- **Limitations**: Distance alphablending is not supported.

**Keys**
- **target**: Target entity name used to determine the projection direction and bounds. If omitted, the decal projects straight down.
- **decalgroup**: If specified, this decal will *only* project onto brushes, patches, and models that share the exact same `decalgroup` key. For brushes and patches, the key must be applied to the `func_group` they belong to.
- **patchSubdivision**: Adjusts the resolution of the decal when projected onto curved patches. The default is 0.4. Lowering this value (e.g. down to 0.1) increases the triangle density for a smoother curve, while higher values (e.g. 1.0 or 4.0) reduce the triangle count.

### Entity: misc_decal

A point entity that projects a 2D quad decal onto map geometry without requiring in-map brush or patch geometry as the projection source. The quad is centered at the entity's origin and faces opposite to the projection direction.

- **Projection Direction**: Determined by the `angles` key, or by targeting another entity (such as a `target_position`) via the `target` key. If neither is specified, it projects in the direction defined by the `angles` key (defaulting to the +X axis, or "East", if angles are zero/not set). To project straight down, set the Pitch to `90`.
- **Targeting / Groups**: Multiple `misc_decal` entities can safely target the same target entity (e.g. `target_position`) without conflict.

**Keys**
- **shader**: The shader to project (e.g., `textures/decals/logo`). Required; defaults to `textures/common/nodraw` with a warning if omitted.
- **width**: The width of the projecting quad source in game units. If omitted, defaults to the shader's image width multiplied by `0.25` (or `64` if the image size cannot be resolved).
- **height**: The height of the projecting quad source in game units. If omitted, defaults to the shader's image height multiplied by `0.25` (or `64` if the image size cannot be resolved).
- **scale**: A multiplier applied to the final width and height of the decal. Useful for scaling decals proportionally without calculating the absolute dimensions. Default: `1.0`.
- **distance** / **depth**: The projection depth/distance in game units. Default: `64`.
- **angles**: Rotation angles (Pitch Yaw Roll) that define the projection direction if no `target` is set.
- **rotate**: An angle (in degrees) to rotate the projected decal clockwise around the projection axis. Particularly useful for spinning the decal when pointing at a target entity.
- **target**: Target entity name used to determine the projection direction. If specified, the projection vector points from the `misc_decal` towards the target entity.
- **decalgroup**: If specified, this decal will *only* project onto brushes, patches, and models that share the exact same `decalgroup` key.
- **vertexcolor**: If specified, overwrites the vertex lighting of the generated decal geometry with a flat custom color (format: `R G B` or hex `#RRGGBB`).

---

## 💻 CLI Command Reference

### Makebsp CLI
Makebsp is the primary tool for BSP compilation, visibility calculation, and utility tasks.

**BSP Compilation (Default Mode)**
Used to compile a `.map` file into a `.bsp` file.
*New or relevant to makebsp:*
- `-game <G>`: Load a specific game profile (e.g., quake3, qfusion) from `makebspdata/<G>.json`.
- `-samplesize <N>`: Sets the default lightmap sample size (e.g., 4, 8, 16). Lower values = higher resolution.
- `-lightmapimagesize <N>`: Forces a specific lightmap atlas size (e.g., 1024).
- `-enforceSampleSize <0|1>`: If enabled (1), strictly follows the sample size defined in shaders or globally, forcing subdivision if necessary.
- `-guessuvs`: [Experimental] Automatically calculates optimal UV packing resolution for triangle soup (models) before repacking.
- `-noautocaulk`: Disables early automatic face caulking (by default, makebsp automatically strips and caulks redundant coplanar, contained, or fully submerged faces before BSP construction).
- `-rootdir / -basepath / -fs_basepath <P>`: Set the engine root directory path. Can be specified multiple times to build layered search paths.
- `-userdir / -fs_homepath <P>`: Set the user/home directory path (where the compiled BSP will be written). Can be specified multiple times.
- `-gamedir / -fs_game <P>`: Set the active mod/game directory name. Can be specified multiple times.

*From q3map:*
- `-onlyents`: Only update the entities lump in an existing BSP file.
- `-onlytextures`: Only update the texture info in an existing BSP file.
- `-micro <V>`: Set the threshold volume for "microbrushes" to be ignored (default is very small).
- `-fulldetail`: Treat all brushes as structural (ignores the "detail" flag).
- `-nodetail`: Completely ignore detail brushes during BSP construction.
- `-nowater`: Skip processing of water surfaces.
- `-nofill`: Skip the outside-filling stage (can be used for "leaky" maps during development).
- `-nofog`: Skip processing of fog volumes.
- `-novis`: Skip inline visibility calculation.
- `-nosubdivide`: Disable subdivision of large surfaces.
- `-nocurves`: Ignore all curved surfaces (patches).
- `-notjunc`: Skip T-junction narrowing and fixing.
- `-chamferedges`: Enables automatic edge chamfering for smooth corner lighting (automatically skipped when compiling with `-fast`).
- `-nochamferedges`: Explicitly disables edge chamfering (overrides game profile defaults).
- `-chamfernosubdivide`: Disables T-junction surface splitting prior to chamfering.
- `-mergetrisoups <0|1>`: Enable/disable global merging of adjacent planar triangle soups (default 1).
- `-nodecimateplanar`: Disable the planar trisoup decimation pass.
- `-chamferconvexwidth <V>`: Sets the global size of convex chamfer strips (default 1.25).
- `-chamferconcavewidth <V>`: Sets the global size of concave chamfer strips (< 0 uses convex width, 0 skips concave chamfers).
- `-saveprt`: Do not delete the .prt file after processing.
- `-leaktest`: Abort immediately if a leak is found.
- `-v`: Enable verbose output.
- `-threads <N>`: Manually set the number of worker threads.
- `-fast`: Drop quality for quick development tests (disables edge chamfering and ignores `q3map_maxsamplesize`).

**Other Main Switches**
These switches change the primary mode of the executable.
- `-visonly`: [Notice: The standard visibility is already calculated with the bsp] Standalone Visibility calculation (requires .prt file).
  - `-merge`: Merges adjacent visibility data (can reduce file size).
  - `-nopassage`: Disables the passage-flow visibility optimization.
- `-exportmodels <bspname>`: Exports all `misc_model` (Triangle Soup) geometry from a BSP into `.obj` files. Models processed with -meta/forcemeta will be split in multple mini-meshes and unusable. Only useful for models originally compiled for vertex lighting.
    - `-ignoreplanar 1|0`: When enabled (default 1), skips exporting trisoup surfaces that are perfectly flat/planar (useful to avoid exporting standard wall geometry that was meta'd).
- `-info <bspname>`: Displays detailed statistics and lump information for the specified BSP file.

---

### Makelight CLI

**Radiosity**
- `-rad_passes <N>`: Number of radiosity (light bounce) iterations.
- `-radiosity <F>`: Global intensity multiplier for bounced light.
- `-rad_ao_intensity <F>`: Intensity of the crease ambient occlusion effect (0.0 to 1.0).
- `-rad_ao_min / -rad_ao_max`: Define the distance range for the radiosity ambient occlusion effect.

**Ambient lighting**
- `-ambient_grid_samples <N>`: Hemisphere ray count per Light Grid point for macro ambient.
- `-ambient_samples <N>`: Hemisphere ray count per Lightmap Texel for macro ambient.
- `-ambient_testradius <F>`: Maximum ray length for macro-ambient occlusion in world units.
- `-ambient_gatheradius <F>`: Gather radius for spherical interpolation in world units.

**Attenuation (Shading is angle attenuation)**
- `-shading <type>`: Set the shading model (lambert, halflambert, quadratic, doublequadratic, unreal).
- `-shading_softbias <F>`: Override the default soft bias (0.0 to 1.0) for the shading model.
- `-sunshading <type>`: Override the shading model specifically for the sun.
- `-attenuation <type>`: Set the distance falloff model (standard, soft, linear, unreal, smoothstep).

**Deluxe Mapping (Directional Lightmaps)**
- `-deluxe <0|1>`: Enable (1) or disable (0) deluxemapping (direction maps).
- `-deluxe_minangle <A>`: Clamp the minimum incidence angle for deluxe vectors (the higher the value the less 'bumpmapped'). Can be overridden per-material using `q3map_deluxe_minangle`.
- `-deluxe_radiosity_exaggerate <F>`: Exaggerate the incidence angle for bounced light. (may produce glitches on specular surfaces)
- `-deluxe_ambient_exaggerate <F>`: Exaggerate the incidence angle for ambient light. (Its impact is very low no matter how big it is)

**Post-Processing & Filtering**
- `-exposurefilter <type>`: Highlight compression filter (softknee, reinhard, filmic). To reduce hotspots.
- `-saturation <F>`: Global lightmap saturation multiplier (1.0 = normal, 1.5 = +50%, 0.0 = greyscale).
- `-saturationramp <type>`: Saturation contrast curve (prevents clipping). Valid modes: filmic, power, halfpower, midtone, off.
- `-antialiasing <N>`: Number of post-process anti-aliasing passes (The smooth filter is better, IMO).
- `-smoothpasses <N>`: Number of lightmap smoothing/blurring passes.
- `-smooth <R>`: Radius for smoothing and jittered supersampling.
- `-supersample <radius>`: Enable trace-time supersampling using a 8x jittered pattern. The radius defines the spread of the jitter in world units (e.g., 0.5 or 1.0). Set to 0 to disable.

**Performance & Debug**
- `-fast`: Drop quality for quick tests (disables edge chamfering in makebsp).
- `-lowmem`: Enables memory-mapped file mode to reduce RAM usage on extremely large maps.
- `-opencl <0|1>`: Enable (1) or disable (0) OpenCL GPU acceleration for supported passes.
- `-exportlightmaps`: Export a copy of the lightmaps as images for visual inspection.
- `-debuglightmaps`: Generate BMP files showing lightmap allocation and atlas usage.
- `-debuglightmapsalpha`: Generate BMP files showing exact lit pixels (highly accurate debug).
- `-nodirect`: Skip the direct lighting pass.
- `-directonly`: Only perform direct lighting (skips radiosity and ambient).
- `-radiosityonly`: Only perform the radiosity (bounce) pass.
- `-ambientonly`: Only perform the macro-ambient pass. 
- `-novertex`: Disable vertex lighting generation.
- `-nogrid`: Disable volumetric light grid generation.

**Light Grid (Volumetric Lighting)**
- `-grid_ambientbias <F>`: Non-linear gamma bias applied to the ambient component of the light grid.
- `-grid_directbias <F>`: Non-linear gamma bias applied per-light to the directional component of the light grid.
- `-grid_smoothambient <F>`: Radius in world units for smoothing the ambient component of the light grid.
- `-grid_smoothambient_passes <N>`: Number of iterative smoothing passes to apply to the ambient light grid.
- `-grid_smoothdirect <F>`: Radius in world units for smoothing the directional component of the light grid.
- `-grid_smoothdirect_passes <N>`: Number of iterative smoothing passes to apply to the directional light grid.
- `-grid_minambient <F>`: Minimum hard light floor applied to the ambient light grid.
- `-grid_maxambient <F>`: Hard clamp applied to the maximum ambient light the grid can accumulate.
