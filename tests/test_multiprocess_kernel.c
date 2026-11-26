#define _POSIX_C_SOURCE 200809L
#define CL_TARGET_OPENCL_VERSION 120
#include <stdint.h>
#include <string.h>
#include "multiprocess_test.h"
#include "../umd/src/fpgagpu_umd.h"
#include "kernel_source.h"

/* Use the project's real LLVM backend, then bypass UMD dispatch/allocation
 * to control LOAD_KERNEL and SUBMIT_CMD independently. The UMD program
 * object is intentionally inspected to obtain the compiled ISA words. */
#define N 32u
#define WORDS 1024u
#define BYTES (WORDS * sizeof(uint32_t))
#define SENTINEL UINT32_C(0xdeadbeef)
static const char *sources[2] = {
    STRINGIFY(__kernel void isolation_a(__global const int *input, __global int *output) {
        int i = get_global_id(0); gpu_store32(&output[i], input[i] + 1);
    }),
    STRINGIFY(__kernel void isolation_b(__global const int *input, __global int *output) {
        int i = get_global_id(0); gpu_store32(&output[i], input[i] + 100);
    })
};

struct state {
    unsigned id;
    cl_context ctx;
    cl_program program;
    uint32_t *buf;
    uint64_t input, output;
};

static uint32_t input_value(unsigned id, unsigned i)
{
    return 1000u + id * 1000u + i * 3u;
}

static int dma(struct state *s, unsigned dir, uint64_t addr)
{
    struct fpgagpu_dma_param p = {
        .direction = dir, .size = BYTES, .ddr3_addr = addr,
        .host_vaddr = (uintptr_t)s->buf
    };
    if (ioctl(s->ctx->fd, FPGAGPU_IOC_DMA_TRANSFER, &p) < 0) {
        perror("DMA_TRANSFER"); return -1;
    }
    return 0;
}

static int init(struct state *s)
{
    cl_platform_id platform = NULL;
    cl_device_id device = NULL;
    cl_int err;
    if (posix_memalign((void **)&s->buf, 4096, BYTES) ||
        clGetPlatformIDs(1, &platform, NULL) != CL_SUCCESS ||
        clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL) != CL_SUCCESS)
        return -1;
    s->ctx = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (!s->ctx || err != CL_SUCCESS) return -1;
    s->program = clCreateProgramWithSource(s->ctx, 1, &sources[s->id], NULL, &err);
    if (!s->program || err != CL_SUCCESS ||
        clBuildProgram(s->program, 1, &device, NULL, NULL, NULL) != CL_SUCCESS ||
        !s->program->binary_words || !s->program->num_instructions ||
        s->program->num_instructions > 1024) return -1;
    /* Distinct scratch VRAM per process, outside kernarg and allocator base. */
    s->input = UINT64_C(0x12000000) + s->id * UINT64_C(0x01000000);
    s->output = s->input + UINT64_C(0x1000);
    for (unsigned i = 0; i < WORDS; ++i)
        s->buf[i] = i < N ? input_value(s->id, i) : SENTINEL;
    if (dma(s, FPGAGPU_DMA_TO_DEVICE, s->input) ||
        dma(s, FPGAGPU_DMA_FROM_DEVICE, s->input)) return -1;
    for (unsigned i = 0; i < WORDS; ++i)
        if (s->buf[i] != (i < N ? input_value(s->id, i) : SENTINEL)) {
            fprintf(stderr, "FAIL input DMA precheck process %c word %u\n", 'A' + s->id, i);
            return -1;
        }
    return 0;
}

static int load(struct state *s)
{
    struct fpgagpu_kernel_binary kbin = {
        .user_instr_ptr = (uintptr_t)s->program->binary_words,
        .instr_size = s->program->num_instructions,
        .kernel_id = s->id + 1u
    };
    if (ioctl(s->ctx->fd, FPGAGPU_IOC_LOAD_KERNEL, &kbin) < 0) {
        perror("LOAD_KERNEL"); return -1;
    }
    return 0;
}

