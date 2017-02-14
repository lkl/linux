/*
 * Rump hypercall interface for LKL
 * Copyright (c) 2015 Hajime Tazaki
 *
 * Author: Hajime Tazaki <thehajime@gmail.com>
 */

#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>

#include <unistd.h>
#include <poll.h>
#include <sys/uio.h>

#include <lkl_host.h>
#include "iomem.h"
#include "jmp_buf.h"
#include "rump.h"


/* FIXME */
#define BIT(x) (1ULL << x)
#define NSEC_PER_SEC	1000000000L
#define container_of(ptr, type, member) \
	(type *)((char *)(ptr) - __builtin_offsetof(type, member))

/* FIXME */
int *__errno(void);
#undef errno
#define errno (*__errno())


/* console */
static void rump_print(const char *str, int len)
{
	while (len-- > 0) {
		rumpuser_putchar(*str);
		str++;
	}
}


/* semaphore/mutex */
struct rumpuser_sem {
	struct rumpuser_mtx *lock;
	int count;
	struct rumpuser_cv *cond;
};

struct lkl_mutex {
	struct rumpuser_mtx *mutex;
};

struct lkl_sem {
	struct rumpuser_sem sem;
};

static struct lkl_sem *rump_sem_alloc(int count)
{
	struct lkl_sem *sem;

	rumpuser_malloc(sizeof(*sem), 0, (void **)&sem);
	if (!sem)
		return NULL;

	rumpuser_mutex_init(&sem->sem.lock, RUMPUSER_MTX_SPIN);
	sem->sem.count = count;
	rumpuser_cv_init(&sem->sem.cond);

	return sem;
}

static void rump_sem_free(struct lkl_sem *_sem)
{
	struct rumpuser_sem *sem = (struct rumpuser_sem *)&_sem->sem;

	rumpuser_cv_destroy(sem->cond);
	rumpuser_mutex_destroy(sem->lock);
	rumpuser_free(sem, 0);
}

static void rump_sem_up(struct lkl_sem *_sem)
{
	struct rumpuser_sem *sem = (struct rumpuser_sem *)&_sem->sem;

	rumpuser_mutex_enter(sem->lock);
	sem->count++;
	if (sem->count > 0)
		rumpuser_cv_signal(sem->cond);
	rumpuser_mutex_exit(sem->lock);
}

static void rump_sem_down(struct lkl_sem *_sem)
{
	struct rumpuser_sem *sem = (struct rumpuser_sem *)&_sem->sem;

	rumpuser_mutex_enter(sem->lock);
	while (sem->count <= 0)
		rumpuser_cv_wait(sem->cond, sem->lock);
	sem->count--;
	rumpuser_mutex_exit(sem->lock);
}

static struct lkl_mutex *rump_mutex_alloc(int recursive)
{
	struct lkl_mutex *_mutex;

	rumpuser_malloc(sizeof(*_mutex), 0, (void **)&_mutex);
	if (!_mutex)
		return NULL;

	rumpuser_mutex_init(&_mutex->mutex, RUMPUSER_MTX_SPIN);

	return _mutex;
}

static void rump_mutex_lock(struct lkl_mutex *_mutex)
{
	rumpuser_mutex_enter(_mutex->mutex);
}

static void rump_mutex_unlock(struct lkl_mutex *_mutex)
{
	rumpuser_mutex_exit(_mutex->mutex);
}

static void rump_mutex_free(struct lkl_mutex *_mutex)
{
	rumpuser_mutex_destroy(_mutex->mutex);
	rumpuser_free(_mutex, 0);
}

/* XXX: dummy TLS */
struct lkl_tls_key {
	unsigned int *key;
};
static struct lkl_tls_key *rump_tls_alloc(void (*destructor)(void *))
{
	return NULL;
}

static void rump_tls_free(struct lkl_tls_key *key)
{
}

static int rump_tls_set(struct lkl_tls_key *key, void *data)
{
	rumpuser_curlwpop(RUMPUSER_LWP_SET, (struct lwp *)data);
	return 0;
}

static void *rump_tls_get(struct lkl_tls_key *key)
{
	return rumpuser_curlwp();
}


/* memory */
static void *rump_mem_alloc(size_t size)
{
	void *mem;

	rumpuser_malloc(size, 0, (void **)&mem);
	return mem;
}

static void rump_mem_free(void *mem)
{
	rumpuser_free(mem, 0);
}

/* thread */
static lkl_thread_t rump_thread_create(void (*fn)(void *), void *arg)
{
	void *thrid;
	int ret;

	ret = rumpuser_thread_create((void * (*)(void *))fn, arg,
				     "lkl_thr", 1, 1, -1, &thrid);
	if (ret)
		return 0;

	return (lkl_thread_t) thrid;
}

