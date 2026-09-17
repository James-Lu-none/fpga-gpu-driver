#include "fpgagpu_umd.h"

static struct _cl_platform_id g_platform = {
    .name = "FPGA-GPU OpenCL Platform",
    .vendor = "FPGA-GPU Open Source",
    .version = "OpenCL 1.2 FPGA-GPU 1.0",
    .profile = "EMBEDDED_PROFILE"
};

static struct _cl_device_id g_device = {
    .platform = &g_platform,
    .name = "Artix-7 SIMT GPGPU Accelerator",
    .vendor = "FPGA-GPU",
    .type = CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR,
    .max_compute_units = 2,
    .max_work_item_dimensions = 3,
    .max_work_item_sizes = {32, 32, 1},
    .max_work_group_size = 32,
    .global_mem_size = FPGAGPU_VRAM_TOTAL_SIZE,
    .max_mem_alloc_size = 32ULL * 1024ULL * 1024ULL, // 32MB max per transfer
    .address_bits = 32,
    .fd = -1
};

CL_API_ENTRY cl_int CL_API_CALL
clGetPlatformIDs(cl_uint num_entries, cl_platform_id *platforms, cl_uint *num_platforms) {
    if (platforms == NULL && num_platforms == NULL) return CL_INVALID_VALUE;
    if (num_entries == 0 && platforms != NULL) return CL_INVALID_VALUE;

    if (num_platforms) {
        *num_platforms = 1;
    }
    if (platforms && num_entries >= 1) {
        platforms[0] = &g_platform;
    }
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetPlatformInfo(cl_platform_id platform, cl_platform_info param_name,
                  size_t param_value_size, void *param_value, size_t *param_value_size_ret) {
    if (platform != &g_platform) return CL_INVALID_PLATFORM;

    const char *str = NULL;
    switch (param_name) {
        case CL_PLATFORM_PROFILE: str = g_platform.profile; break;
        case CL_PLATFORM_VERSION: str = g_platform.version; break;
        case CL_PLATFORM_NAME:    str = g_platform.name; break;
        case CL_PLATFORM_VENDOR:  str = g_platform.vendor; break;
        case CL_PLATFORM_EXTENSIONS: str = ""; break;
        default: return CL_INVALID_VALUE;
    }

    size_t len = strlen(str) + 1;
    if (param_value_size_ret) *param_value_size_ret = len;
    if (param_value) {
        if (param_value_size < len) return CL_INVALID_VALUE;
        memcpy(param_value, str, len);
    }
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetDeviceIDs(cl_platform_id platform, cl_device_type device_type,
               cl_uint num_entries, cl_device_id *devices, cl_uint *num_devices) {
    (void)platform;
    if ((device_type & (CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR | CL_DEVICE_TYPE_DEFAULT)) == 0) {
        return CL_DEVICE_NOT_FOUND;
    }
    if (num_devices) *num_devices = 1;
    if (devices && num_entries >= 1) {
        devices[0] = &g_device;
    }
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetDeviceInfo(cl_device_id device, cl_device_info param_name,
                size_t param_value_size, void *param_value, size_t *param_value_size_ret) {
    if (device != &g_device) return CL_INVALID_DEVICE;

#define COPY_VAL(type, val) do { \
        if (param_value_size_ret) *param_value_size_ret = sizeof(type); \
        if (param_value) { \
            if (param_value_size < sizeof(type)) return CL_INVALID_VALUE; \
            *(type *)param_value = (val); \
        } \
        return CL_SUCCESS; \
    } while (0)

#define COPY_STR(str) do { \
        size_t _len = strlen(str) + 1; \
        if (param_value_size_ret) *param_value_size_ret = _len; \
        if (param_value) { \
            if (param_value_size < _len) return CL_INVALID_VALUE; \
            memcpy(param_value, str, _len); \
        } \
        return CL_SUCCESS; \
    } while (0)

    switch (param_name) {
        case CL_DEVICE_NAME: COPY_STR(g_device.name);
        case CL_DEVICE_VENDOR: COPY_STR(g_device.vendor);
        case CL_DEVICE_TYPE: COPY_VAL(cl_device_type, g_device.type);
        case CL_DEVICE_MAX_COMPUTE_UNITS: COPY_VAL(cl_uint, g_device.max_compute_units);
        case CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS: COPY_VAL(cl_uint, g_device.max_work_item_dimensions);
        case CL_DEVICE_MAX_WORK_GROUP_SIZE: COPY_VAL(size_t, g_device.max_work_group_size);
        case CL_DEVICE_GLOBAL_MEM_SIZE: COPY_VAL(cl_ulong, g_device.global_mem_size);
        case CL_DEVICE_MAX_MEM_ALLOC_SIZE: COPY_VAL(cl_ulong, g_device.max_mem_alloc_size);
        case CL_DEVICE_ADDRESS_BITS: COPY_VAL(cl_uint, g_device.address_bits);
        case CL_DEVICE_AVAILABLE: COPY_VAL(cl_bool, CL_TRUE);
        case CL_DEVICE_COMPILER_AVAILABLE: COPY_VAL(cl_bool, CL_TRUE);
        default: return CL_INVALID_VALUE;
    }
}
