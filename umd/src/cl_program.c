#include "fpgagpu_umd.h"

CL_API_ENTRY cl_program CL_API_CALL
clCreateProgramWithSource(cl_context context,
                          cl_uint count,
                          const char **strings,
                          const size_t *lengths,
                          cl_int *errcode_ret) {
    if (!context) {
        if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT;
        return NULL;
    }
    if (count == 0 || strings == NULL) {
        if (errcode_ret) *errcode_ret = CL_INVALID_VALUE;
        return NULL;
    }

    size_t total_len = 0;
    for (cl_uint i = 0; i < count; i++) {
        if (strings[i] == NULL) {
            if (errcode_ret) *errcode_ret = CL_INVALID_VALUE;
            return NULL;
        }
        total_len += (lengths && lengths[i] > 0) ? lengths[i] : strlen(strings[i]);
    }

    cl_program prog = (cl_program)malloc(sizeof(struct _cl_program));
    if (!prog) {
        if (errcode_ret) *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    prog->source = (char *)malloc(total_len + 1);
    if (!prog->source) {
        free(prog);
        if (errcode_ret) *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    size_t offset = 0;
    for (cl_uint i = 0; i < count; i++) {
        size_t len = (lengths && lengths[i] > 0) ? lengths[i] : strlen(strings[i]);
        memcpy(prog->source + offset, strings[i], len);
        offset += len;
    }
    prog->source[offset] = '\0';
    prog->source_len = total_len;
    prog->context = context;
    prog->binary_words = NULL;
    prog->num_instructions = 0;
    prog->is_built = false;
    prog->ref_count = 1;

    if (errcode_ret) *errcode_ret = CL_SUCCESS;
    return prog;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseProgram(cl_program program) {
    if (!program) return CL_INVALID_PROGRAM;
    program->ref_count--;
    if (program->ref_count == 0) {
        if (program->source) free(program->source);
        if (program->binary_words) free(program->binary_words);
        free(program);
    }
    return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clBuildProgram(cl_program program,
               cl_uint num_devices,
               const cl_device_id *device_list,
               const char *options,
               void (CL_CALLBACK *pfn_notify)(cl_program program, void *user_data),
               void *user_data) {
    (void)num_devices;
    (void)device_list;
    (void)options;
    (void)pfn_notify;
    (void)user_data;

    if (!program) return CL_INVALID_PROGRAM;

    // 1. Write kernel source to temporary file
    pid_t pid = getpid();
    static int counter = 0;
    char src_tmpl[256];
    snprintf(src_tmpl, sizeof(src_tmpl), "/tmp/fpgagpu_kernel_%d_%d.c", (int)pid, counter++);
    int fd = open(src_tmpl, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("[UMD] open temp source file failed");
        return CL_COMPILER_NOT_AVAILABLE;
    }

    // Prepend device_intrinsics.h include if not present
    const char *header_include = "#include \"device_intrinsics.h\"\n";
    if (strstr(program->source, "device_intrinsics.h") == NULL) {
        if (write(fd, header_include, strlen(header_include)) < 0) {
            close(fd);
            unlink(src_tmpl);
            return CL_BUILD_PROGRAM_FAILURE;
        }
    }
    if (write(fd, program->source, program->source_len) < 0) {
        close(fd);
        unlink(src_tmpl);
        return CL_BUILD_PROGRAM_FAILURE;
    }
    close(fd);

    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s.bin", src_tmpl);

    // 2. Locate compiler driver script
    const char *compiler_dir = getenv("FPGAGPU_COMPILER_DIR");
    char compiler_script[512];
    if (compiler_dir) {
        snprintf(compiler_script, sizeof(compiler_script), "%s/scripts/fpgagpu-clang", compiler_dir);
    } else {
        // Look relative to workspace or known default
        snprintf(compiler_script, sizeof(compiler_script), "/home/user/workspace/fpga-gpu/fpga-gpu-compiler/scripts/fpgagpu-clang");
    }

    // 3. Compile kernel to raw binary machine code (.bin)
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s -b %s -o %s > /dev/null 2>&1", compiler_script, src_tmpl, bin_path);
    printf("[UMD] Compiling OpenCL C Kernel via LLVM Backend: %s\n", compiler_script);

    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[UMD] LLVM Kernel compilation failed!\n");
        unlink(src_tmpl);
        unlink(bin_path);
        return CL_BUILD_PROGRAM_FAILURE;
    }

    // 4. Read binary file into memory
    FILE *bf = fopen(bin_path, "rb");
    if (!bf) {
        perror("[UMD] fopen compiled kernel binary");
        unlink(src_tmpl);
        unlink(bin_path);
        return CL_BUILD_PROGRAM_FAILURE;
    }

    fseek(bf, 0, SEEK_END);
    long fsize = ftell(bf);
    fseek(bf, 0, SEEK_SET);

    if (fsize <= 0 || (fsize % 4) != 0) {
        fprintf(stderr, "[UMD] Invalid binary machine code size (%ld bytes)\n", fsize);
        fclose(bf);
        unlink(src_tmpl);
        unlink(bin_path);
        return CL_BUILD_PROGRAM_FAILURE;
    }

    if (program->binary_words) free(program->binary_words);
    program->binary_words = (uint32_t *)malloc(fsize);
    if (!program->binary_words) {
        fclose(bf);
        unlink(src_tmpl);
        unlink(bin_path);
        return CL_OUT_OF_HOST_MEMORY;
    }

    if (fread(program->binary_words, 1, fsize, bf) != (size_t)fsize) {
        perror("[UMD] fread kernel binary failed");
        fclose(bf);
        unlink(src_tmpl);
        unlink(bin_path);
        return CL_BUILD_PROGRAM_FAILURE;
    }

    program->num_instructions = (uint32_t)(fsize / 4);
    program->is_built = true;

    fclose(bf);
    unlink(src_tmpl);
    unlink(bin_path);

    printf("[UMD] Kernel built successfully: %u ISA instructions (%ld bytes)\n",
           program->num_instructions, fsize);
    return CL_SUCCESS;
}
