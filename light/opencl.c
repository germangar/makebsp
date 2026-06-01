#include "light.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RELEASE_BUILD
#include "kernels_embedded.h"

/* Returns the embedded source string for a kernel filename, or NULL if not found. */
static const char *GetEmbeddedKernel(const char *filename) {
    if (!strcmp(filename, "aa_filter.cl")) return kernel_source_aa_filter;
    if (!strcmp(filename, "lm_common.cl")) return kernel_source_lm_common;
    if (!strcmp(filename, "smooth_filter.cl")) return kernel_source_smooth_filter;
    if (!strcmp(filename, "trisoup_filter.cl")) return kernel_source_trisoup_filter;
    return NULL;
}
#endif

cl_platform_id g_clPlatform;
cl_device_id   g_clDevice;
cl_context     g_clContext;
cl_command_queue g_clQueue;

qboolean useOpenCL    = qfalse;
qboolean openclEnabled = qtrue;

/* Persistent GPU state shared by all post-processing filters */
GpuLightmapState g_gpuLM;

/*
================
InitOpenCL
================
*/
void InitOpenCL(void) {
    cl_uint numPlatforms;
    cl_int  err;
    char platformName[128];
    char deviceName[128];

    _printf("--- InitOpenCL ---\n");
    useOpenCL = qfalse;
    memset(&g_gpuLM, 0, sizeof(g_gpuLM));

    err = clGetPlatformIDs(1, &g_clPlatform, &numPlatforms);
    if (err != CL_SUCCESS || numPlatforms == 0) {
        _printf("No OpenCL platforms found.\n");
        return;
    }

    clGetPlatformInfo(g_clPlatform, CL_PLATFORM_NAME, sizeof(platformName), platformName, NULL);
    _printf("Platform: %s\n", platformName);

    err = clGetDeviceIDs(g_clPlatform, CL_DEVICE_TYPE_GPU, 1, &g_clDevice, NULL);
    if (err != CL_SUCCESS)
        err = clGetDeviceIDs(g_clPlatform, CL_DEVICE_TYPE_ALL, 1, &g_clDevice, NULL);

    if (err != CL_SUCCESS) {
        _printf("No OpenCL devices found.\n");
        return;
    }

    clGetDeviceInfo(g_clDevice, CL_DEVICE_NAME, sizeof(deviceName), deviceName, NULL);
    _printf("Device:   %s\n", deviceName);

    g_clContext = clCreateContext(NULL, 1, &g_clDevice, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        _printf("Failed to create OpenCL context.\n");
        return;
    }

#if CL_TARGET_OPENCL_VERSION >= 200
    g_clQueue = clCreateCommandQueueWithProperties(g_clContext, g_clDevice, NULL, &err);
#else
    g_clQueue = clCreateCommandQueue(g_clContext, g_clDevice, 0, &err);
#endif

    if (err != CL_SUCCESS) {
        _printf("Failed to create OpenCL command queue.\n");
        return;
    }

    useOpenCL = qtrue;
    _printf("OpenCL initialized successfully.\n");
}

/*
================
ShutdownOpenCL
================
*/
void ShutdownOpenCL(void) {
    if (!useOpenCL) return;

    if (g_clQueue)   { clReleaseCommandQueue(g_clQueue);  g_clQueue   = NULL; }
    if (g_clContext) { clReleaseContext(g_clContext);      g_clContext = NULL; }

    useOpenCL = qfalse;
}

cl_program BuildOpenCLProgram(const char *filename, const char *options) {
    cl_program prog;
    cl_int     err;
    char      *src = NULL;
    int        fileSize = 0;
    size_t     size;
    qboolean   isFreeNeeded = qfalse;

    if (!useOpenCL) return NULL;

#ifdef RELEASE_BUILD
    const char *embedded = GetEmbeddedKernel(filename);
    if (embedded) {
        src = (char *)embedded;
        fileSize = strlen(src);
    }
#endif

    if (!src) {
        char fullPath[MAX_OS_PATH];
        sprintf(fullPath, "%skernels/%s", executablePath, filename);
        fileSize = LoadFile(fullPath, (void **)&src);
        if (fileSize <= 0) {
            _printf("BuildOpenCLProgram: Could not load %s\n", fullPath);
            return NULL;
        }
        isFreeNeeded = qtrue;
    }

    size = (size_t)fileSize;
    prog = clCreateProgramWithSource(g_clContext, 1, (const char **)&src, &size, &err);
    if (isFreeNeeded) free(src);

    if (err != CL_SUCCESS) {
        _printf("BuildOpenCLProgram: Failed to create program from %s\n", filename);
        return NULL;
    }

    err = clBuildProgram(prog, 1, &g_clDevice, options, NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t logSize;
        clGetProgramBuildInfo(prog, g_clDevice, CL_PROGRAM_BUILD_LOG, 0, NULL, &logSize);
        char *log = malloc(logSize);
        clGetProgramBuildInfo(prog, g_clDevice, CL_PROGRAM_BUILD_LOG, logSize, log, NULL);
        _printf("BuildOpenCLProgram: Build failed for %s\nLog:\n%s\n", filename, log);
        free(log);
        clReleaseProgram(prog);
        return NULL;
    }

    return prog;
}

