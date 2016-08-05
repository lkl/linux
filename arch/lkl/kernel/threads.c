#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <asm/host_ops.h>
#include <asm/cpu.h>
#include <linux/tick.h>


static volatile int threads_counter;

struct thread_info *alloc_thread_info_node(struct task_struct *task, int node)
{
	struct thread_info *ti;

	/*
	 * We can't use host memory to make the cleanup simpler
	 * because virt_to_page() is used on thread_info memory in
	 * account_kernel_stack().
	 */
	ti = kmalloc(sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return NULL;

	ti->dead = NULL;
	ti->exit_jmpb = NULL;
	ti->task = task;
	ti->sched_sem = lkl_ops->sem_alloc(0);
	if (!ti->sched_sem) {
		kfree(ti);
		return NULL;
	}

	return ti;
}

/*
 * The only new tasks created are kernel threads that have a predefined starting
 * point thus no stack copy is required.
 */
void setup_thread_stack(struct task_struct *p, struct task_struct *org)
{
	struct thread_info *ti = task_thread_info(p);
	struct thread_info *org_ti = task_thread_info(org);

	ti->flags = org_ti->flags;
	ti->preempt_count = org_ti->preempt_count;
	ti->addr_limit = org_ti->addr_limit;
}

static void kill_thread(struct thread_info *ti)
{
	if (WARN_ON(!ti->dead))
		return;

	*ti->dead = true;
	lkl_ops->sem_up(ti->sched_sem);
}

void free_thread_info(struct thread_info *ti)
{
	kill_thread(ti);
	/*
	 * Here we only free the memory. The scheduling semaphore is
	 * still needed and we free it from the host thread.
	 */
	kfree(ti);
}


struct thread_info *_current_thread_info = &init_thread_union.thread_info;

/*
 * schedule() expects the return of this function to be the task that we
 * switched away from. Returning prev is not going to work because we
 * are actually going to return the previous taks that was scheduled
 * before the task we are going to wake up, and not the current task,
 * e.g.:
 *
 * swapper -> init: saved prev on swapper stack is swapper
 * init -> ksoftirqd0: saved prev on init stack is init
 * ksoftirqd0 -> swapper: returned prev is swapper
 */
static struct task_struct *abs_prev = &init_task;

/*
 * Default schedule actions: wake up the next thread by incrementing
 * the schedule semaphore and blocking the prev thread by waiting on
 * the scheduling semaphore.
 *
 * Special case: when we switch to the idle thread, check if the idle
 * thread has been preempted by a host thread. In this case we need to
 * release the CPU lock to avoid deadlocking the idle thread. We also
 * want to force a CPU wakeup in case the direct host calls have waked
 * up other threads.
 *
 * NOTE: the reschedule is needed to kick the idle nohz
 * machine. However, I am not sure if this is sufficient, as the tail
 * of the cpu_idle_loop will be delayed while the direct host calls
 * run.
 */
static void sched_next_action(struct thread_info *prev,
			      struct thread_info *next)
{
	if (next == task_thread_info(&init_task) && lkl_cpu_preempted_idle()) {
		schedule_tail(abs_prev);
		set_thread_flag(TIF_NEED_RESCHED);
		lkl_cpu_put();
		lkl_cpu_wakeup();
	} else {
		lkl_ops->sem_up(next->sched_sem);
	}
}

static void sched_prev_action(struct thread_info *prev,
			      struct thread_info *next)
{
	lkl_ops->sem_down(prev->sched_sem);