static void rump_thread_detach(void)
{
	/* NOP */
}

static void rump_thread_exit(void)
{
	rumpuser_thread_exit();
}

static int rump_thread_join(lkl_thread_t tid)
{
	return rumpuser_thread_join((void *)tid);
}

static lkl_thread_t rump_thread_self(void)
{
	return (lkl_thread_t)rumpuser_thread_self();
}

static int rump_thread_equal(lkl_thread_t a, lkl_thread_t b)
{
	return a == b;
}

/* time/timer */
static bool threads_are_go;
static struct rumpuser_mtx *thrmtx;
static struct rumpuser_cv *thrcv;

struct thrdesc {
	void (*f)(void *);
	void *arg;
	int canceled;
	void *thrid;
	struct timespec timeout;
	struct rumpuser_mtx *mtx;
	struct rumpuser_cv *cv;
};

static void *rump_timer_trampoline(void *arg)
{
	struct thrdesc *td = arg;
	void (*f)(void *);
	void *thrarg;
	int err;

	/* from src-netbsd/sys/rump/librump/rumpkern/thread.c */
	/* don't allow threads to run before all CPUs have fully attached */
	if (!threads_are_go) {
		rumpuser_mutex_enter_nowrap(thrmtx);
		while (!threads_are_go)
			rumpuser_cv_wait_nowrap(thrcv, thrmtx);
		rumpuser_mutex_exit(thrmtx);
	}

	f = td->f;
	thrarg = td->arg;
	if (td->timeout.tv_sec != 0 || td->timeout.tv_nsec != 0) {
		rumpuser_mutex_enter(td->mtx);
		err = rumpuser_cv_timedwait(td->cv, td->mtx,
					    td->timeout.tv_sec,
					    td->timeout.tv_nsec);
		if (td->canceled) {
			if (!td->thrid)
				rumpuser_free(td, 0);
			goto end;
		}
		rumpuser_mutex_exit(td->mtx);
		/* FIXME: we should not use rumpuser__errtrans here
		 * 60==ETIMEDOUT(netbsd), rumpuser__errtrans(ETIMEDOUT))
		 */
		if (err && err != 60)
			goto end;
	}

	f(thrarg);

	rumpuser_thread_exit();
end:
	return arg;
}

static void rump_timer_cancel(void *timer)
{
	struct thrdesc *td = timer;

	if (td->canceled)
		return;

	td->canceled = 1;
	rumpuser_mutex_enter(td->mtx);
	rumpuser_cv_signal(td->cv);
	rumpuser_mutex_exit(td->mtx);

	rumpuser_mutex_destroy(td->mtx);
	rumpuser_cv_destroy(td->cv);

	if (td->thrid)
		rumpuser_thread_join(td->thrid);

	rumpuser_free(td, 0);
}

/* from src-netbsd/sys/rump/librump/rumpkern/thread.c */
static void rump_thread_allow(struct lwp *l)
{
	rumpuser_mutex_enter(thrmtx);
	if (l == NULL)
		threads_are_go = true;

	rumpuser_cv_broadcast(thrcv);
	rumpuser_mutex_exit(thrmtx);
}

static unsigned long long time_ns(void)
{
	struct timespec ts;

	rumpuser_clock_gettime(RUMPUSER_CLOCK_RELWALL, (int64_t *)&ts.tv_sec,
			       &ts.tv_nsec);

	return ((unsigned long long) ts.tv_sec * NSEC_PER_SEC) + ts.tv_nsec;
}

static void *timer_alloc(void (*fn)(void *), void *arg)
{
	struct thrdesc *td;

	rumpuser_malloc(sizeof(*td), 0, (void **)&td);

	memset(td, 0, sizeof(*td));
	td->f = fn;
	td->arg = arg;

	rumpuser_mutex_init(&td->mtx, RUMPUSER_MTX_SPIN);
	rumpuser_cv_init(&td->cv);

	return td;
}

static int timer_set_oneshot(void *_timer, unsigned long ns)
{
	int ret;
	struct thrdesc *td = _timer;

	td->timeout = (struct timespec){ .tv_sec = ns / NSEC_PER_SEC,
					 .tv_nsec = ns % NSEC_PER_SEC};
	ret = rumpuser_thread_create(rump_timer_trampoline, td, "timer",
				     0, 0, -1, &td->thrid);

	return ret ? -1 : 0;
}

static void timer_free(void *_timer)
{
	rump_timer_cancel(_timer);
}

static void panic(void)
{
	rumpuser_exit(RUMPUSER_PANIC);
}

