ifeq ($(KERNEL_ROOT),)
    KERNEL_ROOT = ~/kernel/linux-3.14.3-iocp
endif

all: test1

test1: test1.c
	gcc -o test1 -idirafter $(KERNEL_ROOT) -pthread test1.c -lrt

clean:
	rm -f test1 test1.o

