#include "fpgagpu_umd.h"
#include <pthread.h>

#define PAGE_SIZE_4K 4096ULL

static pthread_mutex_t vram_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t current_heap_ptr = FPGAGPU_VRAM_USER_BASE;
static bool vram_initialized = false;

void fpgagpu_vram_init(void) {
    pthread_mutex_lock(&vram_mutex);
    current_heap_ptr = FPGAGPU_VRAM_USER_BASE;
    vram_initialized = true;
    pthread_mutex_unlock(&vram_mutex);
}

uint64_t fpgagpu_vram_alloc(size_t size) {
    if (size == 0) return 0;

    pthread_mutex_lock(&vram_mutex);
    if (!vram_initialized) {
        current_heap_ptr = FPGAGPU_VRAM_USER_BASE;
        vram_initialized = true;
    }

    // Align size to 4KB page boundary
    size_t aligned_size = (size + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);

    if (current_heap_ptr + aligned_size > FPGAGPU_VRAM_TOTAL_SIZE) {
        pthread_mutex_unlock(&vram_mutex);
        fprintf(stderr, "[UMD] VRAM allocation failed: Out of memory (requested %zu bytes)\n", size);
        return 0;
    }

    uint64_t alloc_addr = current_heap_ptr;
    current_heap_ptr += aligned_size;

    pthread_mutex_unlock(&vram_mutex);
    return alloc_addr;
}

void fpgagpu_vram_free(uint64_t addr, size_t size) {
    (void)addr;
    (void)size;
    // For simple bump allocator in prototype, memory is reclaimed on context teardown or reset
}
