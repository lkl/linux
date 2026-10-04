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
#include <poll.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#endif

#include <slirp/libslirp.h>

#include "virtio.h"
#include <lkl_host.h>

/* slirp_ssize_t was introduced in libslirp 4.8.0 */
#if !SLIRP_CHECK_VERSION(4, 8, 0)
#define slirp_ssize_t ssize_t
#endif

/* ---- Platform abstraction ---- */

/* Both ends are non-blocking; used only to wake up poll() */
struct slirp_pipe {
#ifdef __MINGW32__
	SOCKET rd, wr;
#else
	int rd, wr;
#endif
};

#ifdef __MINGW32__
#define SLIRP_POLLPRI 0

static int slirp_set_nonblock(SOCKET s)
{
	u_long mode = 1;

	return ioctlsocket(s, FIONBIO, &mode) == SOCKET_ERROR ? -1 : 0;
}

/* Emulate a pipe with a loopback TCP connection */
static int slirp_pipe_open(struct slirp_pipe *p)
{
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

	if (slirp_set_nonblock(s1) < 0 || slirp_set_nonblock(s2) < 0) {
		closesocket(s1);
		closesocket(s2);
		return -1;
	}

	p->rd = s2;
	p->wr = s1;
	return 0;
}

static void slirp_pipe_close(struct slirp_pipe *p)
{
	closesocket(p->rd);
	closesocket(p->wr);
}

static void slirp_pipe_kick(struct slirp_pipe *p)
{
	char c = 0;

	/* A full pipe already guarantees a pending wakeup */
	send(p->wr, &c, 1, 0);
}

static void slirp_pipe_drain(struct slirp_pipe *p)
{
	char buf[256];

	while (recv(p->rd, buf, sizeof(buf), 0) > 0)
		;
}

static int64_t slirp_clock_ns(void)
{
	LARGE_INTEGER freq, count;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	return (int64_t)((double)count.QuadPart / freq.QuadPart * 1000000000.0);
}

static int slirp_do_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
	return WSAPoll(fds, nfds, timeout_ms);
}

static int slirp_parse_addr(const char *str, struct in_addr *addr)
{
	addr->s_addr = inet_addr(str);
	return addr->s_addr == INADDR_NONE ? -1 : 0;
}

#else /* POSIX */

#define SLIRP_POLLPRI POLLPRI

static int slirp_set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int slirp_pipe_open(struct slirp_pipe *p)
{
	int fds[2];

	if (pipe(fds) < 0)
		return -1;

	if (slirp_set_nonblock(fds[0]) < 0 || slirp_set_nonblock(fds[1]) < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}

	p->rd = fds[0];
	p->wr = fds[1];
	return 0;
}

static void slirp_pipe_close(struct slirp_pipe *p)
{
	close(p->rd);
	close(p->wr);
}

static void slirp_pipe_kick(struct slirp_pipe *p)
{
	char c = 0;

	/* A full pipe already guarantees a pending wakeup */
	if (write(p->wr, &c, 1) < 0)
		return;
}

static void slirp_pipe_drain(struct slirp_pipe *p)
{
	char buf[256];

	while (read(p->rd, buf, sizeof(buf)) > 0)
		;
}

