
M=$(shell pwd)
KERNELDIR=/lib/modules/`uname -r`/source

obj-m := i2c-user.o

compile:
	make -C $(KERNELDIR) M=$(M) modules 

.PHONY: compile
