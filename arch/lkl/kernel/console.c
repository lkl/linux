#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/syscalls.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/major.h>

static void console_write(struct console *con, const char *str, unsigned len)
{
	static char buf[256];
	static int verbose;

	/* when console isn't NULL (not called from file_write() */
	if (con && !verbose) {
		if (!lkl_ops->getparam)
			verbose = 1;
		else if (lkl_ops->getparam("RUMP_VERBOSE", buf, sizeof(buf))
			 == 0)
			if (*buf != 0)
				verbose = 1;
	}

	if (con && !verbose)
		return;

	if (lkl_ops->print)
		lkl_ops->print(str, len);
}

#ifdef CONFIG_LKL_EARLY_CONSOLE
static struct console lkl_boot_console = {
	.name	= "lkl_boot_console",
	.write	= console_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT,
	.index	= -1,
};

int __init lkl_boot_console_init(void)
{
	register_console(&lkl_boot_console);
	return 0;
}
early_initcall(lkl_boot_console_init);
#endif

static struct console lkl_console = {
	.name	= "lkl_console",
	.write	= console_write,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
};

int __init lkl_console_init(void)
{
	register_console(&lkl_console);
	return 0;
}
core_initcall(lkl_console_init);

static ssize_t file_write(struct file *fp, const char __user *s,
			  size_t n, loff_t *off)
{
	console_write(NULL, s, n);
	return n;
}

static ssize_t file_read(struct file *file, char __user *buf, size_t size,
			 loff_t *ppos)
{
	int err = 0;
#ifdef TO_BE_IMPLEMENTED
	/* need to use iovread in host_ops (not directly from rump hypercall) */
	struct rumpuser_iovec iov;
	ssize_t ret;

	iov.iov_base = buf;
	iov.iov_len = size;

	err = rumpuser_iovread(0, &iov, 1, 0, &ret);
	if (err == 0)
		return ret;

#endif
	return -err;
}

static const struct file_operations lkl_stdio_fops = {
	.owner		= THIS_MODULE,
	.write =	file_write,
	.read =		file_read,
};

static int __init lkl_stdio_init(void)
{
	int err;

	/* prepare /dev/console */
	err = register_chrdev(TTYAUX_MAJOR, "console", &lkl_stdio_fops);
	if (err < 0) {
		pr_err("can't register lkl stdio console.\n");
		return err;
	}

	return 0;
}
/* should be _before_ default_rootfs creation (noinitramfs.c) */
fs_initcall(lkl_stdio_init);

static int __init lkl_memdev_init(void)
{
	int err;

	/* prepare /dev/null */
	err = sys_mknod((const char __user __force *) "/dev/null",
			S_IFCHR | 0600, new_encode_dev(MKDEV(MEM_MAJOR, 3)));
	if (err < 0) {
		pr_err("can't register /dev/null.\n");
		return err;
	}

	return 0;
}
device_initcall(lkl_memdev_init);