/*
================
BuildOpenCLProgramWithCommon

Compiles lm_common.cl prepended to <filename>.
This is the standard builder for all lightmap post-processing filters.
================
*/
cl_program BuildOpenCLProgramWithCommon(const char *filename, const char *options) {
    if (!useOpenCL) return NULL;

    char  *commonSrc = NULL, *filterSrc = NULL;
    int    commonSize = 0, filterSize = 0;
    cl_program prog = NULL;
    cl_int  err;
    qboolean commonFreeNeeded = qfalse;
    qboolean filterFreeNeeded = qfalse;

#ifdef RELEASE_BUILD
    const char *embeddedCommon = GetEmbeddedKernel("lm_common.cl");
    const char *embeddedFilter = GetEmbeddedKernel(filename);
    if (embeddedCommon) {
        commonSrc = (char *)embeddedCommon;
        commonSize = strlen(commonSrc);
    }
    if (embeddedFilter) {
        filterSrc = (char *)embeddedFilter;
        filterSize = strlen(filterSrc);
    }
#endif

    if (!commonSrc) {
        char commonPath[MAX_OS_PATH];
        sprintf(commonPath, "%smakebsp/kernels/lm_common.cl", executablePath);
        commonSize = LoadFile(commonPath, (void **)&commonSrc);
        if (commonSize <= 0) {
            _printf("BuildOpenCLProgramWithCommon: Could not load %s\n", commonPath);
            return NULL;
        }
        commonFreeNeeded = qtrue;
    }

    if (!filterSrc) {
        char filterPath[MAX_OS_PATH];
        sprintf(filterPath,  "%smakebsp/kernels/%s", executablePath, filename);
        filterSize = LoadFile(filterPath, (void **)&filterSrc);
        if (filterSize <= 0) {
            _printf("BuildOpenCLProgramWithCommon: Could not load %s\n", filterPath);
            if (commonFreeNeeded) free(commonSrc);
            return NULL;
        }
        filterFreeNeeded = qtrue;
    }

    const char *sources[2] = { commonSrc, filterSrc };
    size_t      sizes[2]   = { (size_t)commonSize, (size_t)filterSize };

    prog = clCreateProgramWithSource(g_clContext, 2, sources, sizes, &err);
    if (commonFreeNeeded) free(commonSrc);
    if (filterFreeNeeded) free(filterSrc);

    if (err != CL_SUCCESS) {
        _printf("BuildOpenCLProgramWithCommon: clCreateProgramWithSource failed (%d)\n", err);
        return NULL;
    }

    /* Inject atlas dimensions as compile-time constants so lm_common.cl can use them */
    char fullOpts[256];
    int scale = g_gpuLM.upscale > 0 ? g_gpuLM.upscale : 1;
    snprintf(fullOpts, sizeof(fullOpts),
             "-DLIGHTMAP_WIDTH=%d -DLIGHTMAP_HEIGHT=%d %s",
             LIGHTMAP_WIDTH * scale, LIGHTMAP_HEIGHT * scale,
             options ? options : "");

    err = clBuildProgram(prog, 1, &g_clDevice, fullOpts, NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t logSize;
        clGetProgramBuildInfo(prog, g_clDevice, CL_PROGRAM_BUILD_LOG, 0, NULL, &logSize);
        char *log = malloc(logSize);
        clGetProgramBuildInfo(prog, g_clDevice, CL_PROGRAM_BUILD_LOG, logSize, log, NULL);
        _printf("BuildOpenCLProgramWithCommon: Build failed for %s\nLog:\n%s\n", filename, log);
        free(log);
        clReleaseProgram(prog);
        return NULL;
    }

    return prog;
}

/*
================
GpuLightmapState_Free

Releases all persistent GPU buffers.
================
*/
void GpuLightmapState_Free(void) {
    GpuLightmapState *s = &g_gpuLM;

#define REL(b) if (b) { clReleaseMemObject(b); b = NULL; }
    REL(s->atlasA);
    REL(s->atlasB);
    REL(s->deluxeA);
    REL(s->deluxeB);
    REL(s->normalA);
    REL(s->normalB);
    REL(s->maskBuf);
    REL(s->surfacesBuf);
    REL(s->partnerData);
    REL(s->partnerOffsets);
    REL(s->validList);
    REL(s->radiiBuf);
    REL(s->pixelToSurface);
    REL(s->pixelToX);
    REL(s->pixelToY);
#undef REL

    s->numPlanarSurfaces = 0;
    s->numValid          = 0;
    s->totalAtlasPixels  = 0;
    s->pingIsA           = 1;
}

