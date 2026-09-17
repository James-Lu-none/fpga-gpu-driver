CC ?= gcc
CFLAGS ?= -Wall -O2
CL_CFLAGS = $(CFLAGS) -I../include -I../include/CL
CL_LDFLAGS = -L../umd -lfpgagpu_opencl -Wl,-rpath,../umd -pthread

TESTS = test_ioctl test_dma test_opencl_buffer_copy test_opencl_vector_add test_opencl_matmul 

all: $(TESTS)

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) $< -o $@ -pthread

test_dma: test_dma.c
	$(CC) $(CFLAGS) $< -o $@

test_opencl_buffer_copy: test_opencl_buffer_copy.c
	$(CC) $(CL_CFLAGS) $< -o $@ $(CL_LDFLAGS)

test_opencl_vector_add: test_opencl_vector_add.c
	$(CC) $(CL_CFLAGS) $< -o $@ $(CL_LDFLAGS)

test_opencl_matmul: test_opencl_matmul.c
	$(CC) $(CL_CFLAGS) $< -o $@ $(CL_LDFLAGS)

clean:
	rm -f $(TESTS)

.PHONY: all clean
