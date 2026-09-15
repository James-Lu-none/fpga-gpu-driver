#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include "../include/uapi/vgpu_ioctl.h"

#define VGPU_DEVICE "/dev/vgpu0"

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
    struct vgpu_dma_param dma_tx = {
        .direction  = VGPU_DMA_TO_DEVICE,
        .size       = size,
        .ddr3_addr  = ddr3_addr,
        .host_vaddr = (uintptr_t)tx_buf,
    };

    double t0 = get_time_sec();
    if (ioctl(fd, VGPU_IOC_DMA_TRANSFER, &dma_tx) < 0) {
        perror("FAILED (ioctl H2C DMA)");
        free(tx_buf);
        free(rx_buf);
        return -1;
    }
    double t1 = get_time_sec();

    // 2. Device -> Host (C2H)
    struct vgpu_dma_param dma_rx = {
        .direction  = VGPU_DMA_FROM_DEVICE,
        .size       = size,
        .ddr3_addr  = ddr3_addr,
        .host_vaddr = (uintptr_t)rx_buf,
    };

    double t2 = get_time_sec();
    if (ioctl(fd, VGPU_IOC_DMA_TRANSFER, &dma_rx) < 0) {
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

int main(int argc, char **argv) {
    printf("vGPU Core Driver IOCTL Verification Tool (/dev/vgpu0)\n");

    int fd = open(VGPU_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("Error opening " VGPU_DEVICE);
        fprintf(stderr, "Make sure vgpu_driver.ko is loaded\n");
        return 1;
    }

    printf("Successfully opened %s (fd=%d)\n", VGPU_DEVICE, fd);

    struct vgpu_version_info ver = {0};
    if (ioctl(fd, VGPU_IOC_GET_VERSION, &ver) == 0) {
        printf("FPGA Bitstream Version: 0x%08X (Magic: 0x%08X)\n", ver.hw_version, ver.hw_magic);
    }

    printf("[Phase 1] Testing DDR3 VRAM DMA Transfers via VGPU_IOC_DMA_TRANSFER...\n");

    int failed = 0;
    failed += test_dma_transfer(fd, 0x00000000, 4096);       // 4KB at base
    failed += test_dma_transfer(fd, 0x00010000, 65536);      // 64KB
    failed += test_dma_transfer(fd, 0x00100000, 262144);     // 256KB at 1MB
    failed += test_dma_transfer(fd, 0x01000000, 1048576);    // 1MB at 16MB
    failed += test_dma_transfer(fd, 0x10000000, 2097152);    // 2MB at 256MB
    failed += test_dma_transfer(fd, 0x20000000, 4194304);    // 4MB at 512MB
    failed += test_dma_transfer(fd, 0x3F000000, 1048576);    // 1MB at 1008MB (near 1GB)

    printf("[Phase 2] Testing BRAM Ring Buffer via VGPU_IOC_SUBMIT_CMD...\n");

    struct vgpu_command cmd = {
        .opcode       = 1,          // e.g. Vector Add
        .grid_dim_x   = 32,
        .grid_dim_y   = 1,
        .block_dim_x  = 32,
        .block_dim_y  = 1,
        .dma_src_addr = 0x00000000, // Data in DDR3 VRAM
        .dma_dst_addr = 0x00100000, // Output in DDR3 VRAM
        .num_elements = 1024,
    };

    printf("Submitting task descriptor to PicoRV32 BRAM Ring Buffer... ");
    fflush(stdout);

    if (ioctl(fd, VGPU_IOC_SUBMIT_CMD, &cmd) < 0) {
        perror("FAILED (VGPU_IOC_SUBMIT_CMD)");
        failed++;
    } else {
        printf("SUCCESS\n");
    }

    printf("Waiting for PicoRV32/GPU task completion via VGPU_IOC_DOORBELL... ");
    fflush(stdout);

    if (ioctl(fd, VGPU_IOC_DOORBELL, 0) < 0) {
        perror("FAILED (VGPU_IOC_DOORBELL)");
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
