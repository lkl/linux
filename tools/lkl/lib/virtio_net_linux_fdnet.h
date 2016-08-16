#ifndef _VIRTIO_NET_LINUX_FDNET_H
#define _VIRTIO_NET_LINUX_FDNET_H

struct ifreq;

struct lkl_netdev_linux_fdnet {
	struct lkl_netdev dev;
	/* file-descriptor based device */
	int fd;
	/* Needed to initiate shutdown */
	int eventfd;
	/* epoll fds for rx and tx */
	int epoll_fd;
};

extern struct lkl_netdev_linux_fdnet_ops {
	/*
	 * We need this so that we can "unhijack" this function in
	 * case we decided to hijack it.
	 */
	int (*eventfd)(unsigned int initval, int flags);
} lkl_netdev_linux_fdnet_ops;

/**
 * lkl_register_netdev_linux_fdnet - register a file descriptor-based network
 * device as a NIC
 *
 * @fd - a POSIX file descriptor number for input/output
 * @returns a struct lkl_netdev_linux_fdnet entry for virtio-net
 */
struct lkl_netdev_linux_fdnet *lkl_register_netdev_linux_fdnet(int fd);


/**
 * lkl_unregister_netdev_linux_fdnet - unregister a file descriptor-based
 * network device as a NIC
 *
 * @nd - a struct lkl_netdev_linux_fdnet entry to be unregistered
 */
void lkl_unregister_netdev_linux_fdnet(struct lkl_netdev_linux_fdnet *nd);

/**
 * lkl_netdev_tap_init - initialize tap related structure fot lkl_netdev.
 *
 * @path - the path to open the device.
 * @offload - offload bits for the device
 * @ifr - struct ifreq for ioctl.
 */
struct lkl_netdev_linux_fdnet *lkl_netdev_tap_init(const char *path,
						   int offload,
						   struct ifreq *ifr);

#endif /* _VIRTIO_NET_LINUX_FDNET_H*/
