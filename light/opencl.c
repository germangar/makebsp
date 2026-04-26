#include "light.h"
#include <stdio.h>
#include <stdlib.h>

cl_platform_id g_clPlatform;
cl_device_id g_clDevice;
cl_context g_clContext;
cl_command_queue g_clQueue;

qboolean useOpenCL = qfalse;
qboolean openclEnabled = qtrue;

/*
================
InitOpenCL
================
*/
void InitOpenCL(void) {
    cl_uint numPlatforms;
    cl_int err;
    char platformName[128];
    char deviceName[128];

    _printf("--- InitOpenCL ---\n");
    useOpenCL = qfalse;

    // Get first platform
    err = clGetPlatformIDs(1, &g_clPlatform, &numPlatforms);
    if (err != CL_SUCCESS || numPlatforms == 0) {
        _printf("No OpenCL platforms found.\n");
        return;
    }

    clGetPlatformInfo(g_clPlatform, CL_PLATFORM_NAME, sizeof(platformName), platformName, NULL);
    _printf("Platform: %s\n", platformName);

    // Get first GPU device
    err = clGetDeviceIDs(g_clPlatform, CL_DEVICE_TYPE_GPU, 1, &g_clDevice, NULL);
    if (err != CL_SUCCESS) {
        // Fallback to any device if no GPU
        err = clGetDeviceIDs(g_clPlatform, CL_DEVICE_TYPE_ALL, 1, &g_clDevice, NULL);
    }

    if (err != CL_SUCCESS) {
        _printf("No OpenCL devices found.\n");
        return;
    }

    clGetDeviceInfo(g_clDevice, CL_DEVICE_NAME, sizeof(deviceName), deviceName, NULL);
    _printf("Device:   %s\n", deviceName);

    // Create context
    g_clContext = clCreateContext(NULL, 1, &g_clDevice, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        _printf("Failed to create OpenCL context.\n");
        return;
    }

    // Create command queue
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

    if (g_clQueue) {
        clReleaseCommandQueue(g_clQueue);
        g_clQueue = NULL;
    }
    if (g_clContext) {
        clReleaseContext(g_clContext);
        g_clContext = NULL;
    }

    useOpenCL = qfalse;
}

/*
================
BuildOpenCLProgram
================
*/
cl_program BuildOpenCLProgram(const char *filename, const char *options) {
    cl_program program;
    cl_int err;
    char *source;
    int fileSize;
    size_t size;
    char fullPath[MAX_OS_PATH];

    if (!useOpenCL) return NULL;

    sprintf(fullPath, "kernels/%s", filename);
    fileSize = LoadFile(fullPath, (void **)&source);
    if (fileSize <= 0) {
        _printf("BuildOpenCLProgram: Could not load %s\n", fullPath);
        return NULL;
    }

    size = (size_t)fileSize;

    // Create program from source
    program = clCreateProgramWithSource(g_clContext, 1, (const char **)&source, &size, &err);
    free(source);

    if (err != CL_SUCCESS) {
        _printf("BuildOpenCLProgram: Failed to create program from %s\n", filename);
        return NULL;
    }

    // Build program
    err = clBuildProgram(program, 1, &g_clDevice, options, NULL, NULL);
    if (err != CL_SUCCESS) {
        char *log;
        size_t logSize;

        _printf("BuildOpenCLProgram: Failed to build %s\n", filename);

        // Get build log
        clGetProgramBuildInfo(program, g_clDevice, CL_PROGRAM_BUILD_LOG, 0, NULL, &logSize);
        log = malloc(logSize);
        clGetProgramBuildInfo(program, g_clDevice, CL_PROGRAM_BUILD_LOG, logSize, log, NULL);
        _printf("Build Log:\n%s\n", log);
        free(log);

        clReleaseProgram(program);
        return NULL;
    }

    return program;
}

