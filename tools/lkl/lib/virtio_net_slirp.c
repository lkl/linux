// SPDX-License-Identifier: GPL-2.0
/*
 * libslirp based virtual network interface for LKL
 *
 * Provides user-mode networking without requiring root privileges.
 * Supports port forwarding from host to LKL guest network.
 * Portable: works on both POSIX and Win32 (MinGW).
 *
 * Copyright (c) 2025 Sheldon Qi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef __MINGW32__
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#endif

#include <slirp/libslirp.h>

#include "virtio.h"
#include <lkl_host.h>

/* ---- Platform abstraction ---- */

#ifdef __MINGW32__
typedef CRITICAL_SECTION slirp_mutex_t;
typedef HANDLE slirp_thread_t;
typedef SOCKET slirp_fd_t;
#define SLIRP_INVALID_FD INVALID_SOCKET

static inline void slirp_mutex_init(slirp_mutex_t *m) { InitializeCriticalSection(m); }
static inline void slirp_mutex_destroy(slirp_mutex_t *m) { DeleteCriticalSection(m); }
static inline void slirp_mutex_lock(slirp_mutex_t *m) { EnterCriticalSection(m); }
static inline void slirp_mutex_unlock(slirp_mutex_t *m) { LeaveCriticalSection(m); }

static int slirp_socketpair(slirp_fd_t pair[2])
{
	/* Create a loopback TCP connection pair (emulates pipe) */
	SOCKET listener, s1, s2;
	struct sockaddr_in addr;
	int addrlen = sizeof(addr);

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener == INVALID_SOCKET)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
	    listen(listener, 1) == SOCKET_ERROR ||
	    getsockname(listener, (struct sockaddr *)&addr, &addrlen) == SOCKET_ERROR) {
		closesocket(listener);
		return -1;
	}

	s1 = socket(AF_INET, SOCK_STREAM, 0);
	if (s1 == INVALID_SOCKET) {
		closesocket(listener);
		return -1;
	}

	if (connect(s1, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		closesocket(s1);
		closesocket(listener);
		return -1;
	}

	s2 = accept(listener, NULL, NULL);
	closesocket(listener);
	if (s2 == INVALID_SOCKET) {
		closesocket(s1);
		return -1;
	}

	/* Set non-blocking on read end */
	u_long mode = 1;

	ioctlsocket(s2, FIONBIO, &mode);

	pair[0] = s2; /* read end (non-blocking) */
	pair[1] = s1; /* write end */
	return 0;
}

static inline void slirp_pipe_close(slirp_fd_t fd) { closesocket(fd); }

static inline int slirp_pipe_write(slirp_fd_t fd, const char *buf, int len)
{
	return send(fd, buf, len, 0);
}

static inline int slirp_pipe_read(slirp_fd_t fd, char *buf, int len)
{
	return recv(fd, buf, len, 0);
}

static int64_t slirp_clock_ns(void)
{
	LARGE_INTEGER freq, count;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	return (int64_t)((double)count.QuadPart / freq.QuadPart * 1000000000.0);
}

static int slirp_do_poll(WSAPOLLFD *fds, int nfds, int timeout_ms)
{
	return WSAPoll(fds, nfds, timeout_ms);
}

#else /* POSIX */

typedef pthread_mutex_t slirp_mutex_t;
typedef pthread_t slirp_thread_t;
typedef int slirp_fd_t;
#define SLIRP_INVALID_FD (-1)

static inline void slirp_mutex_init(slirp_mutex_t *m) { pthread_mutex_init(m, NULL); }
static inline void slirp_mutex_destroy(slirp_mutex_t *m) { pthread_mutex_destroy(m); }
static inline void slirp_mutex_lock(slirp_mutex_t *m) { pthread_mutex_lock(m); }
static inline void slirp_mutex_unlock(slirp_mutex_t *m) { pthread_mutex_unlock(m); }