static int64_t slirp_clock_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int slirp_do_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
	int ret;

	do {
		ret = poll(fds, nfds, timeout_ms);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static int slirp_parse_addr(const char *str, struct in_addr *addr)
{
	return inet_aton(str, addr) ? 0 : -1;
}

#endif /* __MINGW32__ */

/* ---- Data structures ---- */

#define PKT_RING_SIZE 256

struct pkt_entry {
	uint8_t *data;
	int len;
};

struct lkl_netdev_slirp {
	struct lkl_netdev dev;
	Slirp *slirp;

	/*
	 * libslirp is not thread safe: serializes all libslirp calls made
	 * from the virtio TX path, the slirp poll thread and the hostfwd API.
	 */
	struct lkl_mutex *slirp_lock;
	lkl_thread_t poll_tid;
	int running;
	/* wakes up the slirp poll thread */
	struct slirp_pipe wake;

	/* packets from slirp waiting to be received by the guest */
	struct pkt_entry rx_ring[PKT_RING_SIZE];
	int rx_head;
	int rx_tail;
	int hup;
	struct lkl_mutex *rx_lock;
	/* wakes up the virtio net poll thread */
	struct slirp_pipe pipe;
};

/* ---- slirp callbacks ---- */

static slirp_ssize_t slirp_send_packet_cb(const void *buf, size_t len,
					  void *opaque)
{
	struct lkl_netdev_slirp *nd = opaque;
	struct pkt_entry *pkt;
	int next;

	lkl_host_ops.mutex_lock(nd->rx_lock);
	next = (nd->rx_head + 1) % PKT_RING_SIZE;
	if (next == nd->rx_tail || nd->hup) {
		/* Ring full or device gone: drop the packet */
		lkl_host_ops.mutex_unlock(nd->rx_lock);
		return (slirp_ssize_t)len;
	}

	pkt = &nd->rx_ring[nd->rx_head];
	pkt->data = malloc(len);
	if (!pkt->data) {
		lkl_host_ops.mutex_unlock(nd->rx_lock);
		return -1;
	}
	memcpy(pkt->data, buf, len);
	pkt->len = (int)len;
	nd->rx_head = next;

	slirp_pipe_kick(&nd->pipe);
	lkl_host_ops.mutex_unlock(nd->rx_lock);

	return (slirp_ssize_t)len;
}

static void slirp_guest_error_cb(const char *msg, void *opaque)
{
	fprintf(stderr, "slirp guest error: %s\n", msg);
}

static int64_t slirp_clock_get_ns_cb(void *opaque)
{
	return slirp_clock_ns();
}

struct slirp_timer {
	SlirpTimerCb cb;
	void *cb_opaque;
	int64_t expire_ms;
};

static void *slirp_timer_new_cb(SlirpTimerCb cb, void *cb_opaque, void *opaque)
{
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
	free(timer);
}

static void slirp_timer_mod_cb(void *timer, int64_t expire_time, void *opaque)
{
	struct slirp_timer *t = timer;

	t->expire_ms = expire_time;
}

static void slirp_register_poll_fd_cb(int fd, void *opaque)
{
}

static void slirp_unregister_poll_fd_cb(int fd, void *opaque)
{
}

static void slirp_notify_cb(void *opaque)
{
	struct lkl_netdev_slirp *nd = opaque;

	slirp_pipe_kick(&nd->wake);
}

static const SlirpCb slirp_callbacks = {
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
	struct pollfd *fds;
	int nfds;
	int capacity;
};

static int add_poll_cb(int fd, int events, void *opaque)
{
	struct poll_state *ps = opaque;
	struct pollfd *pfd;

	if (ps->nfds >= ps->capacity) {
		int capacity = ps->capacity ? ps->capacity * 2 : 16;
		struct pollfd *fds;

		fds = realloc(ps->fds, sizeof(*fds) * capacity);
		if (!fds)
			return -1;
		ps->fds = fds;
		ps->capacity = capacity;
	}

	pfd = &ps->fds[ps->nfds];
	pfd->fd = fd;
	pfd->events = 0;
	pfd->revents = 0;
	if (events & SLIRP_POLL_IN)
		pfd->events |= POLLIN;
	if (events & SLIRP_POLL_OUT)
		pfd->events |= POLLOUT;
	if (events & SLIRP_POLL_PRI)
		pfd->events |= SLIRP_POLLPRI;

	return ps->nfds++;
}

static int get_revents_cb(int idx, void *opaque)
{
	struct poll_state *ps = opaque;
	int revents = 0;

	if (idx < 0 || idx >= ps->nfds)
		return 0;

	if (ps->fds[idx].revents & POLLIN)
		revents |= SLIRP_POLL_IN;
	if (ps->fds[idx].revents & POLLOUT)
		revents |= SLIRP_POLL_OUT;
	if (ps->fds[idx].revents & SLIRP_POLLPRI)
		revents |= SLIRP_POLL_PRI;
	if (ps->fds[idx].revents & POLLERR)
		revents |= SLIRP_POLL_ERR;
	if (ps->fds[idx].revents & POLLHUP)
		revents |= SLIRP_POLL_HUP;

	return revents;
}

static void slirp_poll_thread(void *arg)
{
	struct lkl_netdev_slirp *nd = arg;
	struct poll_state ps = {0};

	for (;;) {
		uint32_t timeout = 100;
		int ret;

		lkl_host_ops.mutex_lock(nd->slirp_lock);
		if (!nd->running) {
			lkl_host_ops.mutex_unlock(nd->slirp_lock);
			break;
		}
		ps.nfds = 0;
		add_poll_cb((int)nd->wake.rd, SLIRP_POLL_IN, &ps);
		slirp_pollfds_fill(nd->slirp, &timeout, add_poll_cb, &ps);
		lkl_host_ops.mutex_unlock(nd->slirp_lock);

		ret = slirp_do_poll(ps.fds, ps.nfds, (int)timeout);
		if (ret > 0 && (ps.fds[0].revents & POLLIN))
			slirp_pipe_drain(&nd->wake);

		lkl_host_ops.mutex_lock(nd->slirp_lock);
		slirp_pollfds_poll(nd->slirp, ret < 0, get_revents_cb, &ps);
		lkl_host_ops.mutex_unlock(nd->slirp_lock);
	}

	free(ps.fds);
}

/* ---- LKL netdev ops ---- */

static int slirp_net_tx(struct lkl_netdev *dev, struct iovec *iov, int cnt)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);
	uint8_t *buf;
	int i, off, total = 0;

	for (i = 0; i < cnt; i++)
		total += iov[i].iov_len;

	buf = malloc(total);
	if (!buf)
		return -1;

	for (i = 0, off = 0; i < cnt; i++) {
		memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
		off += iov[i].iov_len;
	}

	lkl_host_ops.mutex_lock(nd->slirp_lock);
	slirp_input(nd->slirp, buf, total);
	lkl_host_ops.mutex_unlock(nd->slirp_lock);

	free(buf);
	return total;
}

