#ifndef _FPGAGPU_UMD_H
#define _FPGAGPU_UMD_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <CL/cl.h>
#include "../../include/uapi/fpgagpu_ioctl.h"

#define FPGAGPU_MAX_ARGS        32
#define FPGAGPU_MAX_DEVICES     1
#define FPGAGPU_KERNARG_BASE    0x00000000ULL
#define FPGAGPU_KERNARG_SIZE    4096
#define FPGAGPU_VRAM_USER_BASE  0x00010000ULL // 64KB offset
#define FPGAGPU_VRAM_TOTAL_SIZE (1024ULL * 1024ULL * 1024ULL) // 1GB DDR3

/* OpenCL Platform Object */
struct _cl_platform_id {
    const char *name;
    const char *vendor;
    const char *version;
    const char *profile;
};

/* OpenCL Device Object */
struct _cl_device_id {
    cl_platform_id platform;
    const char *name;
    const char *vendor;
    cl_device_type type;
    cl_uint max_compute_units;
    cl_uint max_work_item_dimensions;
    size_t max_work_item_sizes[3];
    size_t max_work_group_size;
    cl_ulong global_mem_size;
    cl_ulong max_mem_alloc_size;
    cl_uint address_bits;
    int fd; // device file descriptor
};

/* OpenCL Context Object */
struct _cl_context {
    cl_device_id device;
    int fd;
    cl_uint ref_count;
};

/* OpenCL Command Queue Object */
struct _cl_command_queue {
    cl_context context;
    cl_device_id device;
    cl_command_queue_properties properties;
    cl_uint ref_count;
};

/* OpenCL Memory Object (Buffer) */
struct _cl_mem {
    cl_context context;
    cl_mem_flags flags;
    size_t size;
    void *host_ptr;
    uint64_t ddr3_addr; // Allocated address in FPGA DDR3
    cl_uint ref_count;
};

/* OpenCL Program Object */
struct _cl_program {
    cl_context context;
    char *source;
    size_t source_len;
    uint32_t *binary_words;
    uint32_t num_instructions;
    bool is_built;
    cl_uint ref_count;
};

/* OpenCL Kernel Argument Entry */
typedef struct {
    bool is_set;
    bool is_mem;
    size_t size;
    uint32_t scalar_val;
    cl_mem mem_obj;
} fpgagpu_kernel_arg_t;

/* OpenCL Kernel Object */
struct _cl_kernel {
    cl_program program;
    char *name;
    fpgagpu_kernel_arg_t args[FPGAGPU_MAX_ARGS];
    cl_uint num_args;
    cl_uint ref_count;
};

/* OpenCL Event Object */
struct _cl_event {
    cl_context context;
    cl_command_queue queue;
    cl_int status;
    cl_uint ref_count;
};

/* Memory Allocator API (VRAM) */
uint64_t fpgagpu_vram_alloc(size_t size);
void fpgagpu_vram_free(uint64_t addr, size_t size);
void fpgagpu_vram_init(void);

#endif /* _FPGAGPU_UMD_H */
