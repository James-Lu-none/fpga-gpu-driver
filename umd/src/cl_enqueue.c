#include "fpgagpu_umd.h"

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueNDRangeKernel(cl_command_queue command_queue,
                       cl_kernel kernel,
                       cl_uint work_dim,
                       const size_t *global_work_offset,
                       const size_t *global_work_size,
                       const size_t *local_work_size,
                       cl_uint num_events_in_wait_list,
                       const cl_event *event_wait_list,
                       cl_event *event) {
    (void)global_work_offset;
    (void)num_events_in_wait_list;
    (void)event_wait_list;

    if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
    if (!kernel) return CL_INVALID_KERNEL;
    if (!kernel->program || !kernel->program->is_built) return CL_INVALID_PROGRAM_EXECUTABLE;
    if (work_dim == 0 || work_dim > 3 || !global_work_size) return CL_INVALID_WORK_DIMENSION;

    int fd = command_queue->context->fd;

    // Step 1: Marshal kernel arguments into 4KB aligned Kernarg buffer
    uint32_t *kernarg_buf = NULL;
    if (posix_memalign((void **)&kernarg_buf, 4096, FPGAGPU_KERNARG_SIZE) != 0) {
        return CL_OUT_OF_HOST_MEMORY;
    }
    memset(kernarg_buf, 0, FPGAGPU_KERNARG_SIZE);

    for (cl_uint i = 0; i < kernel->num_args; i++) {
        if (!kernel->args[i].is_set) {
            fprintf(stderr, "[UMD] Error: Kernel argument %u was not set!\n", i);
            free(kernarg_buf);
            return CL_INVALID_KERNEL_ARGS;
        }
        kernarg_buf[i * 2 + 0] = kernel->args[i].scalar_val;
        kernarg_buf[i * 2 + 1] = kernel->args[i].scalar_val;
        printf("[UMD] Kernarg[%u] = 0x%08x (lane0/lane1 offset 0x%x/0x%x)\n",
               i, kernel->args[i].scalar_val, (i * 8), (i * 8 + 4));
    }

    // Step 2: Upload Kernarg Buffer to DDR3 at FPGAGPU_KERNARG_BASE (0x00000000)
    struct fpgagpu_dma_param dma_upload = {
        .direction  = FPGAGPU_DMA_TO_DEVICE,
        .size       = FPGAGPU_KERNARG_SIZE,
        .ddr3_addr  = FPGAGPU_KERNARG_BASE,
        .host_vaddr = (uint64_t)(uintptr_t)kernarg_buf
    };

    if (ioctl(fd, FPGAGPU_IOC_DMA_TRANSFER, &dma_upload) < 0) {
        perror("[UMD] Failed to upload Kernarg buffer via DMA");
        free(kernarg_buf);
        return CL_OUT_OF_RESOURCES;
    }
    free(kernarg_buf);

    // Step 3: Load Compiled Machine Code into GPU I-RAM
    struct fpgagpu_kernel_binary kbin = {
        .user_instr_ptr = (uint64_t)(uintptr_t)kernel->program->binary_words,
        .instr_size     = kernel->program->num_instructions,
        .kernel_id      = 1
    };

    if (ioctl(fd, FPGAGPU_IOC_LOAD_KERNEL, &kbin) < 0) {
        perror("[UMD] Failed to load kernel into GPU I-RAM");
        return CL_OUT_OF_RESOURCES;
    }

    // Step 4: Calculate Grid & Block (Workgroup) Dimensions
    // Hardware baseline: 2 lanes per warp/block in physical SIMD datapath
    uint32_t block_x = (local_work_size && local_work_size[0] > 0) ? (uint32_t)local_work_size[0] : 2;
    uint32_t block_y = (work_dim > 1 && local_work_size && local_work_size[1] > 0) ? (uint32_t)local_work_size[1] : 1;

    uint32_t grid_x = (uint32_t)((global_work_size[0] + block_x - 1) / block_x);
    uint32_t grid_y = (work_dim > 1) ? (uint32_t)((global_work_size[1] + block_y - 1) / block_y) : 1;

    uint32_t total_elements = (uint32_t)(global_work_size[0] * (work_dim > 1 ? global_work_size[1] : 1));

    printf("[UMD] Enqueuing Kernel '%s': Grid=(%u, %u), Block=(%u, %u), Total Items=%u\n",
           kernel->name, grid_x, grid_y, block_x, block_y, total_elements);

    // Step 5: Submit Task Launch Command to BRAM Ring Buffer
    struct fpgagpu_launch_cmd cmd = {
        .opcode       = FPGAGPU_OPCODE_LAUNCH_KERNEL,
        .grid_dim_x   = grid_x,
        .grid_dim_y   = grid_y,
        .block_dim_x  = block_x,
        .block_dim_y  = block_y,
        .dma_src_addr = FPGAGPU_KERNARG_BASE,
        .dma_dst_addr = 0,
        .num_elements = total_elements
    };

    if (ioctl(fd, FPGAGPU_IOC_SUBMIT_CMD, &cmd) < 0) {
        perror("[UMD] Failed to submit kernel launch command");
        return CL_OUT_OF_RESOURCES;
    }

    // Step 6: Wait for PicoRV32 + Hardware SMs Execution via Doorbell
    if (ioctl(fd, FPGAGPU_IOC_DOORBELL, 0) < 0) {
        perror("[UMD] Doorbell wait failed");
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
clWaitForEvents(cl_uint num_events, const cl_event *event_list) {
    (void)num_events;
    (void)event_list;
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseEvent(cl_event event) {
    if (!event) return CL_INVALID_EVENT;
    event->ref_count--;
    if (event->ref_count == 0) {
        free(event);
    }
    return CL_SUCCESS;
}
