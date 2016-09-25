#ifndef _ASM_LKL_CPU_H
#define _ASM_LKL_CPU_H

int lkl_cpu_get(void);
void lkl_cpu_put(void);
int lkl_cpu_try_get(void);
int lkl_cpu_init(void);
void lkl_cpu_shutdown(void);
bool lkl_cpu_is_shutdown(void);
void lkl_cpu_wait_shutdown(void);
void lkl_cpu_wakeup(void);

#endif /* _ASM_LKL_CPU_H */
