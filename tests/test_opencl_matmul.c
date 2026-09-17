#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <CL/cl.h>

// A 4x4 problem creates 16 work-items and accesses multiple cache lines.
#define MATRIX_DIM 4

static const char *kernel_source =
"__kernel void matmul(__global const int *A, __global const int *B, __global int *C, int N) {\n"
"    int col = get_global_id(0);\n"
"    int row = get_global_id(1);\n"
"    int sum = 0;\n"
"    #pragma unroll\n"
"    for (int k = 0; k < 4; k++) {\n"
"        sum += A[row * N + k] * B[k * N + col];\n"
"    }\n"
"    C[row * N + col] = sum;\n"
"}\n";

int main(void) {

    cl_int err;
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;

    clGetPlatformIDs(1, &platform, NULL);
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL);

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

    // Step 1: Initialize Host Matrices (2x2)
    int32_t *h_A = NULL;
    int32_t *h_B = NULL;
    int32_t *h_C = NULL;

    if (posix_memalign((void **)&h_A, 4096, 4096) != 0 ||
        posix_memalign((void **)&h_B, 4096, 4096) != 0 ||
        posix_memalign((void **)&h_C, 4096, 4096) != 0) {
        perror("posix_memalign failed");
        return 1;
    }

    memset(h_A, 0, 4096);
    memset(h_B, 0, 4096);
    memset(h_C, 0, 4096);

    // Fill both matrices with deterministic non-zero data. Keeping values
    // small makes failures easy to inspect while exercising every 4x4 entry.
    for (int i = 0; i < MATRIX_DIM * MATRIX_DIM; i++) {
        h_A[i] = (i % 7) + 1;
        h_B[i] = (i % 5) + 2;
    }

    // Build the CPU golden result with the same general N x N algorithm used
    // by the device kernel, rather than relying on a hand-written 2x2 result.
    int32_t expected[MATRIX_DIM * MATRIX_DIM];
    for (int r = 0; r < MATRIX_DIM; r++) {
        for (int c = 0; c < MATRIX_DIM; c++) {
            int sum = 0;
            for (int k = 0; k < MATRIX_DIM; k++) {
                sum += h_A[r * MATRIX_DIM + k] * h_B[k * MATRIX_DIM + c];
            }
            expected[r * MATRIX_DIM + c] = sum;
        }
    }

    // Step 2: Allocate Device Buffers
    cl_mem d_A = clCreateBuffer(context, CL_MEM_READ_ONLY, 4096, NULL, NULL);
    cl_mem d_B = clCreateBuffer(context, CL_MEM_READ_ONLY, 4096, NULL, NULL);
    cl_mem d_C = clCreateBuffer(context, CL_MEM_WRITE_ONLY, 4096, NULL, NULL);

    clEnqueueWriteBuffer(queue, d_A, CL_TRUE, 0, 4096, h_A, 0, NULL, NULL);
    clEnqueueWriteBuffer(queue, d_B, CL_TRUE, 0, 4096, h_B, 0, NULL, NULL);
    clEnqueueWriteBuffer(queue, d_C, CL_TRUE, 0, 4096, h_C, 0, NULL, NULL);

    // Step 3: Compile OpenCL C Kernel
    printf("[Step 2] Compiling 2D Matrix Multiplication Kernel...\n");
    cl_program program = clCreateProgramWithSource(context, 1, &kernel_source, NULL, NULL);
    err = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clBuildProgram failed\n");
        return 1;
    }

    // Step 4: Create Kernel and Set Arguments
    cl_kernel kernel = clCreateKernel(program, "matmul", NULL);
    int32_t dim = MATRIX_DIM;
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_A);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_B);
    clSetKernelArg(kernel, 2, sizeof(cl_mem), &d_C);
    clSetKernelArg(kernel, 3, sizeof(int32_t), &dim);

    // Step 5: Launch 2D NDRange Kernel
    printf("[Step 3] Launching 2D NDRange Kernel (%dx%d threads)...\n", MATRIX_DIM, MATRIX_DIM);
    size_t global_work_size[2] = {MATRIX_DIM, MATRIX_DIM};
    size_t local_work_size[2]  = {2, 1};

    err = clEnqueueNDRangeKernel(queue, kernel, 2, NULL,
                                 global_work_size, local_work_size,
                                 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clEnqueueNDRangeKernel failed\n");
        return 1;
    }

    // Step 6: Synchronize and Read Back
    clFinish(queue);
    clEnqueueReadBuffer(queue, d_C, CL_TRUE, 0, 4096, h_C, 0, NULL, NULL);

    // Step 7: Verification
    printf("\n[Step 4] Verification Matrix C = A x B:\n");
    printf(" Row, Col | Actual Result | Expected Result | Status\n");
    int mismatches = 0;
    for (int r = 0; r < MATRIX_DIM; r++) {
        for (int c = 0; c < MATRIX_DIM; c++) {
            int idx = r * MATRIX_DIM + c;
            int pass = (h_C[idx] == expected[idx]);
            if (!pass) mismatches++;
            printf(" (%d, %d)   | %13d | %15d | %s\n",
                   r, c, h_C[idx], expected[idx], pass ? "PASS" : "FAIL ***");
        }
    }

    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseMemObject(d_A);
    clReleaseMemObject(d_B);
    clReleaseMemObject(d_C);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
    free(h_A);
    free(h_B);
    free(h_C);

    if (mismatches == 0) {
        printf("\n>>> SUCCESS! Matrix Multiplication verified! <<<\n\n");
        return 0;
    } else {
        printf("\n>>> FAILURE: %d mismatches found! <<<\n\n", mismatches);
        return 1;
    }
}
