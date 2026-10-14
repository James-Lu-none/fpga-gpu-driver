CC ?= gcc
CFLAGS ?= -Wall -O2
CL_CFLAGS = $(CFLAGS) -I../include -I../include/CL
TEST_COMMON = raw_data.c
TEST_COMMON_HEADERS = raw_data.h
CL_LDFLAGS = -L../umd -lfpgagpu_opencl -Wl,-rpath,../umd -pthread

TESTS = test_ioctl test_dma test_opencl_buffer_copy test_opencl_vector_add test_opencl_matmul test_opencl_matmul_occupancy test_opencl_branch_divergence

all: $(TESTS)

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) $< -o $@ -pthread

test_dma: test_dma.c
	$(CC) $(CFLAGS) $< -o $@

test_opencl_buffer_copy: test_opencl_buffer_copy.c $(TEST_COMMON) $(TEST_COMMON_HEADERS)
	$(CC) $(CL_CFLAGS) $< $(TEST_COMMON) -o $@ $(CL_LDFLAGS)

test_opencl_vector_add: test_opencl_vector_add.c kernel_source.h $(TEST_COMMON) $(TEST_COMMON_HEADERS)
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
