#include "fpgagpu_umd.h"

CL_API_ENTRY cl_context CL_API_CALL
clCreateContext(const cl_context_properties *properties,
                cl_uint num_devices,
                const cl_device_id *devices,
                void (CL_CALLBACK *pfn_notify)(const char *, const void *, size_t, void *),
                void *user_data,
                cl_int *errcode_ret) {
    (void)properties;
    (void)pfn_notify;
    (void)user_data;

    if (num_devices == 0 || devices == NULL) {
        if (errcode_ret) *errcode_ret = CL_INVALID_VALUE;
        return NULL;
    }

    int fd = open("/dev/fpgagpu0", O_RDWR);
    if (fd < 0) {
        // Fallback to legacy device node
        fd = open("/dev/vgpu0", O_RDWR);
    }

    if (fd < 0) {
        perror("[UMD] Failed to open /dev/fpgagpu0 or /dev/vgpu0");
        if (errcode_ret) *errcode_ret = CL_DEVICE_NOT_AVAILABLE;
        return NULL;
    }

    // Verify driver version via ioctl
    struct fpgagpu_version_info ver = {0};
    if (ioctl(fd, FPGAGPU_IOC_GET_VERSION, &ver) == 0) {
        printf("[UMD] Connected to FPGA-GPU Hardware Bitstream v%u.%u\n",
               ver.major_version, ver.minor_version);
    }

    cl_context ctx = (cl_context)malloc(sizeof(struct _cl_context));
    if (!ctx) {
        close(fd);
        if (errcode_ret) *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    ctx->device = devices[0];
    ctx->device->fd = fd;
    ctx->fd = fd;
    ctx->ref_count = 1;

    fpgagpu_vram_init();

    if (errcode_ret) *errcode_ret = CL_SUCCESS;
    return ctx;
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainContext(cl_context context) {
    if (!context) return CL_INVALID_CONTEXT;
    context->ref_count++;
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseContext(cl_context context) {
    if (!context) return CL_INVALID_CONTEXT;
    context->ref_count--;
    if (context->ref_count == 0) {
        if (context->fd >= 0) {
            close(context->fd);
            context->fd = -1;
        }
        free(context);
    }
    return CL_SUCCESS;
}
