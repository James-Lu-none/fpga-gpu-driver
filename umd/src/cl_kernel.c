#include "fpgagpu_umd.h"

CL_API_ENTRY cl_kernel CL_API_CALL
clCreateKernel(cl_program program,
               const char *kernel_name,
               cl_int *errcode_ret) {
    if (!program) {
        if (errcode_ret) *errcode_ret = CL_INVALID_PROGRAM;
        return NULL;
    }
    if (!program->is_built) {
        if (errcode_ret) *errcode_ret = CL_INVALID_PROGRAM_EXECUTABLE;
        return NULL;
    }
    if (!kernel_name) {
        if (errcode_ret) *errcode_ret = CL_INVALID_VALUE;
        return NULL;
    }

    cl_kernel k = (cl_kernel)malloc(sizeof(struct _cl_kernel));
    if (!k) {
        if (errcode_ret) *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    memset(k, 0, sizeof(struct _cl_kernel));
    k->program = program;
    k->name = strdup(kernel_name);
    k->ref_count = 1;

    if (errcode_ret) *errcode_ret = CL_SUCCESS;
    return k;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseKernel(cl_kernel kernel) {
    if (!kernel) return CL_INVALID_KERNEL;
    kernel->ref_count--;
    if (kernel->ref_count == 0) {
        if (kernel->name) free(kernel->name);
        free(kernel);
    }
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clSetKernelArg(cl_kernel kernel,
               cl_uint arg_index,
               size_t arg_size,
               const void *arg_value) {
    if (!kernel) return CL_INVALID_KERNEL;
    if (arg_index >= FPGAGPU_MAX_ARGS) return CL_INVALID_ARG_INDEX;

    if (!arg_value) {
        // Null pointer for local memory (not used in minimal pipeline)
        return CL_INVALID_ARG_VALUE;
    }

    kernel->args[arg_index].size = arg_size;
    kernel->args[arg_index].is_set = true;

    // Check if argument is cl_mem (buffer object pointer)
    if (arg_size == sizeof(cl_mem)) {
        cl_mem mem = *(cl_mem *)arg_value;
        kernel->args[arg_index].is_mem = true;
        kernel->args[arg_index].mem_obj = mem;
        kernel->args[arg_index].scalar_val = (uint32_t)mem->ddr3_addr;
    } else {
        // Scalar value (int, float, etc.)
        kernel->args[arg_index].is_mem = false;
        kernel->args[arg_index].mem_obj = NULL;
        uint32_t val = 0;
        if (arg_size <= sizeof(uint32_t)) {
            memcpy(&val, arg_value, arg_size);
        }
        kernel->args[arg_index].scalar_val = val;
    }

    if (arg_index >= kernel->num_args) {
        kernel->num_args = arg_index + 1;
    }

    return CL_SUCCESS;
}
