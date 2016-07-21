#include <linux/init.h>
#include <linux/init_task.h>
#include <linux/reboot.h>
#include <linux/fs.h>
#include <linux/start_kernel.h>
#include <linux/syscalls.h>
#include <asm/host_ops.h>
#include <asm/irq.h>
#include <asm/unistd.h>
#include <asm/syscalls.h>
#include <asm/cpu.h>

struct lkl_host_operations *lkl_ops;
static char cmd_line[COMMAND_LINE_SIZE];
static void *init_sem;
void (*pm_power_off)(void) = NULL;
static unsigned long mem_size;

long lkl_panic_blink(int state)
{
	lkl_ops->panic();
	return 0;
}

void __init setup_arch(char **cl)
{
	*cl = cmd_line;
	panic_blink = lkl_panic_blink;
	bootmem_init(mem_size);
}

int run_init_process(const char *init_filename)
{
	initial_syscall_thread(init_sem);

	kernel_halt();

	return 0;
}

static void __init lkl_run_kernel(void *arg)
{
	/* Nobody will ever join us */
	lkl_ops->thread_detach();

	lkl_cpu_get();

	start_kernel();
}

int __init lkl_start_kernel(struct lkl_host_operations *ops,
			unsigned long _mem_size,
			const char *fmt, ...)
{
	va_list ap;
	int ret;

	lkl_ops = ops;
	mem_size = _mem_size;

	va_start(ap, fmt);
	ret = vsnprintf(boot_command_line, COMMAND_LINE_SIZE, fmt, ap);
	va_end(ap);

	if (ops->virtio_devices)
		strncpy(boot_command_line + ret, ops->virtio_devices,
			COMMAND_LINE_SIZE - ret);

	memcpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);

	ret = threads_init();
	if (ret)
		return ret;

	init_sem = lkl_ops->sem_alloc(0);
	if (!init_sem)
		return -ENOMEM;

	ret = lkl_cpu_init();
	if (ret)
		goto out_free_init_sem;

	ret = lkl_ops->thread_create(lkl_run_kernel, NULL);
	if (!ret) {
		ret = -ENOMEM;
		goto out_free_init_sem;
	}

	lkl_ops->sem_down(init_sem);

	return 0;

out_free_init_sem:
	lkl_ops->sem_free(init_sem);

	return ret;
}

void machine_halt(void)
{
	lkl_cpu_shutdown();
}

void machine_power_off(void)
{
	machine_halt();
}

void machine_restart(char *unused)
{
	machine_halt();
}

extern int lkl_netdevs_remove(void);

long lkl_sys_halt(void)
{
	long err;
	long params[6] = { 0, };

	err = lkl_syscall(__NR_reboot, params);
	if (err < 0)
		return err;

	lkl_cpu_wait_shutdown();

	free_initial_syscall_thread();
	if (lkl_netdevs_remove() == 0)
		/* We know that there is nothing else touching our
		 * memory. */
		free_mem();

	return 0;
}


/* skip mounting the "real" rootfs. ramfs is good enough. */
static int __init fs_setup(void)
{
	int fd;

	fd = sys_open("/init", O_CREAT, 0600);
	WARN_ON(fd < 0);
	sys_close(fd);

	return 0;
}
late_initcall(fs_setup);