	/*
	 * Note that we can't free the thread_info memory here to
	 * avoid the dead stack pointer and prev copy in __switch_to
	 * because at this point there may be other Linux threads
	 * running which would race with kfree.
	 */
	if (*prev->dead) {
		lkl_ops->sem_free(prev->sched_sem);
		__sync_fetch_and_sub(&threads_counter, 1);
		if (prev->exit_jmpb)
			lkl_ops->jmp_buf_longjmp(prev->exit_jmpb);
		else
			lkl_ops->thread_exit();
	}
}


typedef void (*lkl_sched_action)(struct thread_info *prev,
				 struct thread_info *next);
static struct {
	lkl_sched_action prev_action;
	lkl_sched_action next_action;
	struct task_struct *task;
	struct lkl_jmp_buf *jmpb;
} sched = {
	.prev_action = sched_prev_action,
	.next_action = sched_next_action,
};

/*
 * Host thread enter schedule actions: we want to switch from the idle
 * thread to our designated system call thread.
 *
 * If the scheduler picked the right task there is nothing to do - the
 * prev (idle) thread can't run because we have the CPU and the next
 * thread (us) is already running.
 *
 * Otherwise, we wait until the scheduler picks us. In this case we
 * have to resume next and wait on the task.sched semaphore.
 */
static void sched_next_action_host_enter(struct thread_info *prev,
					 struct thread_info *next)
{
	if (next->task != sched.task)
		lkl_ops->sem_up(next->sched_sem);
}

static void sched_prev_action_host_enter(struct thread_info *prev,
					 struct thread_info *next)
{
	if (next->task != sched.task)
		lkl_ops->sem_down(task_thread_info(sched.task)->sched_sem);
}

/*
 * Thread exit schedule actions: we want to kill the host thread
 * associated with a Linux thread because we won't use it, we will
 * directly use the caller's host thread to run Linux system calls.
 *
 * The next action remains unchanged. The prev action just calls the
 * host thread exit host operation.
 */
static void sched_prev_action_thread_exit(struct thread_info *prev,
					  struct thread_info *next)
{
	__sync_fetch_and_sub(&threads_counter, 1);
	lkl_ops->thread_exit();
}


/*
 * Host thread leave schedule actions: switch from the current (host)
 * thread to one of the Linux threads.
 *
 * The next action remains unchanged, we need to wake up the next
 * thread - but see the comments in the default next action regarding
 * the case where the Linux idle thread was preempted by a host
 * thread.
 *
 * As for the prev action we want to cut short the schedule() path to
 * avoid running finish_task_switch(). We use our previous saved jump
 * buffer to continue executing the caller's (host) context.
 */
static void sched_prev_action_thread_longjmp(struct thread_info *prev,
					     struct thread_info *next)
{
	lkl_ops->jmp_buf_longjmp(sched.jmpb);
}

/*
 * Prepare for directly running Linux code from a host thread in the
 * context of a given Linux thread.
 *
 * In order to avoid race conditions we can run only when we know that
 * no other Linux threads are running. We do this by "stealing" the
 * CPU from the idle thread.
 *
 * Before running Linux code we need to switch to the given task by
 * waking it up and running the scheduler. If the scheduler picks our
 * tasks we are good to run, otherwise we wait our turn.
 */
void lkl_host_thread_enter(struct task_struct *task)
{
	lkl_cpu_get();

	wake_up_process(task);

	sched.prev_action = sched_prev_action_host_enter;
	sched.next_action = sched_next_action_host_enter;
	sched.task = task;
	if (yield_to(task, true) <= 0)
		schedule();
}

/*
 * We finished executing Linux code and we want to return executing
 * host code. Before doing so we must block the current Linux task and
 * switch to a different task. Note that we can't block the current
 * host thread (like we usualy do in a regular context switch) because
 * the current thread is not a Linux dedicated host. We also can't
 * continue running the schedule tail (e.g. finish_task_switch) so we
 * have to jump back to a previously saved location in our call chain.
 *
 */
void lkl_host_thread_leave(struct lkl_jmp_buf *jmpb)
{
	sched.prev_action = sched_prev_action_thread_longjmp;
	sched.jmpb = jmpb;
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();
}

/*
 * We don't need a dedicated host thread for the current Linux task,
 * so arrange to block the Linux thread and exist the host thread. We
 * can later use this Linux task by calling lkl_host_thread_enter.
 */
void lkl_host_thread_exit(void)
{
	sched.prev_action = sched_prev_action_thread_exit;
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();
}

struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next)
{
	struct thread_info *_prev = task_thread_info(prev);
	struct thread_info *_next = task_thread_info(next);
	struct thread_info _prev_copy;
	bool dead = false;
	lkl_sched_action prev_action = sched.prev_action;
	lkl_sched_action next_action = sched.next_action;

	_current_thread_info = _next;
	abs_prev = prev;
	sched.prev_action = sched_prev_action;
	sched.next_action = sched_next_action;

	/*
	 * We need a way to signal that to a host thread that is dead
	 * without using thread_info's memory which we need to free in
	 * free_thread_info - see the comments there. So use a pointer
	 * to a stack variable wich is fine because a dying task's
	 * host thread will block until free_thread_info is called.
	 */
	_prev->dead = &dead;
	/*
	 * _prev can be freed in free_thread_info when the next thread
	 * starts running. So make a copy here to avoid invalid
	 * accesses in prev_action.
	 */
	_prev_copy = *_prev;

	next_action(_prev, _next);
	prev_action(&_prev_copy, _next);

	/*
	 * If we made it past this point we know _prev has not been
	 * freed and is safe to access it.
	 */
	_prev->dead = NULL;


	return abs_prev;
}

