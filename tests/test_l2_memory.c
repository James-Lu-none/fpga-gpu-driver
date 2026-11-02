#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "../include/uapi/fpgagpu_ioctl.h"
#include "raw_data.h"

#define WORDS 1024
#define SENTINEL 0xa5a5a5a5u
#define CTRL_OFFSET 0x20000u
#define MAP_SIZE 0x21000u
static int dma(int fd, uint32_t direction, uint32_t address, void *buffer)
{
    struct fpgagpu_dma_param p = {
        .direction = direction, .size = WORDS * sizeof(uint32_t),
        .ddr3_addr = address, .host_vaddr = (uintptr_t)buffer,
    };
    if (ioctl(fd, FPGAGPU_IOC_DMA_TRANSFER, &p) < 0) { perror("DMA"); return 1; }
    return 0;
}
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) { fprintf(stderr, "Usage: %s /sys/bus/pci/devices/BDF/resource0\n", argv[0]); return 2; }
    int bar = open(argv[1], O_RDWR | O_SYNC);
    if (bar < 0) { perror("open BAR0"); return 1; }
    void *mapping = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, bar, 0);
    close(bar);
    if (mapping == MAP_FAILED) { perror("mmap BAR0"); return 1; }
    volatile uint32_t *csr = (volatile uint32_t *)((uint8_t *)mapping + CTRL_OFFSET);
    int fd = -1, failed = 1;
    uint32_t magic = csr[0x10 / 4];
    printf("L2 diagnostic magic: 0x%08x\n", magic);
    if (magic != 0x4c325453) { puts("FAIL: Requires the l2 diagnostic bitstream; no DDR writes issued"); goto cleanup; }
    if (csr[0x18 / 4] & 1) { puts("FAIL: L2 writer is already busy"); goto cleanup; }
    fd = open("/dev/fpgagpu0", O_RDWR);
    if (fd < 0) { perror("open /dev/fpgagpu0"); goto cleanup; }
    _Alignas(4096) uint32_t actual[3 * WORDS], expected[3 * WORDS];
    for (unsigned i = 0; i < 3 * WORDS; i++) expected[i] = SENTINEL;
    for (unsigned page = 0; page < 3; page++) {
        if (dma(fd, FPGAGPU_DMA_TO_DEVICE, page * 4096, &expected[page * WORDS]) ||
            dma(fd, FPGAGPU_DMA_FROM_DEVICE, page * 4096, &actual[page * WORDS])) goto cleanup;
    }
    if (memcmp(actual, expected, sizeof(actual))) { puts("FAIL: DMA sentinel precheck"); goto cleanup; }
    puts("PASS: Host DMA precheck for target and both guard pages");
    for (unsigned i = 0; i < WORDS; i++) expected[WORDS + i] = 0x4c320000u ^ i;
    atomic_thread_fence(memory_order_seq_cst);
    uint32_t previous_run = csr[0x20 / 4];
    csr[0x14 / 4] = 1;
    // Readback drains the posted PCIe start write before polling.
    uint32_t status = csr[0x18 / 4];
    double deadline = now() + 10;
    while (((status & 3) != 2 || csr[0x20 / 4] == previous_run) && now() < deadline) {
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
        status = csr[0x18 / 4];
    }
    uint32_t completed = csr[0x1c / 4];
    printf("L2 writer: status=0x%08x completed_lines=%u/128\n", status, completed);
    if (status != 2 || completed != 128 || csr[0x20 / 4] == previous_run) { puts("FAIL: L2 completion timeout or AXI write response error"); goto cleanup; }
    for (unsigned page = 0; page < 3; page++)
        if (dma(fd, FPGAGPU_DMA_FROM_DEVICE, page * 4096, &actual[page * WORDS])) goto cleanup;
    unsigned mismatches = 0;
    for (unsigned i = 0; i < 3 * WORDS; i++) {
        if (actual[i] != expected[i]) {
            if (mismatches < 8)
                printf("FAIL: DDR[0x%08x] expected=0x%08x actual=0x%08x\n", i * 4, expected[i], actual[i]);
            mismatches++;
        }
    }
    if (write_raw("l2_memory_cpu.bin", (int32_t *)expected, 3 * WORDS) ||
        write_raw("l2_memory_gpu.bin", (int32_t *)actual, 3 * WORDS)) goto cleanup;
    failed = mismatches != 0;
    printf("%s: L2 wrote 4096 bytes; Host verified target and both guards; mismatches=%u\n",
           failed ? "FAIL" : "PASS", mismatches);
cleanup:
    if (fd >= 0) close(fd);
    munmap(mapping, MAP_SIZE);
    return failed;
}
