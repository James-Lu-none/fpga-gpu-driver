#include "fpgagpu_umd.h"

CL_API_ENTRY cl_command_queue CL_API_CALL
clCreateCommandQueue(cl_context context,
                     cl_device_id device,
                     cl_command_queue_properties properties,
                     cl_int *errcode_ret) {
    if (!context) {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return NULL;
    }

    cl_command_queue queue = (cl_command_queue)malloc(sizeof(struct _cl_command_queue));
    if (!queue) {
        if (errcode_ret) *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    queue->context = context;
    queue->device = device;
    queue->properties = properties;
    queue->ref_count = 1;

    if (errcode_ret) *errcode_ret = CL_SUCCESS;
    return queue;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseCommandQueue(cl_command_queue command_queue) {
    if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
    command_queue->ref_count--;
    if (command_queue->ref_count == 0) {
        free(command_queue);
    }
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clFinish(cl_command_queue command_queue) {
    if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
    
    // Wait for all submitted tasks to complete by ringing doorbell / waiting for queue empty
    if (ioctl(command_queue->context->fd, FPGAGPU_IOC_DOORBELL, 0) < 0) {
        perror("[UMD] clFinish: Doorbell wait failed");
        return CL_OUT_OF_RESOURCES;
    }
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clFlush(cl_command_queue command_queue) {
    if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
    return CL_SUCCESS;
}
