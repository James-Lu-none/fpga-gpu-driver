#ifndef _FPGAGPU_CORE_H
#define _FPGAGPU_CORE_H

#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include "../uapi/fpgagpu_ioctl.h"

#define FPGAGPU_KERNEL_STAGING_OFFSET 0x00010000 // BAR0 BRAM Staging Buffer for Dynamic Kernels (16KB)
#define FPGAGPU_RING_OFFSET           0x00018000 // Direct BAR0 BRAM Ring Buffer Offset (32KB)
#define QUEUE_SIZE                    512        // Size of task descriptor is 64 bytes, 32KB/64 = 512

/*
 * XDMA Hardware Engine Control Registers (Mapped to BAR1)
 */
#define XDMA_H2C_CHAN0_CTRL   0x0004 // H2C Channel 0 Control Register (Write 1 to Run)
#define XDMA_H2C_CHAN0_STATUS 0x0040 // H2C Channel 0 Status Register (Bit 0: Busy)
#define XDMA_H2C_CHAN0_SG_LO  0x4080 // H2C Channel 0 SGDMA First Descriptor Address Low
#define XDMA_H2C_CHAN0_SG_HI  0x4084 // H2C Channel 0 SGDMA First Descriptor Address High
#define XDMA_H2C_CHAN0_SG_ADJ 0x4088 // H2C Channel 0 SGDMA Adjacent Descriptors

#define XDMA_C2H_CHAN0_CTRL   0x1004 // C2H Channel 0 Control Register (Write 1 to Run)
#define XDMA_C2H_CHAN0_STATUS 0x1040 // C2H Channel 0 Status Register (Bit 0: Busy)
#define XDMA_C2H_CHAN0_SG_LO  0x5080 // C2H Channel 0 SGDMA First Descriptor Address Low
#define XDMA_C2H_CHAN0_SG_HI  0x5084 // C2H Channel 0 SGDMA First Descriptor Address High
#define XDMA_C2H_CHAN0_SG_ADJ 0x5088 // C2H Channel 0 SGDMA Adjacent Descriptors

/*
 * Hardware Dispatch Packet Structure (64-byte aligned)
 * Host CPU / Driver -> PCIe BAR0 Write -> PicoRV32 Command Processor
 */
struct fpgagpu_dispatch_packet {
    u32 magic;         /* 0x4F434C31 ("OCL1") */
    u32 opcode;        /* 0x01: Launch, 0x10: Load Kernel */
    u32 grid_dim_x;    /* Grid Dimension X */
    u32 grid_dim_y;    /* Grid Dimension Y */
    u32 block_dim_x;   /* Block Dimension X (Workgroup size X) */
    u32 block_dim_y;   /* Block Dimension Y (Workgroup size Y) */
    u64 src_dma_addr;  /* Kernarg Base Address / Source DMA Address */
    u64 dst_dma_addr;  /* Destination Address */
    u32 num_elements;  /* Vector Element Count or Instruction Count */
    u32 task_id;       /* Monotonic task id */
    u32 kernel_entry;  /* Entry PC in I-RAM (default 0) */
    u32 reserved[3];   /* Padding to 64 bytes */
} __packed __aligned(64);

#define cuda_task_descriptor fpgagpu_dispatch_packet

/*
 * Lock-Free Ring Buffer (Mapped directly into FPGA BRAM via BAR0)
 */
struct fpgagpu_ring_buffer {
    volatile u32 head; /* Updated by PicoRV32 (Consumer) */
    volatile u32 tail; /* Updated by CPU Host (Producer) */
    u32 reserved[14];  /* 56 bytes padding to align cmds to 64 bytes */
    struct fpgagpu_dispatch_packet cmds[QUEUE_SIZE];
};

#define vgpu_ring_buffer fpgagpu_ring_buffer

extern int queue_mode;

struct fpgagpu_context {
    struct fpgagpu_dev *dev;
    struct fpgagpu_ring_buffer *ring;
    wait_queue_head_t wait_q;
    int irq_fired;
    struct list_head list_node;
};

#define vgpu_context fpgagpu_context

/*
 * XDMA Hardware Descriptor Format (32-byte aligned)
 */
struct xdma_desc {
    u32 control;         /* Magic (0xAD4B0000), EOP (bit 0), IRQ (bit 1) */
    u32 bytes;           /* Transfer size in bytes */
    u64 src_addr;        /* Source DMA Address (Host System RAM) */
    u64 dst_addr;        /* Destination AXI Address (FPGA internal buffer) */
    u64 next_desc;       /* Next Descriptor DMA Address (Host System RAM) */
} __packed;

#define XDMA_DESC_MAGIC 0xAD4B0000
#define XDMA_DESC_EOP   (1u << 0)

struct fpgagpu_dev {
    int minor;
    struct cdev cdev;
    struct device *device;
    struct pci_dev *pci_dev;
    void __iomem *csr_base;  // Mapped PCIe BAR0 (AXI-Lite to BRAM & GPU Control Registers)
    void __iomem *dma_base;  // Mapped PCIe BAR1 (XDMA Config & Status Registers)
    int irq;                 // Allocated PCI IRQ

    struct fpgagpu_ring_buffer *ring; // Points to ring buffer in BRAM
    spinlock_t global_lock;           // Lock among multiple CPU producer threads

    struct list_head ctx_list;
    spinlock_t ctx_lock;

    wait_queue_head_t wait_q;
    int irq_fired;

    void *ring_buffer;
    dma_addr_t dma_handle;

    struct xdma_desc *desc_ring;
    dma_addr_t desc_ring_dma_addr;
    struct page **pinned_pages;
    int num_pinned_pages;

    struct scatterlist *sgl;
    int sgl_nents;
};

#define vgpu_dev fpgagpu_dev
#define MAX_FPGAGPU_DEVICES 4
#define MAX_VGPU_DEVICES MAX_FPGAGPU_DEVICES

extern struct class *g_fpgagpu_class;
#define g_vgpu_class g_fpgagpu_class

long fpgagpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int fpgagpu_mmap(struct file *file, struct vm_area_struct *vma);
void fpgagpu_hw_work_func(struct work_struct *work);
irqreturn_t fpgagpu_irq_handler(int irq, void *dev_id);

#define vgpu_ioctl fpgagpu_ioctl
#define vgpu_mmap fpgagpu_mmap
#define vgpu_hw_work_func fpgagpu_hw_work_func
#define vgpu_irq_handler fpgagpu_irq_handler

#define VGPU_KERNEL_STAGING_OFFSET FPGAGPU_KERNEL_STAGING_OFFSET
#define VGPU_RING_OFFSET           FPGAGPU_RING_OFFSET

#endif /* _FPGAGPU_CORE_H */
