#include "../include/kmd/fpgagpu_core.h"

long vgpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vgpu_context *ctx = file->private_data;
    struct vgpu_dev *dev = ctx->dev;
    
    if (_IOC_TYPE(cmd) != FPGAGPU_IOC_MAGIC && _IOC_TYPE(cmd) != 'V') return -ENOTTY;
    if (_IOC_NR(cmd) > FPGAGPU_IOC_MAXNR) return -ENOTTY;

    switch (cmd) {
        case VGPU_IOC_DMA_TRANSFER: {
            struct vgpu_dma_param dma_param;
            int num_pages, i;
            u32 chan_ctrl, chan_status, chan_sg_lo, chan_sg_hi, chan_sg_adj;
            enum dma_data_direction dma_dir;
            unsigned int gup_flags;
            u64 cur_ddr3;
            struct scatterlist *sg;
            int timeout;

            if (copy_from_user(&dma_param, (struct vgpu_dma_param __user *)arg, sizeof(dma_param))) {
                return -EFAULT;
            }

            if (dma_param.size == 0 || dma_param.host_vaddr == 0) {
                return -EINVAL;
            }
            if (dma_param.size > 32 * 1024 * 1024) { // 單次最大支援 32MB
                return -EINVAL;
            }

            num_pages = (dma_param.size + PAGE_SIZE - 1) / PAGE_SIZE;
            if (num_pages > 8192) return -EINVAL;

            if (dma_param.direction == VGPU_DMA_TO_DEVICE) {
                chan_ctrl   = XDMA_H2C_CHAN0_CTRL;
                chan_status = XDMA_H2C_CHAN0_STATUS;
                chan_sg_lo  = XDMA_H2C_CHAN0_SG_LO;
                chan_sg_hi  = XDMA_H2C_CHAN0_SG_HI;
                chan_sg_adj = XDMA_H2C_CHAN0_SG_ADJ;
                dma_dir     = DMA_TO_DEVICE;
                gup_flags   = 0;
            } else if (dma_param.direction == VGPU_DMA_FROM_DEVICE) {
                chan_ctrl   = XDMA_C2H_CHAN0_CTRL;
                chan_status = XDMA_C2H_CHAN0_STATUS;
                chan_sg_lo  = XDMA_C2H_CHAN0_SG_LO;
                chan_sg_hi  = XDMA_C2H_CHAN0_SG_HI;
                chan_sg_adj = XDMA_C2H_CHAN0_SG_ADJ;
                dma_dir     = DMA_FROM_DEVICE;
                gup_flags   = FOLL_WRITE;
            } else {
                return -EINVAL;
            }

            // Pin user pages in RAM
            dev->num_pinned_pages = get_user_pages_fast(dma_param.host_vaddr, num_pages, 
                                                        gup_flags, dev->pinned_pages);
            if (dev->num_pinned_pages < 0) {
                pr_err("vGPU-Core: get_user_pages_fast failed: %d\n", dev->num_pinned_pages);
                return dev->num_pinned_pages;
            }

            dev->sgl = kmalloc_array(dev->num_pinned_pages, sizeof(struct scatterlist), GFP_KERNEL);
            if (!dev->sgl) {
                for (i = 0; i < dev->num_pinned_pages; i++) put_page(dev->pinned_pages[i]);
                dev->num_pinned_pages = 0;
                return -ENOMEM;
            }

            sg_init_table(dev->sgl, dev->num_pinned_pages);
            for (i = 0; i < dev->num_pinned_pages; i++) {
                sg_set_page(&dev->sgl[i], dev->pinned_pages[i], PAGE_SIZE, 0);
            }

            dev->sgl_nents = dma_map_sg(&dev->pci_dev->dev, dev->sgl, dev->num_pinned_pages, dma_dir);
            if (dev->sgl_nents == 0) {
                pr_err("vGPU-Core: dma_map_sg failed\n");
                kfree(dev->sgl);
                dev->sgl = NULL;
                for (i = 0; i < dev->num_pinned_pages; i++) put_page(dev->pinned_pages[i]);
                dev->num_pinned_pages = 0;
                return -ENOMEM;
            }

            // Build XDMA Descriptor Chain
            cur_ddr3 = dma_param.ddr3_addr;
            for_each_sg(dev->sgl, sg, dev->sgl_nents, i) {
                u32 len = sg_dma_len(sg);
                if (dma_param.direction == VGPU_DMA_TO_DEVICE) {
                    dev->desc_ring[i].src_addr = sg_dma_address(sg);
                    dev->desc_ring[i].dst_addr = cur_ddr3;
                } else {
                    dev->desc_ring[i].src_addr = cur_ddr3;
                    dev->desc_ring[i].dst_addr = sg_dma_address(sg);
                }
                cur_ddr3 += len;
                dev->desc_ring[i].bytes    = len;
                dev->desc_ring[i].control  = XDMA_DESC_MAGIC;

                if (i < dev->sgl_nents - 1) {
                    dev->desc_ring[i].next_desc = dev->desc_ring_dma_addr + (i + 1) * sizeof(struct xdma_desc);
                } else {
                    dev->desc_ring[i].next_desc = 0;
                    dev->desc_ring[i].control  |= XDMA_DESC_EOP; 
                }
            }

            // Start XDMA Transfer
            iowrite32(lower_32_bits(dev->desc_ring_dma_addr), dev->dma_base + chan_sg_lo);
            iowrite32(upper_32_bits(dev->desc_ring_dma_addr), dev->dma_base + chan_sg_hi);
            iowrite32(0, dev->dma_base + chan_sg_adj);
            iowrite32(1, dev->dma_base + chan_ctrl); // Run

            // Synchronous wait for completion with timeout
            timeout = 10000000;
            while ((ioread32(dev->dma_base + chan_status) & 1) != 0 && --timeout > 0) {
                cpu_relax();
            }
            iowrite32(0, dev->dma_base + chan_ctrl); // Stop

            // Teardown & Unmap
            dma_unmap_sg(&dev->pci_dev->dev, dev->sgl, dev->num_pinned_pages, dma_dir);
            kfree(dev->sgl);
            dev->sgl = NULL;

            for (i = 0; i < dev->num_pinned_pages; i++) {
                if (dma_param.direction == VGPU_DMA_FROM_DEVICE) {
                    set_page_dirty_lock(dev->pinned_pages[i]);
                }
                put_page(dev->pinned_pages[i]);
            }
            dev->num_pinned_pages = 0;

            if (timeout == 0) {
                pr_err("vGPU-Core: DMA transfer timed out!\n");
                return -ETIMEDOUT;
            }
            break;
        }
        
        case VGPU_IOC_SUBMIT_CMD: {
            struct vgpu_command user_cmd;

            if (copy_from_user(&user_cmd, (struct vgpu_command __user *)arg, sizeof(user_cmd))) {
                return -EFAULT;
            }

            if (queue_mode == 0) {
                u32 head, tail;

                // Protect multiple CPU producer threads from stepping on each other
                spin_lock(&dev->global_lock);
                
                // Read head/tail from FPGA BRAM (BAR0)
                head = ioread32(&dev->ring->head);
                tail = ioread32(&dev->ring->tail);
                
                if ((tail + 1) % QUEUE_SIZE == head) {
                    spin_unlock(&dev->global_lock);
                    return -EBUSY; // Queue Full
                }
                
                /* 
                 * Build Hardware Task Descriptor (64 bytes)
                 * Direct mapped to PicoRV32's cuda_task_descriptor_t
                 */
                struct fpgagpu_dispatch_packet task = {};
                task.magic        = FPGAGPU_MAGIC_OCL; /* "OCL1" */
                task.opcode       = user_cmd.opcode;
                task.grid_dim_x   = user_cmd.grid_dim_x ? user_cmd.grid_dim_x : 32;
                task.grid_dim_y   = user_cmd.grid_dim_y ? user_cmd.grid_dim_y : 1;
                task.block_dim_x  = user_cmd.block_dim_x ? user_cmd.block_dim_x : 32;
                task.block_dim_y  = user_cmd.block_dim_y ? user_cmd.block_dim_y : 1;
                task.src_dma_addr = user_cmd.dma_src_addr;
                task.dst_dma_addr = user_cmd.dma_dst_addr;
                task.num_elements = user_cmd.num_elements;

                // Write the 64-byte task directly into BRAM Ring Buffer via CSR MMIO
                memcpy_toio(&dev->ring->cmds[tail], &task, sizeof(task));
                
                // Update tail pointer in BRAM. PicoRV32 polls this pointer.
                iowrite32((tail + 1) % QUEUE_SIZE, &dev->ring->tail);
                
                spin_unlock(&dev->global_lock);
            } else {
                pr_err("vGPU-Core: Private queue mode currently unsupported\n");
                return -ENOTSUPP;
            }
            break;
        }

        case VGPU_IOC_DOORBELL:
            if (queue_mode == 0) {
                u32 tail = ioread32(&dev->ring->tail);
                int timeout = 5000000;
                
                while (ioread32(&dev->ring->head) != tail && --timeout > 0) {
                    schedule();
                }
                if (timeout == 0) {
                    pr_err("vGPU-Core: Wait for completion timeout!\n");
                    return -ETIMEDOUT;
                }
            } else {
                pr_err("vGPU-Core: Private queue mode currently unsupported\n");
                return -ENOTSUPP;
            }
            break;

        case VGPU_IOC_GET_VERSION: {
            struct vgpu_version_info info;
            info.major_version = ioread32(dev->csr_base + 0x20008);
            info.minor_version = ioread32(dev->csr_base + 0x2000C);
            if (copy_to_user((void __user *)arg, &info, sizeof(info)))
                return -EFAULT;
            break;
        }

        case VGPU_IOC_LOAD_KERNEL: {
            struct vgpu_kernel_binary kbin;
            u32 __user *u_code;
            void __iomem *staging;
            u32 head, tail, target_tail;
            int i, timeout;

            if (copy_from_user(&kbin, (struct vgpu_kernel_binary __user *)arg, sizeof(kbin)))
                return -EFAULT;

            if (kbin.instr_size == 0 || kbin.instr_size > 1024)
                return -EINVAL;

            u_code = (u32 __user *)kbin.user_instr_ptr;
            staging = dev->csr_base + VGPU_KERNEL_STAGING_OFFSET;

            // Copy instructions word-by-word into BRAM Staging Buffer
            for (i = 0; i < kbin.instr_size; i++) {
                u32 word;
                if (get_user(word, &u_code[i]))
                    return -EFAULT;
                iowrite32(word, staging + (i * 4));
            }

            spin_lock(&dev->global_lock);
            head = ioread32(&dev->ring->head);
            tail = ioread32(&dev->ring->tail);

            if ((tail + 1) % QUEUE_SIZE == head) {
                spin_unlock(&dev->global_lock);
                return -EBUSY;
            }

            struct fpgagpu_dispatch_packet task = {};
            task.magic        = FPGAGPU_MAGIC_OCL; /* "OCL1" */
            task.opcode       = VGPU_OPCODE_LOAD_KERNEL;
            task.num_elements = kbin.instr_size;
            task.task_id      = kbin.kernel_id;

            memcpy_toio(&dev->ring->cmds[tail], &task, sizeof(task));
            target_tail = (tail + 1) % QUEUE_SIZE;
            iowrite32(target_tail, &dev->ring->tail);
            spin_unlock(&dev->global_lock);

            // Synchronously wait for PicoRV32 to finish loading into GPU I-RAM
            timeout = 1000000;
            while (ioread32(&dev->ring->head) != target_tail && --timeout > 0) {
                cpu_relax();
            }
            if (timeout == 0) {
                pr_err("vGPU-Core: Dynamic kernel load timed out!\n");
                return -ETIMEDOUT;
            }
            break;
        }

        default:
            return -ENOTTY;
    }
    return 0;
}
