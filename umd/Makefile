# FPGA-GPU OpenCL User Mode Driver (UMD) Makefile
# Builds libOpenCL.so and libfpgagpu_opencl.so

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC -pthread -I../include -I../include/CL -Isrc
LDFLAGS ?= -shared -pthread

TARGET_NAME = libfpgagpu_opencl.so
LIB_OPENCL  = libOpenCL.so

SRCS = src/cl_platform.c \
       src/cl_context.c \
       src/cl_queue.c \
       src/cl_mem.c \
       src/cl_program.c \
       src/cl_kernel.c \
       src/cl_enqueue.c \
       src/vram_alloc.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET_NAME) $(LIB_OPENCL)

$(TARGET_NAME): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(LIB_OPENCL): $(TARGET_NAME)
	ln -sf $(TARGET_NAME) $(LIB_OPENCL)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET_NAME) $(LIB_OPENCL)

.PHONY: all clean
