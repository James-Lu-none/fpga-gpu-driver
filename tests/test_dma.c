#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include "../include/uapi/fpgagpu_ioctl.h"

#define fpgagpu_DEVICE "/dev/fpgagpu0"

static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int test_dma_transfer(int fd, uint64_t ddr3_addr, size_t size) {
    printf("[Test] AXI Addr: 0x%08lx, Size: %6zu bytes (%4zu KB)... ",
           ddr3_addr, size, size / 1024);
    fflush(stdout);

    uint8_t *tx_buf = NULL;
    uint8_t *rx_buf = NULL;
    if (posix_memalign((void **)&tx_buf, 4096, size) != 0 ||
        posix_memalign((void **)&rx_buf, 4096, size) != 0) {
        printf("FAILED (Memory allocation failed)\n");
        return -1;
    }

    // Pattern fill
    for (size_t i = 0; i < size; i++) {
        tx_buf[i] = (uint8_t)((ddr3_addr + i * 31 + 0xA5) & 0xFF);
    }
    memset(rx_buf, 0x00, size);

    // 1. Host -> Device (H2C)
    struct fpgagpu_dma_param dma_tx = {
        .direction  = fpgagpu_DMA_TO_DEVICE,
        .size       = size,
        .ddr3_addr  = ddr3_addr,
        .host_vaddr = (uintptr_t)tx_buf,
    };

    double t0 = get_time_sec();
    if (ioctl(fd, fpgagpu_IOC_DMA_TRANSFER, &dma_tx) < 0) {
        perror("FAILED (ioctl H2C DMA)");
        free(tx_buf);
        free(rx_buf);
        return -1;
    }
    double t1 = get_time_sec();

    // 2. Device -> Host (C2H)
    struct fpgagpu_dma_param dma_rx = {
        .direction  = fpgagpu_DMA_FROM_DEVICE,
        .size       = size,
        .ddr3_addr  = ddr3_addr,
        .host_vaddr = (uintptr_t)rx_buf,
    };

    double t2 = get_time_sec();
    if (ioctl(fd, fpgagpu_IOC_DMA_TRANSFER, &dma_rx) < 0) {
        perror("FAILED (ioctl C2H DMA)");
        free(tx_buf);
        free(rx_buf);
        return -1;
    }
    double t3 = get_time_sec();

    // 3. Verify
    if (memcmp(tx_buf, rx_buf, size) != 0) {
        printf("FAILED (Data Mismatch!)\n");
        for (size_t i = 0; i < size; i++) {
            if (tx_buf[i] != rx_buf[i]) {
                printf("   Mismatch at byte %zu: expected 0x%02X, got 0x%02X\n",
                       i, tx_buf[i], rx_buf[i]);
                break;
            }
        }
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    double wr_bw = (size / (1024.0 * 1024.0)) / (t1 - t0);
    double rd_bw = (size / (1024.0 * 1024.0)) / (t3 - t2);
    printf("PASSED! (Write: %.1f MB/s, Read: %.1f MB/s)\n", wr_bw, rd_bw);

    free(tx_buf);
    free(rx_buf);
    return 0;
}

static int dma_range(int fd, unsigned direction, uint64_t address, void *buffer, size_t size)
{
    struct fpgagpu_dma_param p = {
        .direction = direction, .size = size, .ddr3_addr = address,
        .host_vaddr = (uintptr_t)buffer,
    };
    if (ioctl(fd, FPGAGPU_IOC_DMA_TRANSFER, &p) < 0) { perror("DMA range"); return -1; }
    return 0;
}

static int test_dma_range(int fd, size_t tx_offset, size_t rx_offset, size_t ddr_offset, size_t size)
{
    const size_t bytes = 4 * (size_t)sysconf(_SC_PAGESIZE);
    const size_t page_size = bytes / 4;
    const uint64_t base = 0x00400000;
    uint8_t *tx = NULL, *rx = NULL, *image = NULL, *expected = NULL;
    int failed = -1;
    printf("[Range] size=%zu host_tx_offset=%zu host_rx_offset=%zu ddr_offset=%zu\n",
           size, tx_offset, rx_offset, ddr_offset);
    if (tx_offset + size > bytes || rx_offset + size > bytes || ddr_offset + size > bytes ||
        posix_memalign((void **)&tx, page_size, bytes) ||
        posix_memalign((void **)&rx, page_size, bytes) ||
        posix_memalign((void **)&image, page_size, bytes) ||
        posix_memalign((void **)&expected, page_size, bytes)) goto cleanup;
    memset(tx, 0x5c, bytes);
    memset(rx, 0xcc, bytes);
    memset(image, 0xa5, bytes);
    memset(expected, 0xa5, bytes);
    for (size_t i = 0; i < size; i++) {
        uint8_t value = (uint8_t)(i * 31 + 7);
        tx[tx_offset + i] = value;
        expected[ddr_offset + i] = value;
    }
    if (dma_range(fd, FPGAGPU_DMA_TO_DEVICE, base, image, bytes) ||
        dma_range(fd, FPGAGPU_DMA_TO_DEVICE, base + ddr_offset, tx + tx_offset, size) ||
        dma_range(fd, FPGAGPU_DMA_FROM_DEVICE, base, image, bytes)) goto cleanup;
    for (size_t i = 0; i < bytes; i++) {
        if (image[i] != expected[i]) {
            printf("FAIL: H2C range/VRAM guard byte=%zu expected=%02x actual=%02x\n", i, expected[i], image[i]);
            goto cleanup;
        }
    }
    if (dma_range(fd, FPGAGPU_DMA_FROM_DEVICE, base + ddr_offset, rx + rx_offset, size)) goto cleanup;
    for (size_t i = 0; i < bytes; i++) {
        uint8_t value = i >= rx_offset && i < rx_offset + size ? tx[tx_offset + i-rx_offset] : 0xcc;
        if (rx[i] != value) {
            printf("FAIL: C2H range/Host guard byte=%zu expected=%02x actual=%02x\n", i, value, rx[i]);
            goto cleanup;
        }
    }
    puts("PASS: exact DMA payload, VRAM guards and Host guards");
    failed = 0;
cleanup:
    free(tx); free(rx); free(image); free(expected);
    return failed;
}

int main(void) {
    printf("fpgagpu Core Driver IOCTL Verification Tool (/dev/fpgagpu0)\n");

    int fd = open(fpgagpu_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("Error opening " fpgagpu_DEVICE);
        fprintf(stderr, "Make sure fpgagpu_driver.ko is loaded\n");
        return 1;
    }

    printf("Successfully opened %s (fd=%d)\n", fpgagpu_DEVICE, fd);

    struct fpgagpu_version_info ver = {0};
    if (ioctl(fd, fpgagpu_IOC_GET_VERSION, &ver) == 0) {
        printf("FPGA Bitstream Version: v%u.%u\n", ver.major_version, ver.minor_version);
    }

    printf("[Phase 1] Testing DDR3 VRAM DMA Transfers via fpgagpu_IOC_DMA_TRANSFER...\n");

    int failed = 0;
    const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    failed += test_dma_range(fd, 32, 96, 64, 128) != 0;
    failed += test_dma_range(fd, page_size-32, page_size-64, page_size-16, 128) != 0;
    failed += test_dma_range(fd, 64, page_size-32, 32, page_size+128) != 0;
    failed += test_dma_transfer(fd, 0x00000000, 4096) != 0;       // 4KB at base
    failed += test_dma_transfer(fd, 0x00010000, 65536) != 0;      // 64KB
    failed += test_dma_transfer(fd, 0x00100000, 262144) != 0;     // 256KB at 1MB
    failed += test_dma_transfer(fd, 0x01000000, 1048576) != 0;    // 1MB at 16MB
    failed += test_dma_transfer(fd, 0x10000000, 2097152) != 0;    // 2MB at 256MB
    failed += test_dma_transfer(fd, 0x20000000, 4194304) != 0;    // 4MB at 512MB
    failed += test_dma_transfer(fd, 0x3F000000, 1048576) != 0;    // 1MB at 1008MB (near 1GB)

    printf("[Phase 2] Testing BRAM Ring Buffer via fpgagpu_IOC_SUBMIT_CMD...\n");

    struct fpgagpu_command cmd = {
        .opcode       = 0,          // Opcode 0: Ring Buffer Probe (NOP/Exit)
        .grid_dim_x   = 1,
        .grid_dim_y   = 1,
        .block_dim_x  = 32,
        .block_dim_y  = 1,
        .dma_src_addr = 0x00000000,
        .dma_dst_addr = 0x00000000,
        .num_elements = 32,
    };

    printf("Submitting task descriptor to PicoRV32 BRAM Ring Buffer... ");
    fflush(stdout);

    if (ioctl(fd, fpgagpu_IOC_SUBMIT_CMD, &cmd) < 0) {
        perror("FAILED (fpgagpu_IOC_SUBMIT_CMD)");
        failed++;
    } else {
        printf("SUCCESS\n");
    }

    printf("Waiting for PicoRV32/GPU task completion via fpgagpu_IOC_DOORBELL... ");
    fflush(stdout);

    if (ioctl(fd, fpgagpu_IOC_DOORBELL, 0) < 0) {
        perror("FAILED (fpgagpu_IOC_DOORBELL)");
        failed++;
    } else {
        printf("SUCCESS (Completed by PicoRV32!)\n");
    }

    if (failed == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("FAILED! %d TEST(S) FAILED\n", failed);
    }

    close(fd);
    return failed ? 1 : 0;
}
