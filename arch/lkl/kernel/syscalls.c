#include <linux/stat.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/task_work.h>
#include <linux/syscalls.h>
#include <linux/kthread.h>
#include <asm/host_ops.h>
#include <asm/syscalls.h>
#include <asm/syscalls_32.h>

static asmlinkage long sys_create_syscall_thread(struct task_struct **task);

typedef long (*syscall_handler_t)(long arg1, ...);

#undef __SYSCALL
#define __SYSCALL(nr, sym) [nr] = (syscall_handler_t)sym,

syscall_handler_t syscall_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] =  (syscall_handler_t)sys_ni_syscall,
#include <asm/unistd.h>

#if __BITS_PER_LONG == 32
#include <asm/unistd_32.h>
#endif
};

static LIST_HEAD(syscall_threads);
static struct task_struct *default_syscall_thread;

int syscall_thread(void *arg)
{
	static int count;

	snprintf(current->comm, sizeof(current->comm), "ksyscalld%d", count++);
	pr_info("lkl: syscall thread %s initialized\n", current->comm);
	lkl_host_thread_exit();
	return 0;
}

static unsigned int syscall_thread_key;

static long __lkl_syscall(struct task_struct *task, long no, long *params)
{
	long ret;
	struct lkl_jmp_buf jmpb;

	if (no < 0 || no >= __NR_syscalls)
		return -ENOSYS;

	lkl_host_thread_enter(task);
	ret = syscall_table[no](params[0], params[1], params[2], params[3],
				params[4], params[5]);
	task_work_run();
	lkl_ops->jmp_buf_set(&jmpb, lkl_host_thread_leave);

	return ret;
}

static void lkl_task_exit(struct lkl_jmp_buf *jmpb)
{
	struct task_struct *task = jmpb->arg;

	lkl_host_thread_enter(task);
	pr_info("lkl: stopping syscall thread %s\n", current->comm);
	task_thread_info(task)->exit_jmpb = jmpb;
	do_exit(0);
}

static void __lkl_stop_syscall_thread(struct task_struct *task)
{
	struct lkl_jmp_buf jmpb = {
		.arg = task
	};

	lkl_ops->jmp_buf_set(&jmpb, lkl_task_exit);
}

static struct task_struct *__lkl_create_syscall_thread(void)
{
	struct task_struct *task = NULL;
	long params[6] = { (long)&task, };
	long ret;

	if (!lkl_ops->tls_set)
		return ERR_PTR(-ENOTSUPP);

	ret = __lkl_syscall(default_syscall_thread,
			    __NR_create_syscall_thread, params);
	if (ret < 0)
		return ERR_PTR(ret);

	ret = lkl_ops->tls_set(syscall_thread_key, task);
	if (ret < 0) {
		__lkl_stop_syscall_thread(task);
		return ERR_PTR(-ENOTSUPP);
	}

	return task;
}

int lkl_create_syscall_thread(void)
{
	struct task_struct *task = __lkl_create_syscall_thread();

	if (IS_ERR(task))
		return PTR_ERR(task);
	return 0;
}


int lkl_stop_syscall_thread(void)
{
	struct task_struct *task = NULL;

	if (lkl_ops->tls_get)
		task = lkl_ops->tls_get(syscall_thread_key);
	if (!task)
		return -EINVAL;
	if (lkl_ops->tls_get)
		lkl_ops->tls_set(syscall_thread_key, NULL);

	__lkl_stop_syscall_thread(task);
	return 0;
}

static int auto_syscall_threads = true;
static int __init setup_auto_syscall_threads(char *str)
{
	get_option (&str, &auto_syscall_threads);

	return 1;
}
__setup("lkl_auto_syscall_threads=", setup_auto_syscall_threads);

long lkl_syscall(long no, long *params)
{
	struct task_struct *task = NULL;

	if (auto_syscall_threads && lkl_ops->tls_get) {
		task = lkl_ops->tls_get(syscall_thread_key);
		if (!task) {
			task = __lkl_create_syscall_thread();
			if (IS_ERR(task)) {
				task = NULL;
				lkl_puts("failed to create syscall thread\n");
			}
		}
	}
	if (!task || no == __NR_reboot)
		task = default_syscall_thread;

	return __lkl_syscall(task, no, params);
}

static asmlinkage long
sys_create_syscall_thread(struct task_struct **task)
{
	pid_t pid;

	pid = kernel_thread(syscall_thread, NULL, CLONE_VM | CLONE_FS |
			    CLONE_FILES | CLONE_THREAD | CLONE_SIGHAND |
			    SIGCHLD);
	if (pid < 0)
		return pid;

	rcu_read_lock();
	*task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	return 0;
}

int initial_syscall_thread(struct lkl_sem *sem)
{
	int ret = 0;

	default_syscall_thread = current;

	if (lkl_ops->tls_alloc) {
		ret = lkl_ops->tls_alloc(&syscall_thread_key);
		if (!ret)
			ret = lkl_ops->tls_set(syscall_thread_key, current);
	}
	if (ret)
		return ret;

	init_pid_ns.child_reaper = 0;

	lkl_ops->sem_up(sem);

	syscall_thread(NULL);

	return ret;
}

void free_initial_syscall_thread(void)
{
	if (lkl_ops->tls_free)
		lkl_ops->tls_free(syscall_thread_key);
}
