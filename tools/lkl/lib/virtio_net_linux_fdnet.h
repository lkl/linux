#ifndef _VIRTIO_NET_LINUX_FDNET_H
#define _VIRTIO_NET_LINUX_FDNET_H

struct lkl_netdev_linux_fdnet {
	struct lkl_netdev dev;
	/* file-descriptor based device */
	int fd;
	/* Needed to initiate shutdown */
	int eventfd;
	/* epoll fds for rx and tx */
	int epoll_rx_fd;
	int epoll_tx_fd;
};

extern struct lkl_netdev_linux_fdnet_ops {
	/*
	 * We need this so that we can "unhijack" this function in
	 * case we decided to hijack it.
	 */
	int (*eventfd)(unsigned int initval, int flags);
} lkl_netdev_linux_fdnet_ops;

struct lkl_netdev_linux_fdnet *lkl_register_netdev_linux_fdnet(int fd);
void lkl_unregister_netdev_linux_fdnet(struct lkl_netdev_linux_fdnet *nd);

#endif /* _VIRTIO_NET_LINUX_FDNET_H*/
