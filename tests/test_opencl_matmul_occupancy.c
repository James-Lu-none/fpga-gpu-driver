#include <CL/cl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MATRIX_DIM 4
#define BUFFER_SIZE 4096
#define NUM_CASES 3

static const char *kernel_source =
"__kernel void matmul(__global const int *A, __global const int *B, __global int *C, int N) {\n"
"    int col = get_global_id(0);\n"
"    int row = get_global_id(1);\n"
"    if (row < N && col < N) {\n"
"        int sum = 0;\n"
"        for (int k = 0; k < 4; k++) {\n"
"            sum += A[row * N + k] * B[k * N + col];\n"
"        }\n"
"        C[row * N + col] = sum;\n"
"    }\n"
"}\n";

static int check_error(cl_int err, const char *operation) {
    if (err != CL_SUCCESS) {
        fprintf(stderr, "%s failed (err=%d)\n", operation, err);
        return 0;
    }
    return 1;
}

int main(void) {
    // These cases use local=(2,1), so they dispatch 4, 8, and 16 blocks.
    const size_t global_sizes[NUM_CASES][2] = {
        {4, 1},
        {4, 2},
        {4, 4},
        {4, 8},
        {4, 16},
        {8, 16},
    };
    const char *case_names[NUM_CASES] = {
        "4 blocks (below one-SM occupancy)",
        "8 blocks (one-SM occupancy boundary)",
        "16 blocks (multi-SM / recycle pressure)",
    };

    int32_t *host_a = NULL;
    int32_t *host_b = NULL;
    int32_t *host_c = NULL;
    if (posix_memalign((void **)&host_a, 4096, BUFFER_SIZE) != 0 ||
        posix_memalign((void **)&host_b, 4096, BUFFER_SIZE) != 0 ||
        posix_memalign((void **)&host_c, 4096, BUFFER_SIZE) != 0) {
        perror("posix_memalign failed");
        return 1;
    }

    memset(host_a, 0, BUFFER_SIZE);
    memset(host_b, 0, BUFFER_SIZE);
    memset(host_c, 0, BUFFER_SIZE);
    for (int i = 0; i < MATRIX_DIM * MATRIX_DIM; i++) {
        host_a[i] = (i % 7) + 1;
        host_b[i] = (i % 5) + 2;
    }

    cl_int err;
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;
    err = clGetPlatformIDs(1, &platform, NULL);
    if (!check_error(err, "clGetPlatformIDs")) return 1;
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL);
    if (!check_error(err, "clGetDeviceIDs")) return 1;

    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (!check_error(err, "clCreateContext")) return 1;
    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);
    if (!check_error(err, "clCreateCommandQueue")) return 1;

    cl_mem device_a = clCreateBuffer(context, CL_MEM_READ_ONLY, BUFFER_SIZE, NULL, &err);
    cl_mem device_b = clCreateBuffer(context, CL_MEM_READ_ONLY, BUFFER_SIZE, NULL, &err);
    cl_mem device_c = clCreateBuffer(context, CL_MEM_READ_WRITE, BUFFER_SIZE, NULL, &err);
    if (!check_error(err, "clCreateBuffer")) return 1;

    err = clEnqueueWriteBuffer(queue, device_a, CL_TRUE, 0, BUFFER_SIZE,
                               host_a, 0, NULL, NULL);
    if (!check_error(err, "write A")) return 1;
    err = clEnqueueWriteBuffer(queue, device_b, CL_TRUE, 0, BUFFER_SIZE,
                               host_b, 0, NULL, NULL);
    if (!check_error(err, "write B")) return 1;

    cl_program program = clCreateProgramWithSource(context, 1, &kernel_source,
                                                   NULL, &err);
    if (!check_error(err, "clCreateProgramWithSource")) return 1;
    err = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
    if (!check_error(err, "clBuildProgram")) return 1;
    cl_kernel kernel = clCreateKernel(program, "matmul", &err);
    if (!check_error(err, "clCreateKernel")) return 1;

    int32_t dimension = MATRIX_DIM;
    clSetKernelArg(kernel, 0, sizeof(device_a), &device_a);
    clSetKernelArg(kernel, 1, sizeof(device_b), &device_b);
    clSetKernelArg(kernel, 2, sizeof(device_c), &device_c);
    clSetKernelArg(kernel, 3, sizeof(dimension), &dimension);

    int failures = 0;
    for (int test_case = 0; test_case < NUM_CASES; test_case++) {
        printf("[Case %d/%d] %s, Grid=(%zu,%zu), Block=(2,1)\n",
               test_case + 1, NUM_CASES, case_names[test_case],
               global_sizes[test_case][0], global_sizes[test_case][1]);

        memset(host_c, 0, BUFFER_SIZE);
        err = clEnqueueWriteBuffer(queue, device_c, CL_TRUE, 0, BUFFER_SIZE,
                                   host_c, 0, NULL, NULL);
        if (!check_error(err, "clear C")) {
            failures++;
            continue;
        }

        size_t local_size[2] = {2, 1};
        err = clEnqueueNDRangeKernel(queue, kernel, 2, NULL,
                                     global_sizes[test_case], local_size,
                                     0, NULL, NULL);
        if (!check_error(err, "clEnqueueNDRangeKernel")) {
            failures++;
            continue;
        }
        err = clFinish(queue);
        if (!check_error(err, "clFinish")) {
            failures++;
            continue;
        }
        err = clEnqueueReadBuffer(queue, device_c, CL_TRUE, 0, BUFFER_SIZE,
                                  host_c, 0, NULL, NULL);
        if (!check_error(err, "read C")) {
            failures++;
            continue;
        }

        int case_failures = 0;
        for (size_t row = 0; row < global_sizes[test_case][1]; row++) {
            for (size_t col = 0; col < global_sizes[test_case][0]; col++) {
                int32_t expected = 0;
                for (int k = 0; k < MATRIX_DIM; k++) {
                    expected += host_a[row * MATRIX_DIM + k] *
                                host_b[k * MATRIX_DIM + col];
                }
                int index = (int)(row * MATRIX_DIM + col);
                if (host_c[index] != expected) {
                    printf("  FAIL row=%zu col=%zu actual=%d expected=%d\n",
                           row, col, host_c[index], expected);
                    case_failures++;
                }
            }
        }
        printf("  %s (%d mismatches)\n",
               case_failures == 0 ? "PASS" : "FAIL", case_failures);
        failures += case_failures;
    }

    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseMemObject(device_a);
    clReleaseMemObject(device_b);
    clReleaseMemObject(device_c);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
    free(host_a);
    free(host_b);
    free(host_c);

    printf("Occupancy stress test: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
