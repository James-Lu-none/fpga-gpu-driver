#define _POSIX_C_SOURCE 200112L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../include/uapi/fpgagpu_ioctl.h"
#include "raw_data.h"

#define WORDS 1024
#define BYTES (WORDS * sizeof(uint32_t))
#define SOURCE_ADDR 0x0000u
#define OUTPUT_ADDR 0x1000u
#define SENTINEL 0xA5A5A5A5u

static int dma(int fd, uint32_t direction, uint32_t address, void *buffer)
{
    struct fpgagpu_dma_param p = {
        .direction = direction, .size = BYTES, .ddr3_addr = address,
        .host_vaddr = (uintptr_t)buffer,
    };
    if (ioctl(fd, FPGAGPU_IOC_DMA_TRANSFER, &p) < 0) { perror("DMA"); return 1; }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2 || (strcmp(argv[1], "store") && strcmp(argv[1], "copy") &&
                     strcmp(argv[1], "register") && strcmp(argv[1], "rs1-add"))) {
        fprintf(stderr, "Usage: %s store|copy|register|rs1-add\n", argv[0]);
        return 2;
    }
    const int copy = !strcmp(argv[1], "copy");
    const int reg_test = !strcmp(argv[1], "register");
    const int rs1_test = !strcmp(argv[1], "rs1-add");
    // Hand-encoded ISA: no compiler, kernarg, S2R, branch or multiply.
    // Both lanes use the same address/value: LSU performs one uniform store.
    const uint32_t store_code[] = {
        0x81081000, // ADDI R1, R0, 0x1000
        0x81101357, // ADDI R2, R0, 0x1357
        0xA1004400, // STR R2, [R1]
        0xFF000000, // EXIT
    };
    const uint32_t copy_code[] = {
        0xA0100000, // LDR R2, [R0] (DDR address 0)
        0x81081000, // ADDI R1, R0, 0x1000
        0xA1004400, // STR R2, [R1]
        0xFF000000, // EXIT
    };
    const uint32_t register_code[] = {
        0x81081000, // ADDI R1, R0, 0x1000
        0xA1000200, // STR R1, [R0]: read R1 through the store-data port
        0xA1004200, // STR R1, [R1]: read the same R1 through both operand ports
        0xFF000000, // EXIT
    };
    const uint32_t rs1_code[] = {
        0x81081000, // ADDI R1, R0, 0x1000
        0x01104000, // ADD R2, R1, R0: exercise rs1 through the ALU
        0xA1000400, // STR R2, [R0]: fixed destination, bypass variable LSU address
        0xFF000000, // EXIT
    };
    const uint32_t *code = rs1_test ? rs1_code : reg_test ? register_code : copy ? copy_code : store_code;
    _Alignas(4096) uint32_t input[WORDS], output[WORDS], expected[WORDS], scratch[WORDS];
    for (unsigned i = 0; i < WORDS; i++) {
        input[i] = 0x24681357u + i;
        output[i] = expected[i] = SENTINEL;
    }
    expected[0] = rs1_test ? SENTINEL : reg_test ? OUTPUT_ADDR : copy ? input[0] : 0x1357;
    printf("ISA %s: source=0x%08x output=0x%08x; four instructions, one block\n",
           argv[1], SOURCE_ADDR, OUTPUT_ADDR);
    if (rs1_test) puts("rs1-add result is written to address 0; the output page remains a guard");
    for (unsigned i = 0; i < 4; i++) printf("instruction[%u]=0x%08x\n", i, code[i]);
    int fd = open("/dev/fpgagpu0", O_RDWR);
    if (fd < 0) { perror("open /dev/fpgagpu0"); return 1; }
    int failed = 1;
    if (dma(fd, FPGAGPU_DMA_TO_DEVICE, SOURCE_ADDR, input) ||
        dma(fd, FPGAGPU_DMA_TO_DEVICE, OUTPUT_ADDR, output) ||
        dma(fd, FPGAGPU_DMA_FROM_DEVICE, SOURCE_ADDR, scratch)) goto cleanup;
    if (memcmp(input, scratch, BYTES)) { puts("FAIL: source DMA precheck"); goto cleanup; }
    if (dma(fd, FPGAGPU_DMA_FROM_DEVICE, OUTPUT_ADDR, scratch)) goto cleanup;
    if (memcmp(output, scratch, BYTES)) { puts("FAIL: output DMA precheck"); goto cleanup; }
    puts("PASS: DMA precheck; submitting hand-encoded kernel");
    struct fpgagpu_kernel_binary binary = {
        .user_instr_ptr = (uintptr_t)code, .instr_size = 4, .kernel_id = 1,
    };
    struct fpgagpu_launch_cmd command = {
        .opcode = FPGAGPU_OPCODE_LAUNCH_KERNEL,
        .grid_dim_x = 1, .grid_dim_y = 1, .block_dim_x = 2, .block_dim_y = 1,
        .num_elements = 2,
    };
    if (ioctl(fd, FPGAGPU_IOC_LOAD_KERNEL, &binary) < 0) { perror("LOAD_KERNEL"); goto cleanup; }
    if (ioctl(fd, FPGAGPU_IOC_SUBMIT_CMD, &command) < 0) { perror("SUBMIT_CMD"); goto cleanup; }
    if (ioctl(fd, FPGAGPU_IOC_DOORBELL, 0) < 0) { perror("DOORBELL"); goto cleanup; }
    if (dma(fd, FPGAGPU_DMA_FROM_DEVICE, OUTPUT_ADDR, output) ||
        dma(fd, FPGAGPU_DMA_FROM_DEVICE, SOURCE_ADDR, scratch)) goto cleanup;
    if (reg_test || rs1_test) input[0] = OUTPUT_ADDR; // These cases intentionally write address 0.
    printf("out[0]: expected=0x%08x actual=0x%08x%s\n", expected[0], output[0],
           output[0] == SENTINEL ? " (sentinel unchanged)" : "");
    printf("source[0]: expected=0x%08x actual=0x%08x\n", input[0], scratch[0]);
    unsigned mismatches = 0;
    for (unsigned i = 0; i < WORDS; i++) {
        if (output[i] != expected[i] || scratch[i] != input[i]) {
            if (mismatches < 4)
                printf("FAIL: word %u output=0x%08x expected=0x%08x source=0x%08x expected=0x%08x\n",
                       i, output[i], expected[i], scratch[i], input[i]);
            mismatches++;
        }
    }
    char file[96];
    snprintf(file, sizeof(file), "isa_%s_cpu.bin", argv[1]);
    if (write_raw(file, (int32_t *)expected, WORDS)) goto cleanup;
    snprintf(file, sizeof(file), "isa_%s_gpu.bin", argv[1]);
    if (write_raw(file, (int32_t *)output, WORDS)) goto cleanup;
    snprintf(file, sizeof(file), "isa_%s_source_cpu.bin", argv[1]);
    if (write_raw(file, (int32_t *)input, WORDS)) goto cleanup;
    snprintf(file, sizeof(file), "isa_%s_source_gpu.bin", argv[1]);
    if (write_raw(file, (int32_t *)scratch, WORDS)) goto cleanup;
    failed = mismatches != 0;
    printf("%s: ISA %s; %u mismatches including source/output guards\n",
           failed ? "FAIL" : "PASS", argv[1], mismatches);
cleanup:
    close(fd);
    return failed;
}