static int slirp_socketpair(slirp_fd_t pair[2])
{
	if (pipe(pair) < 0)
		return -1;
	fcntl(pair[0], F_SETFL, O_NONBLOCK);
	return 0;
}

static inline void slirp_pipe_close(slirp_fd_t fd) { close(fd); }
static inline int slirp_pipe_write(slirp_fd_t fd, const char *buf, int len)
{
	return (int)write(fd, buf, len);
}
static inline int slirp_pipe_read(slirp_fd_t fd, char *buf, int len)
{
	return (int)read(fd, buf, len);
}

static int64_t slirp_clock_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int slirp_do_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
	return poll(fds, nfds, timeout_ms);
}

#endif /* __MINGW32__ */

/* Unified pollfd type */
#ifdef __MINGW32__
typedef WSAPOLLFD slirp_pollfd_t;
#define SLIRP_POLLIN  POLLIN
#define SLIRP_POLLOUT POLLOUT
#define SLIRP_POLLPRI 0
#define SLIRP_POLLERR POLLERR
#define SLIRP_POLLHUP POLLHUP
#define SLIRP_POLLNVAL POLLNVAL
#else
typedef struct pollfd slirp_pollfd_t;
#define SLIRP_POLLIN  POLLIN
#define SLIRP_POLLOUT POLLOUT
#define SLIRP_POLLPRI POLLPRI
#define SLIRP_POLLERR POLLERR
#define SLIRP_POLLHUP POLLHUP
#define SLIRP_POLLNVAL POLLNVAL
#endif

/* ---- Data structures ---- */

#define PKT_RING_SIZE 256
#define PKT_MAX_SIZE  65536

struct pkt_entry {
	uint8_t *data;
	int len;
};

struct lkl_netdev_slirp {
	struct lkl_netdev dev;
	Slirp *slirp;
	slirp_thread_t poll_thread;
	int running;

	struct pkt_entry rx_ring[PKT_RING_SIZE];
	int rx_head;
	int rx_tail;
	slirp_mutex_t rx_lock;

	slirp_fd_t pipe[2];
	int poll_rx;
};

/* ---- slirp callbacks ---- */

static slirp_ssize_t slirp_send_packet_cb(const void *buf, size_t len, void *opaque)
{
	struct lkl_netdev_slirp *nd = opaque;

	slirp_mutex_lock(&nd->rx_lock);
	int next = (nd->rx_head + 1) % PKT_RING_SIZE;

	if (next == nd->rx_tail) {
		slirp_mutex_unlock(&nd->rx_lock);
		return (slirp_ssize_t)len;
	}

	nd->rx_ring[nd->rx_head].data = malloc(len);
	if (!nd->rx_ring[nd->rx_head].data) {
		slirp_mutex_unlock(&nd->rx_lock);
		return -1;
	}
	memcpy(nd->rx_ring[nd->rx_head].data, buf, len);
	nd->rx_ring[nd->rx_head].len = (int)len;
	nd->rx_head = next;
	slirp_mutex_unlock(&nd->rx_lock);

	char c = 'r';

	slirp_pipe_write(nd->pipe[1], &c, 1);
	return (slirp_ssize_t)len;
}

static void slirp_guest_error_cb(const char *msg, void *opaque)
{
	(void)opaque;
	fprintf(stderr, "slirp guest error: %s\n", msg);
}

static int64_t slirp_clock_get_ns_cb(void *opaque)
{
	(void)opaque;
	return slirp_clock_ns();
}

struct slirp_timer {
	SlirpTimerCb cb;
	void *cb_opaque;
	int64_t expire_ms;
};

static void *slirp_timer_new_cb(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
	(void)opaque;
	struct slirp_timer *t = calloc(1, sizeof(*t));

	if (t) {
		t->cb = cb;
		t->cb_opaque = cb_opaque;
		t->expire_ms = -1;
	}
	return t;
}

static void slirp_timer_free_cb(void *timer, void *opaque)
{
	(void)opaque;
	free(timer);
}

