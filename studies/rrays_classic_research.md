# Radeon Rays 2.0 "Classic" (OpenCL) Integration Report

## 1. Overview
Radeon Rays 2.0 (RR2) is the final OpenCL-centric release of the SDK before the transition to DXR. It is a cross-platform, hardware-agnostic library that performs high-performance ray-triangle intersections using OpenCL 1.2+ kernels.

## 2. Why it fits our project
- **Compatibility**: Runs on any GPU supporting OpenCL 1.2 (AMD APUs, NVIDIA, Intel, and even CPUs as a fallback).
- **Linux/Windows**: Native support for both via the standard OpenCL ICD loader.
- **Stability**: Unlike RR4 which is still evolving, RR2 is a mature, stable API that won't change under us.

## 3. Technical Requirements
- **Backend**: `RR_BACKEND_OPENCL`.
- **Memory**: Requires `cl_mem` buffers for geometry.
- **Linking**: Requires `RadeonRays.lib` (or `.a` on Linux) and the OpenCL headers/library.

## 4. Implementation Challenges

### A. The "Ping-Pong" Bottleneck
The biggest challenge is CPU-GPU synchronization.
- **Naive approach**: Call the GPU for every single ray. **Result**: Slower than CPU because of PCIe latency.
- **Correct approach**: Batch millions of rays into a single buffer and send them all at once. We must refactor our lighting loops into "Gather -> Batch -> Dispatch -> Scatter" phases.

### B. Alpha-Masking (The "Transparency" Problem)
For surfaces like grates or leaves:
1. Radeon Rays returns a `hit_id`.
2. We must then look up the texture data for that triangle on the GPU.
3. This requires uploading all relevant **alpha-masked textures** to GPU VRAM as OpenCL `image2d_t` or buffer objects.

### C. Geometry Transformation
The BSP world must be flattened into a single unified Vertex/Index buffer. Since our map doesn't change during compile, we only do this once at startup.

## 5. Phased Implementation Strategy

### Phase 1: The "Filtering" Quick Win
- Initialize the OpenCL context.
- Port the `-aa 2` (Anti-aliasing) filter to a simple OpenCL kernel. 
- Since the lightmap is already on the GPU (or easily sent there), this will remove your current 2.5-minute bottleneck immediately.

### Phase 2: Hybrid Raytracing (Direct Lighting)
- Build the BVH from BSP surfaces.
- In `light_direct.c`, gather all rays for a whole surface.
- Dispatch to RR2 and read back occlusion results.
- **Benefit**: Massive speedup on maps with complex geometry or many shadow-casting entities.

### Phase 3: Full Radiosity GPU Integration
- Port the entire `RadiosityIntegrateOneSurface` logic to an OpenCL kernel.
- The GPU will calculate its own form factors and perform its own visibility checks.
- **Estimated Gain**: 10x to 50x speedup over current CPU implementation.

## 6. Conclusion
Radeon Rays 2.0 is the "compatibility king." It allows us to keep the project portable while unlocking the massive compute power of your 5600G. The biggest hurdle is not the raytracing itself, but the "Housekeeping" (moving textures and geometry to the GPU).
