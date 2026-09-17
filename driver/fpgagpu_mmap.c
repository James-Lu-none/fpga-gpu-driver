#include "../include/kmd/fpgagpu_core.h"

int fpgagpu_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct fpgagpu_context *ctx = file->private_data;
    struct fpgagpu_dev *dev = ctx->dev;
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    pr_info("fpgagpu-Core: mmap called on fpgagpu%d, size: %zu, pgoff: %lu\n", dev->minor, size, vma->vm_pgoff);

    if (vma->vm_pgoff == 0) {
        /*
         * Offset 0: Map the Command Queue (Ring Buffer)
         * This uses Consistent DMA mapping (uncached).
         */
        if (size > PAGE_SIZE) return -EINVAL;

        return dma_mmap_coherent(&dev->pci_dev->dev, vma, 
                                    dev->ring_buffer, 
                                    dev->dma_handle, size);
    }

    return -EINVAL;
}