static int slirp_net_rx(struct lkl_netdev *dev, struct iovec *iov, int cnt)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);
	struct pkt_entry *pkt;
	int i, off = 0;

	lkl_host_ops.mutex_lock(nd->rx_lock);
	if (nd->rx_tail == nd->rx_head) {
		lkl_host_ops.mutex_unlock(nd->rx_lock);
		return -1;
	}

	pkt = &nd->rx_ring[nd->rx_tail];
	for (i = 0; i < cnt && off < pkt->len; i++) {
		int to_copy = pkt->len - off;

		if (to_copy > (int)iov[i].iov_len)
			to_copy = (int)iov[i].iov_len;
		memcpy(iov[i].iov_base, pkt->data + off, to_copy);
		off += to_copy;
	}

	free(pkt->data);
	pkt->data = NULL;
	pkt->len = 0;
	nd->rx_tail = (nd->rx_tail + 1) % PKT_RING_SIZE;
	lkl_host_ops.mutex_unlock(nd->rx_lock);

	return off;
}

static int slirp_net_poll(struct lkl_netdev *dev)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);
	struct pollfd pfd = {
		.fd = nd->pipe.rd,
		.events = POLLIN,
	};
	int ret = LKL_DEV_NET_POLL_TX;

	if (slirp_do_poll(&pfd, 1, -1) < 0)
		return -1;

	if (pfd.revents & (POLLHUP | POLLNVAL))
		return LKL_DEV_NET_POLL_HUP;

	if (pfd.revents & POLLIN)
		slirp_pipe_drain(&nd->pipe);

	lkl_host_ops.mutex_lock(nd->rx_lock);
	if (nd->rx_tail != nd->rx_head)
		ret |= LKL_DEV_NET_POLL_RX;
	lkl_host_ops.mutex_unlock(nd->rx_lock);

	return ret;
}

static void slirp_net_poll_hup(struct lkl_netdev *dev)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);

	/* Stop slirp_send_packet_cb() from using the pipe once closed */
	lkl_host_ops.mutex_lock(nd->rx_lock);
	nd->hup = 1;
	lkl_host_ops.mutex_unlock(nd->rx_lock);

	/* this will cause a POLLHUP / POLLNVAL in the poll function */
	slirp_pipe_close(&nd->pipe);
}

