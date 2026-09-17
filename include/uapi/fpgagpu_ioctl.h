#ifndef _FPGAGPU_IOCTL_H
#define _FPGAGPU_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * IOCTL Magic Number.
 * 'F' for FPGA-GPU / OpenCL Accelerator
 */
#define FPGAGPU_IOC_MAGIC  'F'

/* Dispatch Packet Magic */
#define FPGAGPU_MAGIC_OCL   0x4F434C31UL /* "OCL1" */

/* DMA Directions (Host RAM <-> FPGA DDR3 VRAM) */
#define FPGAGPU_DMA_TO_DEVICE   0 /* Host -> FPGA DDR3 (H2C) */
#define FPGAGPU_DMA_FROM_DEVICE 1 /* FPGA DDR3 -> Host (C2H) */

struct fpgagpu_dma_param {
    __u32 direction;     /* FPGAGPU_DMA_TO_DEVICE or FPGAGPU_DMA_FROM_DEVICE */
    __u32 size;          /* transfer size in bytes */
    __u64 ddr3_addr;     /* FPGA DDR3 AXI base address */
    __u64 host_vaddr;    /* user space buffer pointer (uintptr_t) */
};

/*
 * Compute Command Dispatch Descriptor (Task Command)
 */
struct fpgagpu_launch_cmd {
    __u32 opcode;         /* Operation code: 0x01 = Launch Kernel, 0x10 = Load Kernel */
    __u32 grid_dim_x;     /* Grid Dimension X */
    __u32 grid_dim_y;     /* Grid Dimension Y */
    __u32 block_dim_x;    /* Block Dimension X (Workgroup size X) */
    __u32 block_dim_y;    /* Block Dimension Y (Workgroup size Y) */
    __u64 dma_src_addr;   /* FPGA DDR3 Kernarg Base Address */
    __u64 dma_dst_addr;   /* FPGA DDR3 Target Address / Output */
    __u32 num_elements;   /* Total Work Items or Instructions */
    __u32 payload_size;   /* Reserved / Extended payload size */
    __u64 payload_vaddr;  /* Reserved / Extended payload pointer */
};

/*
 * Hardware Bitstream Version Info
 */
struct fpgagpu_version_info {
    __u32 major_version;  /* FPGA Bitstream Major Version */
    __u32 minor_version;  /* FPGA Bitstream Minor Version */
};

/*
 * Dynamic Kernel Instruction Loading Parameters
 */
struct fpgagpu_kernel_binary {
    __u64 user_instr_ptr; /* User space instruction array pointer (const uint32_t*) */
    union {
        __u32 instr_size; /* Instruction count (max 1024 words = 4096 bytes) */
        __u32 num_words;
    };
    __u32 kernel_id;      /* Unique kernel identifier */
};

#define FPGAGPU_OPCODE_NOP           0x00
#define FPGAGPU_OPCODE_LAUNCH_KERNEL 0x01
#define FPGAGPU_OPCODE_LOAD_KERNEL   0x10

/*
 * IOCTL System Call Definitions
 */
#define FPGAGPU_IOC_SUBMIT_CMD   _IOW(FPGAGPU_IOC_MAGIC, 1, struct fpgagpu_launch_cmd)
#define FPGAGPU_IOC_DOORBELL     _IO(FPGAGPU_IOC_MAGIC,  2)
#define FPGAGPU_IOC_WAIT_FOR_IRQ _IO(FPGAGPU_IOC_MAGIC,  3)
#define FPGAGPU_IOC_DMA_TRANSFER _IOW(FPGAGPU_IOC_MAGIC, 4, struct fpgagpu_dma_param)
#define FPGAGPU_IOC_GET_VERSION  _IOR(FPGAGPU_IOC_MAGIC, 5, struct fpgagpu_version_info)
#define FPGAGPU_IOC_LOAD_KERNEL  _IOW(FPGAGPU_IOC_MAGIC, 6, struct fpgagpu_kernel_binary)

#define FPGAGPU_IOC_MAXNR 6

/* Legacy Aliases for backwards compatibility with existing tests */
#define VGPU_IOC_MAGIC            FPGAGPU_IOC_MAGIC
#define vgpu_command              fpgagpu_launch_cmd
#define vgpu_dma_param            fpgagpu_dma_param
#define vgpu_version_info         fpgagpu_version_info
#define vgpu_kernel_binary        fpgagpu_kernel_binary
#define VGPU_DMA_TO_DEVICE        FPGAGPU_DMA_TO_DEVICE
#define VGPU_DMA_FROM_DEVICE      FPGAGPU_DMA_FROM_DEVICE
#define VGPU_OPCODE_NOP           FPGAGPU_OPCODE_NOP
#define VGPU_OPCODE_LAUNCH_KERNEL FPGAGPU_OPCODE_LAUNCH_KERNEL
#define VGPU_OPCODE_LOAD_KERNEL   FPGAGPU_OPCODE_LOAD_KERNEL
#define VGPU_IOC_SUBMIT_CMD       FPGAGPU_IOC_SUBMIT_CMD
#define VGPU_IOC_DOORBELL         FPGAGPU_IOC_DOORBELL
#define VGPU_IOC_WAIT_FOR_IRQ     FPGAGPU_IOC_WAIT_FOR_IRQ
#define VGPU_IOC_DMA_TRANSFER     FPGAGPU_IOC_DMA_TRANSFER
#define VGPU_IOC_GET_VERSION      FPGAGPU_IOC_GET_VERSION
#define VGPU_IOC_LOAD_KERNEL      FPGAGPU_IOC_LOAD_KERNEL
#define VGPU_IOC_MAXNR            FPGAGPU_IOC_MAXNR

#endif /* _FPGAGPU_IOCTL_H */