struct thread_bootstrap_arg {
	struct thread_info *ti;
	int (*f)(void *);
	void *arg;
};

static void thread_bootstrap(void *_tba)
{
	struct thread_bootstrap_arg *tba = (struct thread_bootstrap_arg *)_tba;
	struct thread_info *ti = tba->ti;
	int (*f)(void *) = tba->f;
	void *arg = tba->arg;

	/* Our lifecycle is managed by the LKL kernel, so we want to
	 * detach here in order to free up host resources when we're
	 * killed */
	lkl_ops->thread_detach();

	lkl_ops->sem_down(ti->sched_sem);
	schedule_tail(abs_prev);
	kfree(tba);

	f(arg);
	do_exit(0);
}

int copy_thread(unsigned long clone_flags, unsigned long esp,
		unsigned long unused, struct task_struct *p)
{
	struct thread_info *ti = task_thread_info(p);
	struct thread_bootstrap_arg *tba;
	int ret;

	tba = kmalloc(sizeof(*tba), GFP_KERNEL);
	if (!tba)
		return -ENOMEM;

	tba->f = (int (*)(void *))esp;
	tba->arg = (void *)unused;
	tba->ti = ti;

	ret = lkl_ops->thread_create(thread_bootstrap, tba);
	if (!ret) {
		kfree(tba);
		return -ENOMEM;
	}

	__sync_fetch_and_add(&threads_counter, 1);

	return 0;
}

void show_stack(struct task_struct *task, unsigned long *esp)
{
}

static inline void pr_early(const char *str)
{
	if (lkl_ops->print)
		lkl_ops->print(str, strlen(str));
}

/**
 * This is called before the kernel initializes, so no kernel calls (including
 * printk) can't be made yet.
 */
int threads_init(void)
{
	struct thread_info *ti = &init_thread_union.thread_info;
	int ret = 0;

	ti->dead = NULL;

	ti->sched_sem = lkl_ops->sem_alloc(0);
	if (!ti->sched_sem) {
		pr_early("lkl: failed to allocate init schedule semaphore\n");
		ret = -ENOMEM;
	}

	return ret;
}

void threads_cleanup(void)
{
	struct task_struct *p;

	for_each_process(p) {
		struct thread_info *ti = task_thread_info(p);

		if (p->pid != 1)
			WARN(!(p->flags & PF_KTHREAD),
			     "non kernel thread task %p\n", p->comm);
		WARN(p->state == TASK_RUNNING,
		     "thread %s still running while halting\n", p->comm);

		kill_thread(ti);
	}

	while (threads_counter)
		;

	lkl_ops->sem_free(init_thread_union.thread_info.sched_sem);
}
