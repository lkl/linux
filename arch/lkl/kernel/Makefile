extra-y := vmlinux.lds
extra-y += vmlinux-fuzzing-postlink.lds

KASAN_SANITIZE_init.o := n
KASAN_SANITIZE_stacktrace.o := n

obj-y = setup.o threads.o irq.o time.o syscalls.o misc.o console.o \
	syscalls_32.o cpu.o init.o stacktrace.o

obj-$(CONFIG_LKL_IRQ_KUNIT_TEST) += irq_test.o
