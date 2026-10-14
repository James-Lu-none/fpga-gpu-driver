#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <CL/cl.h>

#define NUM_ELEMENTS 32
#include "kernel_source.h"
#include "raw_data.h"

// Adjacent work-items have opposite signs, so every two-lane warp diverges.
// Volatile gpu_store32 intrinsics preserve the two branch paths during optimization.
static const char *kernel_source = STRINGIFY(
    __kernel void branch_divergence(__global const int *input,
                                   __global int *output) {
        int id = get_global_id(0);
        int value = input[id];
        if (value < 0) {
            gpu_store32(&output[id], value * 3 + 7);
        } else {
            gpu_store32(&output[id], value * 5 - 9);
        }
    }
);

int main(void) {

    cl_int err;
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;

    if (clGetPlatformIDs(1, &platform, NULL) != CL_SUCCESS) return 1;
    if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL) != CL_SUCCESS) return 1;

    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateContext failed\n");
        return 1;
    }

    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateCommandQueue failed\n");
        return 1;
    }

    int32_t h_A[NUM_ELEMENTS], h_C[NUM_ELEMENTS], expected[NUM_ELEMENTS];
    for (int i = 0; i < NUM_ELEMENTS; i++) {
        h_A[i] = i % 2 == 0 ? -(i + 1) : i + 1;
        h_C[i] = -999;
        expected[i] = h_A[i] < 0 ? h_A[i] * 3 + 7 : h_A[i] * 5 - 9;
    }
    if (write_raw("branch_cpu.bin", expected, NUM_ELEMENTS)) return 1;

    // Step 2: Allocate Device Buffers
    cl_mem d_A = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(h_A), NULL, NULL);
    cl_mem d_C = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(h_A), NULL, NULL);

    if (!d_A || !d_C) return 1;

    if (clEnqueueWriteBuffer(queue, d_A, CL_TRUE, 0, sizeof(h_A), h_A, 0, NULL, NULL) != CL_SUCCESS) return 1;
    if (clEnqueueWriteBuffer(queue, d_C, CL_TRUE, 0, sizeof(h_C), h_C, 0, NULL, NULL) != CL_SUCCESS) return 1;

    // Step 3: Compile OpenCL C Kernel
    cl_program program = clCreateProgramWithSource(context, 1, &kernel_source, NULL, NULL);
    err = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clBuildProgram failed\n");
        return 1;
    }

    // Step 4: Create Kernel and Set Arguments
    cl_kernel kernel = clCreateKernel(program, "branch_divergence", NULL);
    if (!kernel) return 1;
    if (clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_A) != CL_SUCCESS) return 1;
    if (clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_C) != CL_SUCCESS) return 1;

    // Step 5: Launch 1D NDRange Kernel
    size_t global_work_size[1] = {NUM_ELEMENTS};
    size_t local_work_size[1] = {2};

    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL,
                                 global_work_size, local_work_size,
                                 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clEnqueueNDRangeKernel failed\n");
        return 1;
    }

    // Step 6: Synchronize and Read Back
    if (clFinish(queue) != CL_SUCCESS) return 1;
    if (clEnqueueReadBuffer(queue, d_C, CL_TRUE, 0, sizeof(h_C), h_C, 0, NULL, NULL) != CL_SUCCESS) return 1;

    // Headerless little-endian int32 data; compare without parsing console text.
    if (write_raw("branch_gpu.bin", h_C, NUM_ELEMENTS)) return 1;
    int mismatches = memcmp(h_C, expected, sizeof(expected)) != 0;

    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseMemObject(d_A);
    clReleaseMemObject(d_C);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);

    return mismatches ? 1 : 0;
}
