#ifndef _ASM_LKL_PROCESSOR_H
#define _ASM_LKL_PROCESSOR_H

struct task_struct;

static inline void cpu_relax(void)
{
	unsigned long flags;

	/* since this is usually called in a tight loop waiting for some
	 * external condition (e.g. jiffies) lets run interrupts now to allow
	 * the external condition to propagate */
	local_irq_save(flags);
	local_irq_restore(flags);
}

#define current_text_addr() ({ __label__ _l; _l: &&_l; })

static inline unsigned long thread_saved_pc(struct task_struct *tsk)
{
	return 0;
}

static inline void prepare_to_copy(struct task_struct *tsk)
{
}

static inline unsigned long __get_wchan(struct task_struct *p)
{
	return 0;
}

static inline void flush_thread(void)
{
}

struct thread_struct { };

#define INIT_THREAD { }

#define task_pt_regs(tsk) (struct pt_regs *)(NULL)

/* We don't have strict user/kernel spaces */
#define TASK_SIZE ((unsigned long)-1)
#ifndef CONFIG_MMU
#define TASK_UNMAPPED_BASE	0
#else
#define TASK_UNMAPPED_BASE	CONFIG_LKL_TASK_UNMAPPED_BASE
#define STACK_TOP			(CONFIG_LKL_MEMORY_START - 16 * PAGE_SIZE)
#define STACK_TOP_MAX		STACK_TOP

static inline void start_thread(struct pt_regs *regs, unsigned long entry,
			 unsigned long stack) {}
#endif // CONFIG_MMU

#define KSTK_EIP(tsk)	(0)
#define KSTK_ESP(tsk)	(0)

#endif