static void slirp_timer_mod_cb(void *timer, int64_t expire_time, void *opaque)
{
	(void)opaque;
	struct slirp_timer *t = timer;

	t->expire_ms = expire_time;
}

static void slirp_register_poll_fd_cb(int fd, void *opaque)
{
	(void)fd;
	(void)opaque;
}

static void slirp_unregister_poll_fd_cb(int fd, void *opaque)
{
	(void)fd;
	(void)opaque;
}

static void slirp_notify_cb(void *opaque)
{
	struct lkl_netdev_slirp *nd = opaque;
	char c = 'n';

	slirp_pipe_write(nd->pipe[1], &c, 1);
}

static SlirpCb slirp_callbacks = {
	.send_packet = slirp_send_packet_cb,
	.guest_error = slirp_guest_error_cb,
	.clock_get_ns = slirp_clock_get_ns_cb,
	.timer_new = slirp_timer_new_cb,
	.timer_free = slirp_timer_free_cb,
	.timer_mod = slirp_timer_mod_cb,
	.register_poll_fd = slirp_register_poll_fd_cb,
	.unregister_poll_fd = slirp_unregister_poll_fd_cb,
	.notify = slirp_notify_cb,
};

/* ---- slirp poll thread ---- */

struct poll_state {
	slirp_pollfd_t *fds;
	int nfds;
	int capacity;
};

static int add_poll_cb(int fd, int events, void *opaque)
{
	struct poll_state *ps = opaque;

	if (ps->nfds >= ps->capacity) {
		ps->capacity = ps->capacity ? ps->capacity * 2 : 16;
		ps->fds = realloc(ps->fds, sizeof(slirp_pollfd_t) * ps->capacity);
	}

	int idx = ps->nfds++;

	ps->fds[idx].fd = fd;
	ps->fds[idx].events = 0;
	ps->fds[idx].revents = 0;
	if (events & SLIRP_POLL_IN)
		ps->fds[idx].events |= SLIRP_POLLIN;
	if (events & SLIRP_POLL_OUT)
		ps->fds[idx].events |= SLIRP_POLLOUT;
#if SLIRP_POLLPRI
	if (events & SLIRP_POLL_PRI)
		ps->fds[idx].events |= SLIRP_POLLPRI;
#endif
	return idx;
}

static int get_revents_cb(int idx, void *opaque)
{
	struct poll_state *ps = opaque;
	int revents = 0;

	if (idx < 0 || idx >= ps->nfds)
		return 0;

	if (ps->fds[idx].revents & SLIRP_POLLIN)
		revents |= SLIRP_POLL_IN;
	if (ps->fds[idx].revents & SLIRP_POLLOUT)
		revents |= SLIRP_POLL_OUT;
	if (ps->fds[idx].revents & SLIRP_POLLERR)
		revents |= SLIRP_POLL_ERR;
	if (ps->fds[idx].revents & SLIRP_POLLHUP)
		revents |= SLIRP_POLL_HUP;
	return revents;
}

#ifdef __MINGW32__
static DWORD WINAPI slirp_poll_thread_fn(void *arg)
#else
static void *slirp_poll_thread_fn(void *arg)
#endif
{
	struct lkl_netdev_slirp *nd = arg;
	struct poll_state ps = {0};

	while (nd->running) {
		uint32_t timeout = 100;

		ps.nfds = 0;

		slirp_pollfds_fill(nd->slirp, &timeout, add_poll_cb, &ps);

		int ret = slirp_do_poll(ps.fds, ps.nfds, (int)timeout);
#ifndef __MINGW32__
		if (ret < 0 && errno == EINTR)
			continue;
#endif

		slirp_pollfds_poll(nd->slirp, ret < 0 ? 1 : 0,
				   get_revents_cb, &ps);
	}

	free(ps.fds);
#ifdef __MINGW32__
	return 0;
#else
	return NULL;
#endif
}

