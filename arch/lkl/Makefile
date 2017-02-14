include arch/lkl/auto.conf

KBUILD_CFLAGS += -fno-builtin

ifneq (,$(filter $(OUTPUT_FORMAT),elf64-x86-64 elf64-x86-64-freebsd elf32-littlearm ))
KBUILD_CFLAGS += -fPIC
else ifeq ($(OUTPUT_FORMAT),pe-i386)
prefix=_
# workaround for #include_next<stdarg.h> errors
LINUXINCLUDE := -isystem arch/lkl/include/system $(LINUXINCLUDE)
# workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=52991
KBUILD_CFLAGS += -mno-ms-bitfields
else
$(error Unrecognized platform: $(OUTPUT_FORMAT))
endif

ifeq ($(OUTPUT_FORMAT),elf32-littlearm)
CFLAGS_ABI      :=-mabi=aapcs-linux -mno-thumb-interwork -mfpu=vfp
KBUILD_CFLAGS += $(CFLAGS_ABI)
endif

ifneq (,$(filter $(OUTPUT_FORMAT),elf64-x86-64-freebsd))
NPROC=$(shell sysctl -n hw.ncpu)
else
NPROC=$(shell nproc)
endif

LDFLAGS_vmlinux += -r

ifeq ($(buildrump),yes)
# objcopy trick is taken from rumpkernel
GCCSYMBOLS=__umoddi3|__udivdi3|__aeabi_idiv|__aeabi_idivmod|__aeabi_llsl
GCCSYMBOLS:=$(GCCSYMBOLS)|__aeabi_llsr|__aeabi_uidiv|__aeabi_uidivmod|__aeabi_uldivmod
GCCATOMIC=__sync_synchronize|__sync_fetch_and_sub_4|__sync_fetch_and_add_4
GCCATOMIC:=$(GCCATOMIC)|__sync_fetch_and_and_4|__sync_fetch_and_or_4
VMLINUX_SYMS=__start___ex_table|__stop___ex_table|boot_cmdline
EXP_SYMRENAME=rump|RUMP|bmk_|lib_|nuse_|lkl_|__tls|__initcall_start
EXP_SYMRENAME:=$(EXP_SYMRENAME)|__initcall_end|__gcov|_end|_GLOBAL_OFFSET_TABLE|__assert13
EXP_SYMRENAME:=$(EXP_SYMRENAME)|${GCCSYMBOLS}|${GCCATOMIC}'${_SYMQUIRK}'
EXP_SYMRENAME:=$(EXP_SYMRENAME)${RUMP_SYM_NORENAME:D|${RUMP_SYM_NORENAME}}|${VMLINUX_SYMS}

define make_ns_symbols
	${Q}echo "  OBJCPY	" $1 $2;	\
	${NM} -go $1 | awk ' 	\
	$$NF!~/^'${_PQ}'(${EXP_SYMRENAME})/ \
	{s=$$NF;sub(/^'${_PQ}'/, "&rumpns_", s); print $$NF, s}'\
	| sort | uniq  > $2.renametab; \
	$(OBJCOPY) -R .eh_frame -L __start___ex_table \
	-L __stop___ex_table --preserve-dates \
	--redefine-syms $2.renametab $1 $2; \
	rm -f $2.renametab
endef
else
LKL_ENTRY_POINTS := lkl_start_kernel lkl_sys_halt lkl_syscall lkl_trigger_irq \
	lkl_get_free_irq lkl_put_irq lkl_create_syscall_thread \
	lkl_stop_syscall_thread lkl_is_running
define make_ns_symbols
	$(OBJCOPY) -R .eh_frame -R .syscall_defs $(foreach sym,$(LKL_ENTRY_POINTS),\
	-G$(prefix)$(sym)) vmlinux lkl.o
endef
endif

core-y += arch/lkl/kernel/
core-y += arch/lkl/mm/
core-y += arch/lkl/drivers/

all: lkl.o

lkl.o: vmlinux
	$(call make_ns_symbols,vmlinux,lkl.o)

arch/lkl/include/generated/uapi/asm/syscall_defs.h: vmlinux
	$(OBJCOPY) -j .syscall_defs -O binary --set-section-flags .syscall_defs=alloc $< $@
	sed -i 's/\x0//g' $@

install: lkl.o __headers arch/lkl/include/generated/uapi/asm/syscall_defs.h
	@echo "  INSTALL	$(INSTALL_PATH)/lib/lkl.o"
	@mkdir -p $(INSTALL_PATH)/lib/
	@cp lkl.o $(INSTALL_PATH)/lib/
	@$(srctree)/arch/lkl/scripts/headers_install.py \
		$(subst -j,-j$(NPROC),$(findstring -j,$(MAKEFLAGS))) \
		$(INSTALL_PATH)/include

archclean:
	$(Q)$(MAKE) $(clean)=$(boot)

define archhelp
  echo '  install	- Install library and headers to INSTALL_PATH/{lib,include}'
endef