struct lkl_host_operations lkl_host_ops = {
	.panic = panic,
	.thread_create = rump_thread_create,
	.thread_detach = rump_thread_detach,
	.thread_exit = rump_thread_exit,
	.thread_join = rump_thread_join,
	.thread_self = rump_thread_self,
	.thread_equal = rump_thread_equal,
	.sem_alloc = rump_sem_alloc,
	.sem_free = rump_sem_free,
	.sem_up = rump_sem_up,
	.sem_down = rump_sem_down,
	.mutex_alloc = rump_mutex_alloc,
	.mutex_free = rump_mutex_free,
	.mutex_lock = rump_mutex_lock,
	.mutex_unlock = rump_mutex_unlock,
	.tls_alloc = rump_tls_alloc,
	.tls_free = rump_tls_free,
	.tls_set = rump_tls_set,
	.tls_get = rump_tls_get,
	.time = time_ns,
	.timer_alloc = timer_alloc,
	.timer_set_oneshot = timer_set_oneshot,
	.timer_free = timer_free,
	.print = rump_print,
	.mem_alloc = rump_mem_alloc,
	.mem_free = rump_mem_free,
	.ioremap = lkl_ioremap,
	.iomem_access = lkl_iomem_access,
	.jmp_buf_set = jmp_buf_set,
	.jmp_buf_longjmp = jmp_buf_longjmp,
	.irq_request = rump_pci_irq_request,
	.irq_release = rump_pci_irq_release,
	.getparam = (int (*)(const char *, void *, int))rumpuser_getparam,
#ifndef RUMPRUN
	.virtio_devices = lkl_virtio_devs,
#endif
};


/* entry/exit points */
char *boot_cmdline;
static char buf[256];
static int verbose;

int rump_init(void)
{
	if (rumpuser_init(RUMPUSER_VERSION, &hyp) != 0) {
		rumpuser_dprintf("rumpuser init failed\n");
		return -EINVAL;
	}

	rumpuser_mutex_init(&thrmtx, RUMPUSER_MTX_SPIN);
	rumpuser_cv_init(&thrcv);
	threads_are_go = false;

	if (rumpuser_getparam("LKL_BOOT_CMDLINE", buf, sizeof(buf)) == 0)
		boot_cmdline = buf;
	else
		boot_cmdline = "mem=100M";


	lkl_start_kernel(&lkl_host_ops, boot_cmdline);

	rump_thread_allow(NULL);
	/* FIXME: rumprun doesn't have sysproxy.
	 * maybe outsourced and linked -lsysproxy for hijack case ?
	 */
#ifdef ENABLE_SYSPROXY
	rump_sysproxy_init();
#endif
	if (rumpuser_getparam("RUMP_VERBOSE", buf, sizeof(buf)) == 0) {
		if (*buf != 0)
			verbose = 1;
	}

	if (verbose)
		rumpuser_dprintf("rumpuser started.\n");
	return 0;
}

void rump_exit(void)
{
	if (verbose)
		rumpuser_dprintf("rumpuser finishing.\n");

#ifdef ENABLE_SYSPROXY
	rump_sysproxy_fini();
#endif
	rumpuser_exit(0);
}

/* stub calls */
#define RUMP_TEMP_STUB
#ifdef RUMP_TEMP_STUB
enum rump_etfs_type {
	RUMP_ETFS_REG,
	RUMP_ETFS_BLK,
	RUMP_ETFS_CHR,
	RUMP_ETFS_DIR,
	RUMP_ETFS_DIR_SUBDIRS
};

void rump_boot_setsigmodel(int rump_sigmodel)
{
}

int rump_pub_etfs_register(const char *key, const char *hostpath,
			   enum rump_etfs_type ftype)
{
	return 0;
}

int rump_pub_etfs_register_withsize(const char *key, const char *hostpath,
				    enum rump_etfs_type ftype, uint64_t begin,
				    uint64_t size)
{
	return 0;
}

int rump___sysimpl_mount50(const char *str, const char *str2, int i,
			   void *p, size_t s)
{
	return 0;
}

int rump___sysimpl_dup2(int i, int j)
{
	return 0;
}

int rump___sysimpl_socket30(int i, int j, int k)
{
	return 0;
}

int rump___sysimpl_unmount(const char *str, int i)
{
	return 0;
}

void __assert13(const char *file, int line, const char *function,
		const char *failedexpr)
{
}

int rump___sysimpl_close(int fd)
{
	return -1;
}

int rump___sysimpl_ioctl(int fd, u_long com, void *data)
{
	return -1;
}

int rump___sysimpl_mkdir(const char *path, mode_t mode)
{
	return -1;
}

int rump___sysimpl_open(const char *name, int flags, ...)
{
	return -1;
}

#endif /* RUMP_TEMP_STUB */