/* ---- LKL netdev ops ---- */

static int slirp_net_tx(struct lkl_netdev *dev, struct iovec *iov, int cnt)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);

	int total = 0;

	for (int i = 0; i < cnt; i++)
		total += iov[i].iov_len;

	uint8_t *buf = malloc(total);

	if (!buf)
		return -1;

	int off = 0;

	for (int i = 0; i < cnt; i++) {
		memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
		off += iov[i].iov_len;
	}

	slirp_input(nd->slirp, buf, total);
	free(buf);
	return total;
}

static int slirp_net_rx(struct lkl_netdev *dev, struct iovec *iov, int cnt)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);

	slirp_mutex_lock(&nd->rx_lock);
	if (nd->rx_tail == nd->rx_head) {
		slirp_mutex_unlock(&nd->rx_lock);
		nd->poll_rx = 1;
		return -1;
	}

	struct pkt_entry *pkt = &nd->rx_ring[nd->rx_tail];
	int pkt_len = pkt->len;

	int off = 0;

	for (int i = 0; i < cnt && off < pkt_len; i++) {
		int to_copy = pkt_len - off;
		if (to_copy > (int)iov[i].iov_len)
			to_copy = (int)iov[i].iov_len;
		memcpy(iov[i].iov_base, pkt->data + off, to_copy);
		off += to_copy;
	}

	free(pkt->data);
	pkt->data = NULL;
	pkt->len = 0;
	nd->rx_tail = (nd->rx_tail + 1) % PKT_RING_SIZE;
	slirp_mutex_unlock(&nd->rx_lock);

	return off;
}

static int slirp_net_poll(struct lkl_netdev *dev)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);

	slirp_pollfd_t pfd;

	pfd.fd = nd->pipe[0];
	pfd.events = SLIRP_POLLIN;
	pfd.revents = 0;

	int ret;

	do {
		ret = slirp_do_poll(&pfd, 1, -1);
#ifndef __MINGW32__
	} while (ret == -1 && errno == EINTR);
#else
	} while (0);
#endif

	if (ret < 0)
		return -1;

	if (pfd.revents & (SLIRP_POLLHUP | SLIRP_POLLNVAL))
		return LKL_DEV_NET_POLL_HUP;

	if (pfd.revents & SLIRP_POLLIN) {
		char tmp[256];
		slirp_pipe_read(nd->pipe[0], tmp, sizeof(tmp));
	}

	int result = LKL_DEV_NET_POLL_TX;

	slirp_mutex_lock(&nd->rx_lock);
	if (nd->rx_tail != nd->rx_head) {
		nd->poll_rx = 0;
		result |= LKL_DEV_NET_POLL_RX;
	}
	slirp_mutex_unlock(&nd->rx_lock);

	return result;
}

static void slirp_net_poll_hup(struct lkl_netdev *dev)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);

	nd->running = 0;
	slirp_pipe_close(nd->pipe[0]);
	slirp_pipe_close(nd->pipe[1]);
}

static void slirp_net_free(struct lkl_netdev *dev)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);

	nd->running = 0;
#ifdef __MINGW32__
	WaitForSingleObject(nd->poll_thread, INFINITE);
	CloseHandle(nd->poll_thread);
#else
	pthread_join(nd->poll_thread, NULL);
#endif
	slirp_cleanup(nd->slirp);

	while (nd->rx_tail != nd->rx_head) {
		free(nd->rx_ring[nd->rx_tail].data);
		nd->rx_tail = (nd->rx_tail + 1) % PKT_RING_SIZE;
	}

	slirp_mutex_destroy(&nd->rx_lock);
	free(nd);
}

static struct lkl_dev_net_ops slirp_net_ops = {
	.tx = slirp_net_tx,
	.rx = slirp_net_rx,
	.poll = slirp_net_poll,
	.poll_hup = slirp_net_poll_hup,
	.free = slirp_net_free,
};

/* ---- Public API ---- */

