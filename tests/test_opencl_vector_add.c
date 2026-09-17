#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <CL/cl.h>

// 64 elements exercise multiple thread blocks, several cache lines, and the
// LSU request FIFO while keeping the test small enough for quick iteration.
#define NUM_ELEMENTS 64

static const char *kernel_source =
"__kernel void vector_add(__global const int *A, __global const int *B, __global int *C, int n) {\n"
"    int id = get_global_id(0);\n"
"    if (id < n) {\n"
"        C[id] = A[id] + B[id];\n"
"    }\n"
"}\n";

int main(void) {

    cl_int err;
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;
    cl_uint num_platforms = 0, num_devices = 0;

    // Step 1: Discover Platform and Device
    err = clGetPlatformIDs(1, &platform, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        fprintf(stderr, "Failed to find OpenCL platform (err=%d)\n", err);
        return 1;
    }

    char plat_name[128];
    clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(plat_name), plat_name, NULL);
    printf("[Step 1] Platform: %s\n", plat_name);

    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        fprintf(stderr, "Failed to find OpenCL device (err=%d)\n", err);
        return 1;
    }

    char dev_name[128];
    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(dev_name), dev_name, NULL);
    printf("         Device:   %s\n\n", dev_name);

    // Step 2: Create Context & Command Queue
    printf("[Step 2] Creating OpenCL Context and Command Queue...\n");
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (err != CL_SUCCESS || !context) {
        fprintf(stderr, "clCreateContext failed (err=%d)\n", err);
        return 1;
    }

    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);
    if (err != CL_SUCCESS || !queue) {
        fprintf(stderr, "clCreateCommandQueue failed (err=%d)\n", err);
        clReleaseContext(context);
        return 1;
    }

    // Step 3: Prepare Host Data and Buffers
    printf("[Step 3] Allocating Device Memory and Uploading Host Data...\n");
    int32_t *h_A = NULL;
    int32_t *h_B = NULL;
    int32_t *h_C = NULL;

    if (posix_memalign((void **)&h_A, 4096, 4096) != 0 ||
        posix_memalign((void **)&h_B, 4096, 4096) != 0 ||
        posix_memalign((void **)&h_C, 4096, 4096) != 0) {
        perror("posix_memalign failed");
        return 1;
    }

    int32_t expected[NUM_ELEMENTS];
    for (int i = 0; i < NUM_ELEMENTS; i++) {
        h_A[i] = (i + 1) * 10;
        h_B[i] = (i + 1) * 3;
        h_C[i] = 0;
        expected[i] = h_A[i] + h_B[i];
    }

    cl_mem d_A = clCreateBuffer(context, CL_MEM_READ_ONLY, 4096, NULL, &err);
    cl_mem d_B = clCreateBuffer(context, CL_MEM_READ_ONLY, 4096, NULL, &err);
    cl_mem d_C = clCreateBuffer(context, CL_MEM_WRITE_ONLY, 4096, NULL, &err);

    // Upload vectors A and B
    clEnqueueWriteBuffer(queue, d_A, CL_TRUE, 0, 4096, h_A, 0, NULL, NULL);
    clEnqueueWriteBuffer(queue, d_B, CL_TRUE, 0, 4096, h_B, 0, NULL, NULL);
    clEnqueueWriteBuffer(queue, d_C, CL_TRUE, 0, 4096, h_C, 0, NULL, NULL);
    printf("         Input vectors A & B transferred to VRAM via DMA.\n\n");

    // Step 4: Build Program from OpenCL C Source (JIT Compilation)
    printf("[Step 4] Compiling OpenCL C Kernel via LLVM Backend...\n");
    cl_program program = clCreateProgramWithSource(context, 1, &kernel_source, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateProgramWithSource failed (err=%d)\n", err);
        return 1;
    }

    err = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clBuildProgram failed (err=%d)\n", err);
        return 1;
    }

    // Step 5: Create Kernel and Set Kernel Arguments
    printf("[Step 5] Creating Kernel and Setting Arguments...\n");
    cl_kernel kernel = clCreateKernel(program, "vector_add", &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateKernel failed (err=%d)\n", err);
        return 1;
    }

    int32_t num_elements = NUM_ELEMENTS;
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_A);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_B);
    clSetKernelArg(kernel, 2, sizeof(cl_mem), &d_C);
    clSetKernelArg(kernel, 3, sizeof(int32_t), &num_elements);

    // Step 6: Launch Kernel (NDRange Dispatch)
    printf("[Step 6] Enqueuing NDRange Kernel onto GPU...\n");
    size_t global_work_size[1] = {NUM_ELEMENTS};
    size_t local_work_size[1]  = {2}; // 2 threads per block

    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL,
                                 global_work_size, local_work_size,
                                 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clEnqueueNDRangeKernel failed (err=%d)\n", err);
        return 1;
    }

    // Step 7: Wait for Completion & Read Back Results
    printf("[Step 7] Synchronizing Queue and Reading Back Output from VRAM...\n");
    clFinish(queue);

    err = clEnqueueReadBuffer(queue, d_C, CL_TRUE, 0, 4096, h_C, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clEnqueueReadBuffer failed (err=%d)\n", err);
        return 1;
    }

    // Step 8: Golden Verification
    printf("\n[Step 8] Verification against CPU Golden Model:\n");
    printf(" Thread ID | Vector A | Vector B | Output C | Expected | Status  \n");

    int mismatches = 0;
    for (int i = 0; i < NUM_ELEMENTS; i++) {
        int pass = (h_C[i] == expected[i]);
        if (!pass) mismatches++;
        printf(" Thread %2d | %8d | %8d | %8d | %8d | %s\n",
               i, h_A[i], h_B[i], h_C[i], expected[i], pass ? "PASS" : "FAIL ***");
    }

    // Cleanup
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
        printf("\n>>> SUCCESS! All %d elements computed correctly on FPGA-GPU! <<<\n\n", NUM_ELEMENTS);
        return 0;
    } else {
        printf("\n>>> FAILURE: %d mismatches found! <<<\n\n", mismatches);
        return 1;
    }
}
