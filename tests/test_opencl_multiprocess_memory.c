#define _POSIX_C_SOURCE 200809L
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <stdint.h>
#include <string.h>
#include "multiprocess_test.h"

#define BYTES 4096

static uint8_t pattern(unsigned id, size_t i)
{
    return (uint8_t)(i * 13u + (i >> 8) + (id ? 0x97u : 0x31u));
}

static int check_cl(cl_int rc, unsigned id, const char *op)
{
    if (rc == CL_SUCCESS) return 0;
    fprintf(stderr, "FAIL process %c: %s returned %d\n", 'A' + id, op, rc);
    return -1;
}

static int worker(unsigned id, int control)
{
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;
    cl_context ctx = NULL;
    cl_command_queue queue = NULL;
    cl_mem mem = NULL;
    uint8_t *buf = NULL;
    int failed = 1;
    char cmd;
    while (!mp_recv(control, &cmd)) {
        int rc = 0;
        cl_int err;
        switch (cmd) {
        case 'I':
            if (ctx || posix_memalign((void **)&buf, 4096, BYTES) ||
                check_cl(clGetPlatformIDs(1, &platform, NULL), id, "platform") ||
                check_cl(clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL),
                         id, "device")) goto out;
            ctx = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
            if (check_cl(err, id, "clCreateContext") || !ctx) goto out;
            queue = clCreateCommandQueue(ctx, device, 0, &err);
            if (check_cl(err, id, "clCreateCommandQueue") || !queue) goto out;
            mem = clCreateBuffer(ctx, CL_MEM_READ_WRITE, BYTES, NULL, &err);
            if (check_cl(err, id, "clCreateBuffer") || !mem) goto out;
            break;
        case 'W':
            if (!mem) goto out;
            for (size_t i = 0; i < BYTES; ++i) buf[i] = pattern(id, i);
            rc = check_cl(clEnqueueWriteBuffer(queue, mem, CL_TRUE, 0, BYTES,
                                             buf, 0, NULL, NULL), id, "write");
            if (!rc) rc = check_cl(clFinish(queue), id, "finish write");
            break;
        case 'R': {
            if (!mem) goto out;
            memset(buf, 0, BYTES);
            rc = check_cl(clEnqueueReadBuffer(queue, mem, CL_TRUE, 0, BYTES,
                                            buf, 0, NULL, NULL), id, "read");
            if (!rc) rc = check_cl(clFinish(queue), id, "finish read");
            if (rc) break;
            size_t mismatches = 0, other_matches = 0, first = 0;
            for (size_t i = 0; i < BYTES; ++i) {
                if (buf[i] != pattern(id, i)) {
                    if (!mismatches) first = i;
                    ++mismatches;
                }
                if (buf[i] == pattern(id ^ 1u, i)) ++other_matches;
            }
            if (mismatches) {
                fprintf(stderr, "FAIL process %c: %zu/%u bytes differ; first byte %zu expected=%02x actual=%02x; other process pattern matches %zu/%u\n",
                        'A' + id, mismatches, BYTES, first,
                        (unsigned)pattern(id, first), (unsigned)buf[first],
                        other_matches, BYTES);
                rc = -1;
            } else printf("PASS process %c: all %u bytes match own pattern\n", 'A' + id, BYTES);
            break;
        }
        case 'Q':
            failed = 0;
            goto out;
        default:
            goto out;
        }
        fflush(NULL);
        if (mp_send(control, rc ? 'N' : 'Y') || rc) goto out;
    }
out:
    if (mem) clReleaseMemObject(mem);
    if (queue) clReleaseCommandQueue(queue);
    if (ctx) clReleaseContext(ctx);
    free(buf);
    if (!failed) {
        fflush(NULL);
        if (mp_send(control, 'Y')) failed = 1;
    }
    return failed;
}

int main(void)
{
    struct mp_worker w[2] = {{-1, -1}, {-1, -1}};
    puts("Multiprocess VRAM ownership: all DMA operations are serialized");
    int failed = mp_start(w, worker) ||
        mp_step(&w[0], 'I', "A: allocate buffer") ||
        mp_step(&w[0], 'W', "A: write pattern A") ||
        mp_step(&w[0], 'R', "A: baseline readback") ||
        mp_step(&w[1], 'I', "B: allocate while A remains live") ||
        mp_step(&w[1], 'W', "B: write pattern B") ||
        mp_step(&w[1], 'R', "B: baseline readback") ||
        mp_step(&w[0], 'R', "A: verify ownership after B write") ||
        mp_step(&w[1], 'R', "B: verify ownership");
    failed = mp_finish(w, failed);
    puts(failed ? "FAIL: multiprocess VRAM ownership" :
                  "PASS: multiprocess VRAM ownership");
    return failed;
}
