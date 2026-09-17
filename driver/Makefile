obj-m += fpgagpu_driver.o
fpgagpu_driver-objs := fpgagpu_main.o fpgagpu_ioctl.o fpgagpu_mmap.o fpgagpu_sched.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
