CC ?= gcc
CFLAGS ?= -Wall -O2
CL_CFLAGS = $(CFLAGS) -I../include -I../include/CL
TEST_COMMON = raw_data.c
TEST_COMMON_HEADERS = raw_data.h
CL_LDFLAGS = -L../umd -lfpgagpu_opencl -Wl,-rpath,../umd -pthread

TESTS = test_ioctl test_dma test_dma_concurrent test_opencl_multiprocess_memory test_multiprocess_kernel test_opencl_buffer_copy test_opencl_vector_add test_opencl_matmul test_opencl_matmul_occupancy test_opencl_branch_divergence

all: $(TESTS)

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) $< -o $@ -pthread

test_dma: test_dma.c test_dma_concurrent.c test_dma_concurrent.h ../include/uapi/fpgagpu_ioctl.h
	$(CC) $(CFLAGS) -DFPGAGPU_DMA_CONCURRENT_EMBEDDED test_dma.c test_dma_concurrent.c -o $@

test_opencl_buffer_copy: test_opencl_buffer_copy.c $(TEST_COMMON) $(TEST_COMMON_HEADERS) run_sibling_test.h test_opencl_multiprocess_memory
	$(CC) $(CL_CFLAGS) $< $(TEST_COMMON) -o $@ $(CL_LDFLAGS)

test_opencl_vector_add: test_opencl_vector_add.c kernel_source.h $(TEST_COMMON) $(TEST_COMMON_HEADERS) run_sibling_test.h test_multiprocess_kernel
	$(CC) $(CL_CFLAGS) $< $(TEST_COMMON) -o $@ $(CL_LDFLAGS)

test_opencl_matmul: test_opencl_matmul.c kernel_source.h $(TEST_COMMON) $(TEST_COMMON_HEADERS)
	$(CC) $(CL_CFLAGS) $< $(TEST_COMMON) -o $@ $(CL_LDFLAGS)

test_opencl_matmul_occupancy: test_opencl_matmul_occupancy.c kernel_source.h $(TEST_COMMON) $(TEST_COMMON_HEADERS)
	$(CC) $(CL_CFLAGS) $< $(TEST_COMMON) -o $@ $(CL_LDFLAGS)

clean:
	rm -f $(TESTS)

.PHONY: all clean

test_opencl_branch_divergence: test_opencl_branch_divergence.c kernel_source.h $(TEST_COMMON) $(TEST_COMMON_HEADERS)
	$(CC) $(CL_CFLAGS) $< $(TEST_COMMON) -o $@ $(CL_LDFLAGS)

# Standalone entry point for focused concurrency tests.
test_dma_concurrent: test_dma_concurrent.c test_dma_concurrent.h ../include/uapi/fpgagpu_ioctl.h
	$(CC) $(CFLAGS) $< -o $@

test_opencl_multiprocess_memory: test_opencl_multiprocess_memory.c multiprocess_test.h
	$(CC) $(CL_CFLAGS) $< -o $@ $(CL_LDFLAGS)

test_multiprocess_kernel: test_multiprocess_kernel.c multiprocess_test.h kernel_source.h ../umd/src/fpgagpu_umd.h ../include/uapi/fpgagpu_ioctl.h
	$(CC) $(CL_CFLAGS) $< -o $@ $(CL_LDFLAGS)