static int dispatch(struct state *s)
{
    /* Reset output and restore this worker's arguments immediately before
     * dispatch. No competing DMA or kernarg update is allowed in this test. */
    for (unsigned i = 0; i < WORDS; ++i) s->buf[i] = SENTINEL;
    if (dma(s, FPGAGPU_DMA_TO_DEVICE, s->output) ||
        dma(s, FPGAGPU_DMA_FROM_DEVICE, s->output)) return -1;
    for (unsigned i = 0; i < WORDS; ++i)
        if (s->buf[i] != SENTINEL) {
            fprintf(stderr, "FAIL output sentinel precheck process %c word %u\n", 'A' + s->id, i);
            return -1;
        }

    /* Same kernarg ABI as cl_enqueue.c: each 32-bit argument is duplicated
     * into an eight-byte slot. The shared kernarg base is intentionally used
     * serially so instruction ownership is the independent variable. */
    memset(s->buf, 0, BYTES);
    s->buf[0] = s->buf[1] = (uint32_t)s->input;
    s->buf[2] = s->buf[3] = (uint32_t)s->output;
    if (dma(s, FPGAGPU_DMA_TO_DEVICE, FPGAGPU_KERNARG_BASE)) return -1;

    struct fpgagpu_launch_cmd cmd = {
        .opcode = FPGAGPU_OPCODE_LAUNCH_KERNEL,
        .grid_dim_x = N / 16u, .grid_dim_y = 1,
        .block_dim_x = 16, .block_dim_y = 1,
        .dma_src_addr = FPGAGPU_KERNARG_BASE, .dma_dst_addr = 0,
        .num_elements = N
    };
    if (ioctl(s->ctx->fd, FPGAGPU_IOC_SUBMIT_CMD, &cmd) < 0) {
        perror("SUBMIT_CMD"); return -1;
    }
    if (ioctl(s->ctx->fd, FPGAGPU_IOC_DOORBELL, 0) < 0) {
        perror("DOORBELL"); return -1;
    }
    if (dma(s, FPGAGPU_DMA_FROM_DEVICE, s->output)) return -1;
    unsigned mismatches = 0, other_matches = 0, guards = 0, first = 0;
    for (unsigned i = 0; i < N; ++i) {
        uint32_t value = input_value(s->id, i);
        if (s->buf[i] != value + (s->id ? 100u : 1u)) {
            if (!mismatches) first = i;
            ++mismatches;
        }
        if (s->buf[i] == value + (s->id ? 1u : 100u)) ++other_matches;
    }
    for (unsigned i = N; i < WORDS; ++i)
        if (s->buf[i] != SENTINEL) ++guards;
    if (mismatches || guards) {
        fprintf(stderr, "FAIL process %c: payload mismatches=%u/%u other-kernel matches=%u/%u guard mismatches=%u\n",
                'A' + s->id, mismatches, N, other_matches, N, guards);
        if (mismatches)
            fprintf(stderr, "first mismatch word %u expected=%u actual=%u\n", first,
                    input_value(s->id, first) + (s->id ? 100u : 1u), s->buf[first]);
        return -1;
    }
    printf("PASS process %c: own kernel output and guards verified\n", 'A' + s->id);
    return 0;
}

static int worker(unsigned id, int control)
{
    struct state s = { .id = id };
    int failed = 1;
    char cmd;
    while (!mp_recv(control, &cmd)) {
        int rc;
        switch (cmd) {
        case 'I': rc = init(&s); break;
        case 'L': rc = s.program ? load(&s) : -1; break;
        case 'D': rc = s.program ? dispatch(&s) : -1; break;
        case 'Q': failed = 0; goto out;
        default: goto out;
        }
        if (rc) fprintf(stderr, "FAIL process %c command %c\n", 'A' + id, cmd);
        fflush(NULL);
        if (mp_send(control, rc ? 'N' : 'Y') || rc) goto out;
    }
out:
    if (s.program) clReleaseProgram(s.program);
    if (s.ctx) clReleaseContext(s.ctx);
    free(s.buf);
    if (!failed) {
        fflush(NULL);
        if (mp_send(control, 'Y')) failed = 1;
    }
    return failed;
}

int main(void)
{
    struct mp_worker w[2] = {{-1, -1}, {-1, -1}};
    puts("Multiprocess kernel instruction isolation: deterministic interleaving");
    int failed = mp_start(w, worker) ||
        mp_step(&w[0], 'I', "A: compile kernel A and verify input") ||
        mp_step(&w[1], 'I', "B: compile kernel B and verify input") ||
        mp_step(&w[0], 'L', "A: baseline LOAD_A") ||
        mp_step(&w[0], 'D', "A: baseline DISPATCH_A") ||
        mp_step(&w[1], 'L', "B: baseline LOAD_B") ||
        mp_step(&w[1], 'D', "B: baseline DISPATCH_B") ||
        mp_step(&w[0], 'L', "A: LOAD_A") ||
        mp_step(&w[1], 'L', "B: LOAD_B before A dispatch") ||
        mp_step(&w[0], 'D', "A: DISPATCH_A without reloading (must execute A)") ||
        mp_step(&w[1], 'L', "B: LOAD_B") ||
        mp_step(&w[0], 'L', "A: LOAD_A before B dispatch") ||
        mp_step(&w[1], 'D', "B: DISPATCH_B without reloading (must execute B)");
    failed = mp_finish(w, failed);
    puts(failed ? "FAIL: multiprocess kernel instruction isolation" :
                  "PASS: multiprocess kernel instruction isolation");
    return failed;
}
