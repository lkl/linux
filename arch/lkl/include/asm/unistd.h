#include <uapi/asm/unistd.h>

#define __NR_create_syscall_thread	__NR_arch_specific_syscall

__SYSCALL(__NR_create_syscall_thread, sys_create_syscall_thread)

#define __SC_ASCII(t, a) #t "," #a

#define __ASCII_MAP0(m,...)
#define __ASCII_MAP1(m,t,a) m(t,a)
#define __ASCII_MAP2(m,t,a,...) m(t,a) "," __ASCII_MAP1(m,__VA_ARGS__)
#define __ASCII_MAP3(m,t,a,...) m(t,a) "," __ASCII_MAP2(m,__VA_ARGS__)
#define __ASCII_MAP4(m,t,a,...) m(t,a) "," __ASCII_MAP3(m,__VA_ARGS__)
#define __ASCII_MAP5(m,t,a,...) m(t,a) "," __ASCII_MAP4(m,__VA_ARGS__)
#define __ASCII_MAP6(m,t,a,...) m(t,a) "," __ASCII_MAP5(m,__VA_ARGS__)
#define __ASCII_MAP(n,...) __ASCII_MAP##n(__VA_ARGS__)

#ifdef __MINGW32__
#define SECTION_ATTRS "n0"
#else
#define SECTION_ATTRS "a"
#endif

#define __SYSCALL_DEFINE_ARCH(x, name, ...)				\
	asm(".section .syscall_defs,\"" SECTION_ATTRS "\"\n"		\
	    ".ascii \"#ifdef __NR" #name "\\n\"\n"			\
	    ".ascii \"SYSCALL_DEFINE" #x "(" #name ","			\
	    __ASCII_MAP(x, __SC_ASCII, __VA_ARGS__) ")\\n\"\n"		\
	    ".ascii \"#endif\\n\"\n"					\
	    ".section .text\n");
