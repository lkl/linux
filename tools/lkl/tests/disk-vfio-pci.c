// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <lkl.h>
#include <lkl_host.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <inttypes.h>

#include "test.h"
#include "cla.h"

static struct {
	int printk;
	const char *fstype;
	const char *pciname;
} cla;

struct cl_arg args[] = {
	{ "pciname", 'n', "PCI device name (as %x:%x:%x.%x format)", 1,
	  CL_ARG_STR, &cla.pciname },
	{ 0 },
};

static char bootparams[128];

#define min(a, b) (a < b ? a : b)

static int is_nvme_disk(const char *name)
{
	return !strncmp(name, "nvme", 4) && !strchr(name, 'p');
}

static int find_nvme_blkdev(unsigned int *dev, int log_missing)
{
	char buf[4096];
	char *line, *saveptr = NULL;
	int fd, err;

	fd = lkl_sys_open("/proc/partitions", LKL_O_RDONLY, 0);
	if (fd < 0)
		return fd;

	err = lkl_sys_read(fd, buf, sizeof(buf) - 1);
	lkl_sys_close(fd);
	if (err < 0)
		return err;

	buf[err] = '\0';
	line = strtok_r(buf, "\n", &saveptr);
	while (line) {
		unsigned int major, minor;
		unsigned long long blocks;
		char name[32];

		if (sscanf(line, " %u %u %llu %31s", &major, &minor,
			   &blocks, name) == 4 && is_nvme_disk(name)) {
			*dev = LKL_MKDEV(major, minor);
			lkl_test_logf("using /dev/%s (%u:%u)\n",
				      name, major, minor);
			return 0;
		}

		line = strtok_r(NULL, "\n", &saveptr);
	}

	if (log_missing)
		lkl_test_logf("no NVMe disk found in /proc/partitions:\n%s",
			      buf);
	return -LKL_ENODEV;
}

static int wait_for_nvme_blkdev(unsigned int *dev)
{
	int i, err;

	err = lkl_mount_fs("proc");
	if (err < 0) {
		lkl_test_logf("mount proc failed: %s\n", lkl_strerror(err));
		return err;
	}

	for (i = 0; i < 100; i++) {
		err = find_nvme_blkdev(dev, i == 99);
		if (!err)
			return 0;
		usleep(100000);
	}

	return err;
}

static int lkl_test_blkdev(void)
{
	char dev_str[] = { "/dev/xxxxxxxx" };
	char buffer[64*1024];
	uint64_t size, read = 0;
	unsigned int dev;
	int err;
	int fd;

	err = wait_for_nvme_blkdev(&dev);
	if (err < 0)
		return TEST_FAILURE;

	snprintf(dev_str, sizeof(dev_str), "/dev/%08x", dev);

	err = lkl_sys_mknod(dev_str, LKL_S_IFBLK | 0600, dev);
	if (err < 0) {
		lkl_test_logf("mknod failed: %s\n", lkl_strerror(err));
		return TEST_FAILURE;
	}

	fd = lkl_sys_open(dev_str, LKL_O_RDONLY, 0);
	if (fd < 0) {
		lkl_test_logf("open failed: %s\n", lkl_strerror(fd));
		return TEST_FAILURE;
	}

	err = lkl_sys_ioctl(fd, LKL_BLKGETSIZE64, (lkl_ulong_t)&size);
	if (err < 0) {
		lkl_test_logf("BLKGETSIZE64 failed: %s\n", lkl_strerror(fd));
		lkl_sys_close(fd);
		return TEST_FAILURE;
	}

	while (read < size) {
		err = lkl_sys_read(fd, buffer,
				   min(sizeof(buffer), size - read));
		if (err <= 0) {
			lkl_test_logf("read failed: %s\n", lkl_strerror(err));
			lkl_sys_close(fd);
			return TEST_FAILURE;
		}
		read += err;
	}

	lkl_sys_close(fd);
	lkl_test_logf("read %" PRIu64 " bytes\n", read);

	return TEST_SUCCESS;
}

LKL_TEST_CALL(start_kernel, lkl_start_kernel, 0, bootparams);
LKL_TEST_CALL(stop_kernel, lkl_sys_halt, 0);

struct lkl_test tests[] = {
	LKL_TEST(start_kernel),
	LKL_TEST(blkdev),
	LKL_TEST(stop_kernel),
};

int main(int argc, const char **argv)
{
	int ret;

	if (parse_args(argc, argv, args) < 0)
		return -1;

#ifdef RLIMIT_MEMLOCK
	/*
	 * VFIO pins the whole LKL memory region with VFIO_IOMMU_MAP_DMA,
	 * which is accounted against RLIMIT_MEMLOCK. Raise it if the
	 * hard limit allows, otherwise the DMA map fails and no PCI
	 * device is created.
	 */
	struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };

	setrlimit(RLIMIT_MEMLOCK, &rlim);
#endif

	snprintf(bootparams, sizeof(bootparams),
		 "mem=128M loglevel=8 lkl_pci=vfio%s", cla.pciname);

	lkl_host_ops.print = lkl_test_log;

	if (lkl_init(&lkl_host_ops) < 0) {
		printf("%s\n", lkl_test_get_log());
		return 1;
	}

	ret = lkl_test_run(tests, sizeof(tests) / sizeof(struct lkl_test),
			"disk-vfio-pci");

	lkl_cleanup();

	return ret;
}