static void slirp_net_free(struct lkl_netdev *dev)
{
	struct lkl_netdev_slirp *nd =
		container_of(dev, struct lkl_netdev_slirp, dev);

	lkl_host_ops.mutex_lock(nd->slirp_lock);
	nd->running = 0;
	lkl_host_ops.mutex_unlock(nd->slirp_lock);
	slirp_pipe_kick(&nd->wake);
	lkl_host_ops.thread_join(nd->poll_tid);

	slirp_cleanup(nd->slirp);

	while (nd->rx_tail != nd->rx_head) {
		free(nd->rx_ring[nd->rx_tail].data);
		nd->rx_tail = (nd->rx_tail + 1) % PKT_RING_SIZE;
	}

	if (!nd->hup)
		slirp_pipe_close(&nd->pipe);
	slirp_pipe_close(&nd->wake);
	lkl_host_ops.mutex_free(nd->rx_lock);
	lkl_host_ops.mutex_free(nd->slirp_lock);
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
	struct lkl_netdev_slirp *nd;
	/* Only version 1 fields are used, accepted by any libslirp 4.x */
	SlirpConfig cfg = {
		.version = 1,
		.in_enabled = true,
		.in6_enabled = false,
		.vhostname = "lkl-host",
		.disable_host_loopback = false,
	};

#ifdef __MINGW32__
	WSADATA wsa;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return NULL;
#endif

	slirp_parse_addr("10.0.2.0", &cfg.vnetwork);
	slirp_parse_addr("255.255.255.0", &cfg.vnetmask);
	slirp_parse_addr("10.0.2.2", &cfg.vhost);
	slirp_parse_addr("10.0.2.15", &cfg.vdhcp_start);
	slirp_parse_addr("10.0.2.3", &cfg.vnameserver);

	nd = calloc(1, sizeof(*nd));
	if (!nd)
		return NULL;

	nd->slirp_lock = lkl_host_ops.mutex_alloc(0);
	nd->rx_lock = lkl_host_ops.mutex_alloc(0);
	if (!nd->slirp_lock || !nd->rx_lock) {
		fprintf(stderr, "slirp: failed to allocate mutexes\n");
		goto err_free_mutex;
	}

	if (slirp_pipe_open(&nd->pipe) < 0) {
		fprintf(stderr, "slirp: pipe creation failed\n");
		goto err_free_mutex;
	}

	if (slirp_pipe_open(&nd->wake) < 0) {
		fprintf(stderr, "slirp: pipe creation failed\n");
		goto err_close_pipe;
	}

	nd->slirp = slirp_new(&cfg, &slirp_callbacks, nd);
	if (!nd->slirp) {
		fprintf(stderr, "slirp: failed to create instance\n");
		goto err_close_wake;
	}

	nd->running = 1;
	nd->poll_tid = lkl_host_ops.thread_create(slirp_poll_thread, nd);
	if (!nd->poll_tid) {
		fprintf(stderr, "slirp: failed to create poll thread\n");
		goto err_cleanup;
	}

	nd->dev.ops = &slirp_net_ops;
	return &nd->dev;

err_cleanup:
	slirp_cleanup(nd->slirp);
err_close_wake:
	slirp_pipe_close(&nd->wake);
err_close_pipe:
	slirp_pipe_close(&nd->pipe);
err_free_mutex:
	if (nd->rx_lock)
		lkl_host_ops.mutex_free(nd->rx_lock);
	if (nd->slirp_lock)
		lkl_host_ops.mutex_free(nd->slirp_lock);
	free(nd);
	return NULL;
}

int lkl_netdev_slirp_add_hostfwd(struct lkl_netdev *nd, int is_udp,
				 const char *host_addr, int host_port,
				 const char *guest_addr, int guest_port)
{
	struct lkl_netdev_slirp *nds =
		container_of(nd, struct lkl_netdev_slirp, dev);
	struct in_addr haddr, gaddr;
	int ret;

	if (slirp_parse_addr(host_addr, &haddr) < 0 ||
	    slirp_parse_addr(guest_addr, &gaddr) < 0)
		return -1;

	lkl_host_ops.mutex_lock(nds->slirp_lock);
	ret = slirp_add_hostfwd(nds->slirp, is_udp, haddr, host_port,
				gaddr, guest_port);
	lkl_host_ops.mutex_unlock(nds->slirp_lock);
	/* make the poll thread pick up the new listening socket */
	slirp_pipe_kick(&nds->wake);

	return ret;
}

int lkl_netdev_slirp_remove_hostfwd(struct lkl_netdev *nd, int is_udp,
				    const char *host_addr, int host_port)
{
	struct lkl_netdev_slirp *nds =
		container_of(nd, struct lkl_netdev_slirp, dev);
	struct in_addr haddr;
	int ret;

	if (slirp_parse_addr(host_addr, &haddr) < 0)
		return -1;

	lkl_host_ops.mutex_lock(nds->slirp_lock);
	ret = slirp_remove_hostfwd(nds->slirp, is_udp, haddr, host_port);
	lkl_host_ops.mutex_unlock(nds->slirp_lock);

	return ret;
}