struct lkl_netdev *lkl_netdev_slirp_create(void)
{
	struct lkl_netdev_slirp *nd = calloc(1, sizeof(*nd));

	if (!nd)
		return NULL;

#ifdef __MINGW32__
	{
		WSADATA wsa;

		WSAStartup(MAKEWORD(2, 2), &wsa);
	}
#endif

	slirp_mutex_init(&nd->rx_lock);

	if (slirp_socketpair(nd->pipe) < 0) {
		fprintf(stderr, "slirp: pipe creation failed\n");
		free(nd);
		return NULL;
	}

	SlirpConfig cfg = {
		.version = 4,
		.in_enabled = true,
		.vnetwork = { .s_addr = inet_addr("10.0.2.0") },
		.vnetmask = { .s_addr = inet_addr("255.255.255.0") },
		.vhost = { .s_addr = inet_addr("10.0.2.2") },
		.in6_enabled = false,
		.vhostname = "lkl-host",
		.vdhcp_start = { .s_addr = inet_addr("10.0.2.15") },
		.vnameserver = { .s_addr = inet_addr("10.0.2.3") },
		.disable_host_loopback = false,
	};

	nd->slirp = slirp_new(&cfg, &slirp_callbacks, nd);
	if (!nd->slirp) {
		fprintf(stderr, "slirp: failed to create instance\n");
		slirp_pipe_close(nd->pipe[0]);
		slirp_pipe_close(nd->pipe[1]);
		slirp_mutex_destroy(&nd->rx_lock);
		free(nd);
		return NULL;
	}

	nd->running = 1;
#ifdef __MINGW32__
	nd->poll_thread = CreateThread(NULL, 0, slirp_poll_thread_fn, nd, 0, NULL);
	if (!nd->poll_thread) {
		fprintf(stderr, "slirp: failed to create poll thread\n");
		slirp_cleanup(nd->slirp);
		slirp_pipe_close(nd->pipe[0]);
		slirp_pipe_close(nd->pipe[1]);
		slirp_mutex_destroy(&nd->rx_lock);
		free(nd);
		return NULL;
	}
#else
	if (pthread_create(&nd->poll_thread, NULL, slirp_poll_thread_fn, nd) != 0) {
		fprintf(stderr, "slirp: failed to create poll thread\n");
		slirp_cleanup(nd->slirp);
		slirp_pipe_close(nd->pipe[0]);
		slirp_pipe_close(nd->pipe[1]);
		slirp_mutex_destroy(&nd->rx_lock);
		free(nd);
		return NULL;
	}
#endif

	nd->dev.ops = &slirp_net_ops;
	return &nd->dev;
}

int lkl_netdev_slirp_add_hostfwd(struct lkl_netdev *nd, int is_udp,
				  const char *host_addr, int host_port,
				  const char *guest_addr, int guest_port)
{
	struct lkl_netdev_slirp *nds =
		container_of(nd, struct lkl_netdev_slirp, dev);

	struct in_addr haddr, gaddr;
#ifdef __MINGW32__
	haddr.s_addr = inet_addr(host_addr);
	gaddr.s_addr = inet_addr(guest_addr);
#else
	inet_aton(host_addr, &haddr);
	inet_aton(guest_addr, &gaddr);
#endif

	return slirp_add_hostfwd(nds->slirp, is_udp, haddr, host_port,
				 gaddr, guest_port);
}

int lkl_netdev_slirp_remove_hostfwd(struct lkl_netdev *nd, int is_udp,
				     const char *host_addr, int host_port)
{
	struct lkl_netdev_slirp *nds =
		container_of(nd, struct lkl_netdev_slirp, dev);

	struct in_addr haddr;
#ifdef __MINGW32__
	haddr.s_addr = inet_addr(host_addr);
#else
	inet_aton(host_addr, &haddr);
#endif

	return slirp_remove_hostfwd(nds->slirp, is_udp, haddr, host_port);
}
