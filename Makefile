obj-m += ouichefs.o
ouichefs-objs := fs.o super.o inode.o file.o dir.o ouichefs_interface.o

KERNELDIR ?= $(find ~/ -type d -name linux-6.5.7)

all:
	make -C $(KERNELDIR) M=$(PWD) modules

debug:
	make -C $(KERNELDIR) M=$(PWD) ccflags-y+="-DDEBUG -g" modules

clean:
	make -C $(KERNELDIR) M=$(PWD) clean
	rm -rf *~

.PHONY: all clean
