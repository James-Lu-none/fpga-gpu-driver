#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <CL/cl.h>

#define TEST_SIZE 4096

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

    uint8_t *host_src = NULL;
    uint8_t *host_dst = NULL;
    if (posix_memalign((void **)&host_src, 4096, TEST_SIZE) != 0 ||
        posix_memalign((void **)&host_dst, 4096, TEST_SIZE) != 0) {
        perror("posix_memalign failed");
        return 1;
    }

    for (int i = 0; i < TEST_SIZE; i++) {
        host_src[i] = (uint8_t)(i * 13 + 7);
        host_dst[i] = 0;
    }

    printf("[Step 1] Allocating two device buffers (4KB each)...\n");
    cl_mem dev_buf1 = clCreateBuffer(context, CL_MEM_READ_WRITE, TEST_SIZE, NULL, &err);
    cl_mem dev_buf2 = clCreateBuffer(context, CL_MEM_READ_WRITE, TEST_SIZE, NULL, &err);

    printf("[Step 2] Writing host data into dev_buf1 (H2D DMA)...\n");
    clEnqueueWriteBuffer(queue, dev_buf1, CL_TRUE, 0, TEST_SIZE, host_src, 0, NULL, NULL);

    printf("[Step 3] Copying dev_buf1 to dev_buf2 (D2D Buffer Copy)...\n");
    clEnqueueCopyBuffer(queue, dev_buf1, dev_buf2, 0, 0, TEST_SIZE, 0, NULL, NULL);

    printf("[Step 4] Synchronizing with clFinish...\n");
    clFinish(queue);

    printf("[Step 5] Reading back from dev_buf2 to host_dst (D2H DMA)...\n");
    clEnqueueReadBuffer(queue, dev_buf2, CL_TRUE, 0, TEST_SIZE, host_dst, 0, NULL, NULL);

    printf("[Step 6] Verifying Data Integrity...\n");
    int mismatches = 0;
    for (int i = 0; i < TEST_SIZE; i++) {
        if (host_src[i] != host_dst[i])
            mismatches++;
    }

    clReleaseMemObject(dev_buf1);
    clReleaseMemObject(dev_buf2);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
    free(host_src);
    free(host_dst);

    if (mismatches == 0) {
        printf(">>> SUCCESS: 4KB Buffer Copy & Synchronization 100%% Matched! <<<\n\n");
        return 0;
    }

    printf(">>> FAILURE: %d byte mismatches detected! <<<\n\n", mismatches);
    return 1;
}
