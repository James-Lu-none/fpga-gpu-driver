CC = gcc
CFLAGS = -Wall -O2

all: test_ioctl test_dma test_compute

test_ioctl: test_ioctl.c
	$(CC) $(CFLAGS) $< -o $@ -pthread

test_dma: test_dma.c
	$(CC) $(CFLAGS) $< -o $@

test_compute: test_compute.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f test_ioctl test_dma test_compute
