ifeq ($(KERNEL_ROOT),)
    KERNEL_ROOT = ~/kernel/linux-3.14.3-iocp
endif

all: test2

test2: test2.c
	gcc -o test2 -idirafter $(KERNEL_ROOT) -pthread test2.c -lrt

clean:
	rm -f test2 test2.o

