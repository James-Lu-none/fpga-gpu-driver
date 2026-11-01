#define _POSIX_C_SOURCE 200112L
#include <CL/cl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel_source.h"
#include "raw_data.h"

#define WORDS 1024
#define BYTES (WORDS * sizeof(int32_t))
#define SENTINEL ((int32_t)0xA5A5A5A5u)

static const struct {
    const char *name;
    const char *source;
} cases[] = {
    // Two-lane baseline: select one writer for fixed-address tests, avoiding a
    // data race without using thread ID to calculate the destination address.
    {"constant", STRINGIFY(__kernel void smoke(__global int *out) {
        if (get_local_id(0) == 0) out[0] = 0x1357;
    })},
    {"scalar", STRINGIFY(__kernel void smoke(__global int *out, int value) {
        if (get_local_id(0) == 0) out[0] = value;
    })},
    {"copy", STRINGIFY(__kernel void smoke(__global const int *input, __global int *out) {
        if (get_local_id(0) == 0) out[0] = input[0];
    })},
    {"thread-id", STRINGIFY(__kernel void smoke(__global int *out) {
        int id = get_local_id(0);
        out[id] = id;
    })},
};

#define CHECK(call) do { \
    err = (call); \
    if (err != CL_SUCCESS) { \
        fprintf(stderr, "FAIL: %s (OpenCL error=%d)\n", #call, err); \
        goto cleanup; \
    } \
} while (0)

static int run_case(unsigned mode, cl_context context, cl_command_queue queue,
                    cl_mem input_mem, cl_mem output_mem)
{
    _Alignas(4096) int32_t input[WORDS], output[WORDS], expected[WORDS], scratch[WORDS];
    cl_program program = NULL;
    cl_kernel kernel = NULL;
    cl_int err;
    int failed = 1;
    const int32_t scalar = 0x12345678;
    const size_t global = 2, local = 2;
    const char *source = cases[mode].source;
    printf("\nMINIMAL: %s; one block, two lanes\n", cases[mode].name);
    for (unsigned i = 0; i < WORDS; i++) {
        input[i] = (int32_t)(0x24681357u + i);
        output[i] = expected[i] = SENTINEL;
    }
    expected[0] = mode == 0 ? 0x1357 : mode == 1 ? scalar : mode == 2 ? input[0] : 0;
    if (mode == 3) expected[1] = 1;

    CHECK(clEnqueueWriteBuffer(queue, input_mem, CL_TRUE, 0, BYTES, input, 0, NULL, NULL));
    CHECK(clEnqueueWriteBuffer(queue, output_mem, CL_TRUE, 0, BYTES, output, 0, NULL, NULL));
    CHECK(clEnqueueReadBuffer(queue, input_mem, CL_TRUE, 0, BYTES, scratch, 0, NULL, NULL));
    if (memcmp(input, scratch, BYTES)) {
        fprintf(stderr, "FAIL: input buffer DMA upload/readback mismatch before kernel launch\n");
        goto cleanup;
    }
    CHECK(clEnqueueReadBuffer(queue, output_mem, CL_TRUE, 0, BYTES, scratch, 0, NULL, NULL));
    if (memcmp(output, scratch, BYTES)) {
        fprintf(stderr, "FAIL: output sentinel DMA upload/readback mismatch before kernel launch\n");
        goto cleanup;
    }
    puts("PASS: input data and output sentinel verified in VRAM before launch");

    program = clCreateProgramWithSource(context, 1, &source, NULL, &err);
    if (!program || err != CL_SUCCESS) goto cleanup;
    CHECK(clBuildProgram(program, 0, NULL, NULL, NULL, NULL));
    kernel = clCreateKernel(program, "smoke", &err);
    if (!kernel || err != CL_SUCCESS) goto cleanup;
    if (mode == 2) {
        CHECK(clSetKernelArg(kernel, 0, sizeof(input_mem), &input_mem));
        CHECK(clSetKernelArg(kernel, 1, sizeof(output_mem), &output_mem));
    } else {
        CHECK(clSetKernelArg(kernel, 0, sizeof(output_mem), &output_mem));
        if (mode == 1) CHECK(clSetKernelArg(kernel, 1, sizeof(scalar), &scalar));
    }
    CHECK(clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global, &local, 0, NULL, NULL));
    CHECK(clFinish(queue));
    CHECK(clEnqueueReadBuffer(queue, output_mem, CL_TRUE, 0, BYTES, output, 0, NULL, NULL));

    char filename[96];
    snprintf(filename, sizeof(filename), "minimal_%s_cpu.bin", cases[mode].name);
    if (write_raw(filename, expected, WORDS)) goto cleanup;
    snprintf(filename, sizeof(filename), "minimal_%s_gpu.bin", cases[mode].name);
    if (write_raw(filename, output, WORDS)) goto cleanup;
    printf("out[0]: expected=0x%08x actual=0x%08x\n", (uint32_t)expected[0], (uint32_t)output[0]);
    printf("out[1]: expected=0x%08x actual=0x%08x\n", (uint32_t)expected[1], (uint32_t)output[1]);
    unsigned mismatches = 0;
    for (unsigned i = 0; i < WORDS; i++) {
        if (output[i] != expected[i]) {
            if (mismatches < 4)
                printf("FAIL: word %u expected=0x%08x actual=0x%08x%s\n", i,
                       (uint32_t)expected[i], (uint32_t)output[i],
                       output[i] == SENTINEL ? " (sentinel unchanged)" : "");
            mismatches++;
        }
    }
    failed = mismatches != 0;
    printf("%s: %s; %u mismatches, including untouched-buffer guards\n",
           failed ? "FAIL" : "PASS", cases[mode].name, mismatches);
cleanup:
    if (failed && err != CL_SUCCESS) fprintf(stderr, "OpenCL status: %d\n", err);
    if (kernel) clReleaseKernel(kernel);
    if (program) clReleaseProgram(program);
    return failed;
}

int main(int argc, char **argv)
{
    int selected = -1;
    if (argc > 2) { fprintf(stderr, "Usage: %s [constant|scalar|copy|thread-id]\n", argv[0]); return 2; }
    if (argc == 2) {
        for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++)
            if (!strcmp(argv[1], cases[i].name)) selected = (int)i;
        if (selected < 0) { fprintf(stderr, "Unknown minimal test: %s\n", argv[1]); return 2; }
    }
    cl_platform_id platform;
    cl_device_id device;
    cl_context context = NULL;
    cl_command_queue queue = NULL;
    cl_mem input = NULL, output = NULL;
    cl_int err = CL_SUCCESS;
    int failed = 1;
    CHECK(clGetPlatformIDs(1, &platform, NULL));
    CHECK(clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL));
    context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (!context || err != CL_SUCCESS) goto cleanup;
    queue = clCreateCommandQueue(context, device, 0, &err);
    if (!queue || err != CL_SUCCESS) goto cleanup;
    input = clCreateBuffer(context, CL_MEM_READ_ONLY, BYTES, NULL, &err);
    if (!input || err != CL_SUCCESS) goto cleanup;
    output = clCreateBuffer(context, CL_MEM_READ_WRITE, BYTES, NULL, &err);
    if (!output || err != CL_SUCCESS) goto cleanup;
    failed = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (selected >= 0 && (unsigned)selected != i) continue;
        if (run_case(i, context, queue, input, output)) failed = 1;
    }
cleanup:
    if (output) clReleaseMemObject(output);
    if (input) clReleaseMemObject(input);
    if (queue) clReleaseCommandQueue(queue);
    if (context) clReleaseContext(context);
    return failed;
}
