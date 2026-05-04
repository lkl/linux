#include <lkl_host.h>

#if defined(__x86_64__) && defined(_WIN64)
/*
 * Custom non-SEH setjmp/longjmp for x86_64 Windows.
 *
 * Windows x64 longjmp uses RtlUnwindEx which walks the stack frame-by-frame.
 * LKL uses longjmp for cooperative context switching between different stacks
 * (each kernel thread has its own malloc'd stack). RtlUnwindEx cannot handle
 * cross-stack jumps, causing stack overflow during unwinding.
 * We bypass SEH entirely by doing a raw register save/restore.
 *
 * jmp_buf layout (16 bytes aligned):
 *   [0]  rbx
 *   [1]  rbp
 *   [2]  r12
 *   [3]  r13
 *   [4]  r14
 *   [5]  r15
 *   [6]  rsp (after return from setjmp)
 *   [7]  rip (return address)
 *   [8]  rdi
 *   [9]  rsi
 */

static inline int lkl_setjmp(void *buf)
{
	int ret;
	__asm__ __volatile__ (
		"movq %%rbx,   0(%[b])\n\t"
		"movq %%rbp,   8(%[b])\n\t"
		"movq %%r12,  16(%[b])\n\t"
		"movq %%r13,  24(%[b])\n\t"
		"movq %%r14,  32(%[b])\n\t"
		"movq %%r15,  40(%[b])\n\t"
		"leaq 8(%%rsp), %%rax\n\t"
		"movq %%rax,  48(%[b])\n\t"
		"movq (%%rsp), %%rax\n\t"
		"movq %%rax,  56(%[b])\n\t"
		"movq %%rdi,  64(%[b])\n\t"
		"movq %%rsi,  72(%[b])\n\t"
		"xorl %[ret], %[ret]\n\t"
		: [ret] "=a" (ret)
		: [b] "r" (buf)
		: "memory"
	);
	return ret;
}

__attribute__((noreturn))
static void lkl_longjmp(void *buf, int val)
{
	__asm__ __volatile__ (
		"movq  0(%[b]), %%rbx\n\t"
		"movq  8(%[b]), %%rbp\n\t"
		"movq 16(%[b]), %%r12\n\t"
		"movq 24(%[b]), %%r13\n\t"
		"movq 32(%[b]), %%r14\n\t"
		"movq 40(%[b]), %%r15\n\t"
		"movq 48(%[b]), %%rsp\n\t"
		"movq 64(%[b]), %%rdi\n\t"
		"movq 72(%[b]), %%rsi\n\t"
		"movl %[v], %%eax\n\t"
		"testl %%eax, %%eax\n\t"
		"jnz 1f\n\t"
		"incl %%eax\n\t"
		"1:\n\t"
		"jmpq *56(%[b])\n\t"
		:
		: [b] "r" (buf), [v] "r" (val)
		: "memory"
	);
	__builtin_unreachable();
}

void jmp_buf_set(struct lkl_jmp_buf *jmpb, void (*f)(void))
{
	if (!lkl_setjmp(jmpb->buf))
		f();
}

void jmp_buf_longjmp(struct lkl_jmp_buf *jmpb, int val)
{
	lkl_longjmp(jmpb->buf, val);
}

#else
#include <setjmp.h>

void jmp_buf_set(struct lkl_jmp_buf *jmpb, void (*f)(void))
{
	if (!setjmp(*((jmp_buf *)jmpb->buf)))
		f();
}

void jmp_buf_longjmp(struct lkl_jmp_buf *jmpb, int val)
{
	longjmp(*((jmp_buf *)jmpb->buf), val);
}
#endif
