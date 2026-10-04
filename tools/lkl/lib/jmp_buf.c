// SPDX-License-Identifier: GPL-2.0
#ifdef _WIN64
/*
 * The Win64 longjmp() unwinds the stack with RtlUnwindEx(), which fails when
 * LKL jumps between the stacks of different threads. Without a frame, i.e.
 * _setjmp(buf, NULL), longjmp() only restores the registers.
 */
#define __USE_MINGW_SETJMP_NON_SEH
#endif
#include <setjmp.h>
#include <stdint.h>
#include <lkl_host.h>

static jmp_buf *get_jmp_buf(struct lkl_jmp_buf *jmpb)
{
#ifdef _WIN64
	/* the Win64 jmp_buf holds xmm registers and must be 16 bytes aligned */
	_Static_assert(sizeof(jmp_buf) + 15 <= sizeof(jmpb->buf),
		       "lkl_jmp_buf is too small");
	return (jmp_buf *)(((uintptr_t)jmpb->buf + 15) & ~(uintptr_t)15);
#else
	return (jmp_buf *)jmpb->buf;
#endif
}

void jmp_buf_set(struct lkl_jmp_buf *jmpb, void (*f)(void))
{
	if (!setjmp(*get_jmp_buf(jmpb)))
		f();
}

void jmp_buf_longjmp(struct lkl_jmp_buf *jmpb, int val)
{
	longjmp(*get_jmp_buf(jmpb), val);
}