#ifndef RUMPRUN
static int fd_get_capacity(struct lkl_disk disk, unsigned long long *res)
{
	off_t off;

	off = lseek(disk.fd, 0, SEEK_END);
	if (off < 0)
		return -1;

	*res = off;
	return 0;
}

static int blk_request(struct lkl_disk disk, struct lkl_blk_req *req)
{
	int err = 0;
	struct iovec *iovec = (struct iovec *)req->buf;

	/* TODO: handle short reads/writes */
	switch (req->type) {
	case LKL_DEV_BLK_TYPE_READ:
		err = preadv(disk.fd, iovec, req->count, req->sector * 512);
		break;
	case LKL_DEV_BLK_TYPE_WRITE:
		err = pwritev(disk.fd, iovec, req->count, req->sector * 512);
		break;
	case LKL_DEV_BLK_TYPE_FLUSH:
	case LKL_DEV_BLK_TYPE_FLUSH_OUT:
		err = fsync(disk.fd);
		break;
	default:
		return LKL_DEV_BLK_STATUS_UNSUP;
	}

	if (err < 0)
		return LKL_DEV_BLK_STATUS_IOERR;

	return LKL_DEV_BLK_STATUS_OK;
}

struct lkl_dev_blk_ops lkl_dev_blk_ops = {
	.get_capacity = fd_get_capacity,
	.request = blk_request,
};

struct lkl_netdev_rumpfd {
	struct lkl_netdev dev;
	/* TAP device */
	int fd;
};

static int rump_net_tx(struct lkl_netdev *nd,
		       struct iovec *iov, int cnt)
{
	struct lkl_netdev_rumpfd *nd_rumpfd =
		container_of(nd, struct lkl_netdev_rumpfd, dev);
	int ret;

	do {
		ret = writev(nd_rumpfd->fd, iov, cnt);
	} while (ret == -1 && (errno == EINTR));

	if (ret < 0)
		lkl_perror("write to rump fd netdev fails", errno);

	return ret;
}

static int rump_net_rx(struct lkl_netdev *nd,
		       struct iovec *iov, int cnt)
{
	struct lkl_netdev_rumpfd *nd_rumpfd =
		container_of(nd, struct lkl_netdev_rumpfd, dev);
	int ret;

	do {
		ret = readv(nd_rumpfd->fd, iov, cnt);
	} while (ret == -1 && errno == EINTR);

	if (ret <= 0)
		return -1;

	return ret;
}

static int rump_net_poll(struct lkl_netdev *nd)
{
	struct lkl_netdev_rumpfd *nd_rumpfd =
		container_of(nd, struct lkl_netdev_rumpfd, dev);
	struct pollfd pfd = {
		.fd = nd_rumpfd->fd,
		.events = POLLIN | POLLPRI | POLLOUT
	};
	int ret = 0;


	while (1) {
		int err = poll(&pfd, 1, -1);

		if (err < 0 && errno == EINTR)
			continue;
		if (err > 0)
			break;
	}

	if (pfd.revents & (POLLHUP | POLLNVAL))
		return -1;

	if (pfd.revents & POLLIN)
		ret |= LKL_DEV_NET_POLL_RX;
	if (pfd.revents & POLLOUT)
		ret |= LKL_DEV_NET_POLL_TX;

	return ret;
}

struct lkl_dev_net_ops rumpfd_ops = {
	.tx = rump_net_tx,
	.rx = rump_net_rx,
	.poll = rump_net_poll,
};

struct lkl_netdev *lkl_netdev_rumpfd_create(const char *ifname, int fd,
					    struct lkl_netdev_args *args)
{
	struct lkl_netdev_rumpfd *nd;
	char offload[8];

	nd = (struct lkl_netdev_rumpfd *)
		malloc(sizeof(struct lkl_netdev_rumpfd));
	if (!nd) {
		lkl_printf("tap: failed to allocate memory\n");
		return NULL;
	}

	memset(args, 0, sizeof(struct lkl_netdev_args));
	nd->fd = fd;
	nd->dev.ops = &rumpfd_ops;

	if (rumpuser_getparam("LKL_OFFLOAD", offload, sizeof(offload)) == 0) {
		args->offload  = BIT(LKL_VIRTIO_NET_F_GUEST_CSUM) |
			BIT(LKL_VIRTIO_NET_F_GUEST_TSO4) |
			BIT(LKL_VIRTIO_NET_F_MRG_RXBUF) |
			BIT(LKL_VIRTIO_NET_F_CSUM) |
			BIT(LKL_VIRTIO_NET_F_HOST_TSO4);
		nd->dev.has_vnet_hdr = 1;
	}

	return (struct lkl_netdev *)nd;
}
#endif