/*
================
GpuLightmapState_Download

Reads back the current output atlas buffer into lightFloats.
================
*/
void GpuLightmapState_Download(void) {
    GpuLightmapState *s = &g_gpuLM;
    cl_mem src      = s->pingIsA ? s->atlasA  : s->atlasB;
    cl_mem deluxeSrc = s->pingIsA ? s->deluxeA : s->deluxeB;
    cl_mem normalSrc = s->pingIsA ? s->normalA : s->normalB;
    size_t atlasBytes = (size_t)s->totalAtlasPixels * 3 * sizeof(float);
    size_t maskBytes  = (size_t)s->totalAtlasPixels * sizeof(byte);

    if (s->upscale <= 1) {
        clEnqueueReadBuffer(g_clQueue, src,       CL_TRUE, 0, atlasBytes, lightFloats,  0, NULL, NULL);
        if (deluxeFloats && deluxeSrc)
            clEnqueueReadBuffer(g_clQueue, deluxeSrc, CL_TRUE, 0, atlasBytes, deluxeFloats, 0, NULL, NULL);
        if (normalFloats && normalSrc)
            clEnqueueReadBuffer(g_clQueue, normalSrc, CL_TRUE, 0, atlasBytes, normalFloats, 0, NULL, NULL);
    } else {
        /* Downscale 2x2 -> 1x with mask weights */
        float *temp2x  = malloc(atlasBytes);
        float *dtemp2x = (deluxeFloats && deluxeSrc) ? malloc(atlasBytes) : NULL;
        float *ntemp2x = (normalFloats && normalSrc) ? malloc(atlasBytes) : NULL;
        byte  *mask2x  = malloc(maskBytes);
        if (!temp2x || !mask2x) {
            if (temp2x)  free(temp2x);
            if (dtemp2x) free(dtemp2x);
            if (ntemp2x) free(ntemp2x);
            if (mask2x)  free(mask2x);
            return;
        }

        clEnqueueReadBuffer(g_clQueue, src,       CL_TRUE, 0, atlasBytes, temp2x, 0, NULL, NULL);
        if (dtemp2x) clEnqueueReadBuffer(g_clQueue, deluxeSrc, CL_TRUE, 0, atlasBytes, dtemp2x, 0, NULL, NULL);
        if (ntemp2x) clEnqueueReadBuffer(g_clQueue, normalSrc, CL_TRUE, 0, atlasBytes, ntemp2x, 0, NULL, NULL);
        clEnqueueReadBuffer(g_clQueue, s->maskBuf, CL_TRUE, 0, maskBytes, mask2x, 0, NULL, NULL);

        int scale = s->upscale;
        int numLms = s->totalAtlasPixels / (LIGHTMAP_WIDTH * scale * LIGHTMAP_HEIGHT * scale);
        int W = LIGHTMAP_WIDTH, H = LIGHTMAP_HEIGHT;
        int W2 = W * scale, H2 = H * scale;

        #pragma omp parallel for schedule(static)
        for (int m = 0; m < numLms; m++) {
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    int p1 = (m * H + y) * W + x;
                    if (lightAlphaMask[p1] == 0) continue;

                    int p2 = (m * H2 + y * scale) * W2 + x * scale;
                    float sum[3] = {0,0,0};
                    float dsum[3] = {0,0,0};
                    float nsum[3] = {0,0,0};
                    float sumW = 0.0f;

                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            int pa = p2 + dy * W2 + dx;
                            if (mask2x[pa] != 0) {
                                float *smp = &temp2x[pa * 3];
                                sum[0] += smp[0]; sum[1] += smp[1]; sum[2] += smp[2];
                                if (dtemp2x) { dsum[0]+=dtemp2x[pa*3]; dsum[1]+=dtemp2x[pa*3+1]; dsum[2]+=dtemp2x[pa*3+2]; }
                                if (ntemp2x) { nsum[0]+=ntemp2x[pa*3]; nsum[1]+=ntemp2x[pa*3+1]; nsum[2]+=ntemp2x[pa*3+2]; }
                                sumW += 1.0f;
                            }
                        }
                    }

                    if (sumW > 0.01f) {
                        float invW = 1.0f / sumW;
                        lightFloats[p1*3+0] = sum[0]*invW;
                        lightFloats[p1*3+1] = sum[1]*invW;
                        lightFloats[p1*3+2] = sum[2]*invW;
                        if (dtemp2x && deluxeFloats) {
                            float dx = dsum[0]*invW, dy = dsum[1]*invW, dz = dsum[2]*invW;
                            float dlen = sqrtf(dx*dx+dy*dy+dz*dz);
                            if (dlen > 0.001f) { deluxeFloats[p1*3+0]=dx/dlen; deluxeFloats[p1*3+1]=dy/dlen; deluxeFloats[p1*3+2]=dz/dlen; }
                        }
                        if (ntemp2x && normalFloats) {
                            float nx = nsum[0]*invW, ny = nsum[1]*invW, nz = nsum[2]*invW;
                            float nlen = sqrtf(nx*nx+ny*ny+nz*nz);
                            if (nlen > 0.001f) { normalFloats[p1*3+0]=nx/nlen; normalFloats[p1*3+1]=ny/nlen; normalFloats[p1*3+2]=nz/nlen; }
                        }
                    }
                }
            }
        }
        free(temp2x);
        if (dtemp2x) free(dtemp2x);
        if (ntemp2x) free(ntemp2x);
        free(mask2x);
    }
}
