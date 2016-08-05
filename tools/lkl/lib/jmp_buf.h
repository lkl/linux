#ifndef _LKL_LIB_JMP_BUF_H
#define _LKL_LIB_JMP_BUF_H

void jmp_buf_set(struct lkl_jmp_buf *jmpb, void (*f)(struct lkl_jmp_buf *));
void jmp_buf_longjmp(struct lkl_jmp_buf *jmpb);

#endif
