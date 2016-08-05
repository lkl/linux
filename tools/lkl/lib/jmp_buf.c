#include <setjmp.h>
#include <lkl_host.h>

void jmp_buf_set(struct lkl_jmp_buf *jmpb, void (*f)(struct lkl_jmp_buf *))
{
	if (!setjmp(*((jmp_buf *)jmpb->buf)))
		f(jmpb);
}

void jmp_buf_longjmp(struct lkl_jmp_buf *jmpb)
{
	longjmp(*((jmp_buf *)jmpb->buf), 1);
}
