#include "fpgagpu_umd.h"

CL_API_ENTRY cl_mem CL_API_CALL
clCreateBuffer(cl_context context,
               cl_mem_flags flags,
               size_t size,
               void *host_ptr,
               cl_int *errcode_ret) {
    if (!context) {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return NULL;
    }
    if (size == 0) {
        if (errcode_ret) *errcode_ret = CL_INVALID_BUFFER_SIZE;
        return NULL;
    }

    uint64_t ddr3_addr = fpgagpu_vram_alloc(size);
    if (ddr3_addr == 0) {
        if (errcode_ret) *errcode_ret = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        return NULL;
    }

    cl_mem mem = (cl_mem)malloc(sizeof(struct _cl_mem));
    if (!mem) {
        if (errcode_ret) *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    mem->context = context;
    mem->flags = flags;
    mem->size = size;
    mem->host_ptr = host_ptr;
    mem->ddr3_addr = ddr3_addr;
    mem->ref_count = 1;

    // Handle CL_MEM_COPY_HOST_PTR
    if ((flags & CL_MEM_COPY_HOST_PTR) && host_ptr != NULL) {
        struct fpgagpu_dma_param dma = {
            .direction  = FPGAGPU_DMA_TO_DEVICE,
            .size       = (uint32_t)size,
            .ddr3_addr  = ddr3_addr,
            .host_vaddr = (uint64_t)(uintptr_t)host_ptr
        };
        if (ioctl(context->fd, FPGAGPU_IOC_DMA_TRANSFER, &dma) < 0) {
            perror("[UMD] clCreateBuffer: Initial DMA upload failed");
            free(mem);
            if (errcode_ret) *errcode_ret = CL_MEM_OBJECT_ALLOCATION_FAILURE;
            return NULL;
        }
    }

    if (errcode_ret) *errcode_ret = CL_SUCCESS;
    return mem;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseMemObject(cl_mem memobj) {
    if (!memobj) return CL_INVALID_MEM_OBJECT;
    memobj->ref_count--;
    if (memobj->ref_count == 0) {
        fpgagpu_vram_free(memobj->ddr3_addr, memobj->size);
        free(memobj);
    }
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteBuffer(cl_command_queue command_queue,
                     cl_mem buffer,
                     cl_bool blocking_write,
                     size_t offset,
                     size_t size,
                     const void *ptr,
                     cl_uint num_events_in_wait_list,
                     const cl_event *event_wait_list,
                     cl_event *event) {
    (void)blocking_write;
    (void)num_events_in_wait_list;
    (void)event_wait_list;

    if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
    if (!buffer) return CL_INVALID_MEM_OBJECT;
    if (!ptr) return CL_INVALID_VALUE;
    if (offset + size > buffer->size) return CL_INVALID_VALUE;

    struct fpgagpu_dma_param dma = {
        .direction  = FPGAGPU_DMA_TO_DEVICE,
        .size       = (uint32_t)size,
        .ddr3_addr  = buffer->ddr3_addr + offset,
        .host_vaddr = (uint64_t)(uintptr_t)ptr
    };

    if (ioctl(command_queue->context->fd, FPGAGPU_IOC_DMA_TRANSFER, &dma) < 0) {
        perror("[UMD] clEnqueueWriteBuffer DMA transfer failed");
        return CL_OUT_OF_RESOURCES;
    }

    if (event) {
        cl_event ev = (cl_event)malloc(sizeof(struct _cl_event));
        if (ev) {
            ev->context = command_queue->context;
            ev->queue = command_queue;
            ev->status = CL_COMPLETE;
            ev->ref_count = 1;
            *event = ev;
        }
    }

    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadBuffer(cl_command_queue command_queue,
                    cl_mem buffer,
                    cl_bool blocking_read,
                    size_t offset,
                    size_t size,
                    void *ptr,
                    cl_uint num_events_in_wait_list,
                    const cl_event *event_wait_list,
                    cl_event *event) {
    (void)blocking_read;
    (void)num_events_in_wait_list;
    (void)event_wait_list;

    if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
    if (!buffer) return CL_INVALID_MEM_OBJECT;
    if (!ptr) return CL_INVALID_VALUE;
    if (offset + size > buffer->size) return CL_INVALID_VALUE;

    struct fpgagpu_dma_param dma = {
        .direction  = FPGAGPU_DMA_FROM_DEVICE,
        .size       = (uint32_t)size,
        .ddr3_addr  = buffer->ddr3_addr + offset,
        .host_vaddr = (uint64_t)(uintptr_t)ptr
    };

    if (ioctl(command_queue->context->fd, FPGAGPU_IOC_DMA_TRANSFER, &dma) < 0) {
        perror("[UMD] clEnqueueReadBuffer DMA transfer failed");
        return CL_OUT_OF_RESOURCES;
    }

    if (event) {
        cl_event ev = (cl_event)malloc(sizeof(struct _cl_event));
        if (ev) {
            ev->context = command_queue->context;
            ev->queue = command_queue;
            ev->status = CL_COMPLETE;
            ev->ref_count = 1;
            *event = ev;
        }
    }

    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBuffer(cl_command_queue command_queue,
                    cl_mem src_buffer,
                    cl_mem dst_buffer,
                    size_t src_offset,
                    size_t dst_offset,
                    size_t size,
                    cl_uint num_events_in_wait_list,
                    const cl_event *event_wait_list,
                    cl_event *event) {
    (void)num_events_in_wait_list;
    (void)event_wait_list;

    if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
    if (!src_buffer || !dst_buffer) return CL_INVALID_MEM_OBJECT;
    if (src_offset + size > src_buffer->size) return CL_INVALID_VALUE;
    if (dst_offset + size > dst_buffer->size) return CL_INVALID_VALUE;

    // Buffer copy via page-aligned bounce buffer
    void *bounce = NULL;
    if (posix_memalign(&bounce, 4096, size) != 0) {
        return CL_OUT_OF_HOST_MEMORY;
    }

    // Step 1: Read src -> bounce (D2H)
    struct fpgagpu_dma_param dma_read = {
        .direction  = FPGAGPU_DMA_FROM_DEVICE,
        .size       = (uint32_t)size,
        .ddr3_addr  = src_buffer->ddr3_addr + src_offset,
        .host_vaddr = (uint64_t)(uintptr_t)bounce
    };
    if (ioctl(command_queue->context->fd, FPGAGPU_IOC_DMA_TRANSFER, &dma_read) < 0) {
        free(bounce);
        return CL_OUT_OF_RESOURCES;
    }

    // Step 2: Write bounce -> dst (H2D)
    struct fpgagpu_dma_param dma_write = {
        .direction  = FPGAGPU_DMA_TO_DEVICE,
        .size       = (uint32_t)size,
        .ddr3_addr  = dst_buffer->ddr3_addr + dst_offset,
        .host_vaddr = (uint64_t)(uintptr_t)bounce
    };
    if (ioctl(command_queue->context->fd, FPGAGPU_IOC_DMA_TRANSFER, &dma_write) < 0) {
        free(bounce);
        return CL_OUT_OF_RESOURCES;
    }

    free(bounce);

    if (event) {
        cl_event ev = (cl_event)malloc(sizeof(struct _cl_event));
        if (ev) {
            ev->context = command_queue->context;
            ev->queue = command_queue;
            ev->status = CL_COMPLETE;
            ev->ref_count = 1;
            *event = ev;
        }
    }

    return CL_SUCCESS;
}
