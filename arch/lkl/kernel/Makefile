SYSCALL_DEFS_HDR := ../include/generated/uapi/asm/syscall_defs.h
UNISTD_HDR = arch/lkl/include/asm/unistd.h

extra-y := vmlinux.lds $(SYSCALL_DEFS_HDR)
obj-y = setup.o threads.o irq.o time.o syscalls.o misc.o mem.o console.o


quiet_cmd_syscall_def = CALL    $<
      cmd_syscall_def = $< sysdef $(UNISTD_HDR) arch/lkl/include/generated/uapi/asm/syscall_defs.h

arch/lkl/kernel/$(SYSCALL_DEFS_HDR): arch/lkl/scripts/make_syscall_tbl.sh $(UNISTD_HDR)
	@mkdir -p arch/lkl/include/generated/uapi/asm/
	$(call cmd,syscall_def)
