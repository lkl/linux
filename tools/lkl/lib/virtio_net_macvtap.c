/*
 * macvtap based virtual network interface feature for LKL
 * Copyright (c) 2016 Hajime Tazaki
 *
 * Author: Hajime Tazaki <thehajime@gmail.com>
 *
 * Current implementation is linux-specific.
 */

/*
 * You need to configure host device in advance.
 *
 * sudo ip link add link eth0 name vtap0 type macvtap mode passthru
 * sudo ip link set dev vtap0 up
 * sudo chown thehajime /dev/tap22
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>

#include "virtio.h"
#include "virtio_net_linux_fdnet.h"

#define BIT(x) (1ULL << x)

struct lkl_netdev *lkl_netdev_macvtap_create(const char *path, int offload)
{
	struct lkl_netdev_linux_fdnet *nd;
	int fd, ret, tap_arg = 0;
	int vnet_hdr_sz = 0;

	struct ifreq ifr = {
		.ifr_flags = IFF_TAP | IFF_NO_PI,
	};

	if (offload & BIT(LKL_VIRTIO_NET_F_GUEST_CSUM))
		tap_arg |= TUN_F_CSUM;
	if (offload & (BIT(LKL_VIRTIO_NET_F_GUEST_TSO4) |
	    BIT(LKL_VIRTIO_NET_F_MRG_RXBUF)))
		tap_arg |= TUN_F_TSO4 | TUN_F_CSUM;

	if (tap_arg || (offload & (BIT(LKL_VIRTIO_NET_F_CSUM) |
	    BIT(LKL_VIRTIO_NET_F_HOST_TSO4)))) {
		ifr.ifr_flags |= IFF_VNET_HDR;
		vnet_hdr_sz = sizeof(struct lkl_virtio_net_hdr_v1);
	}

	fd = open(path, O_RDWR|O_NONBLOCK);
	if (fd < 0) {
		perror("open");
		return NULL;
	}

	ret = ioctl(fd, TUNSETIFF, &ifr);
	if (ret < 0) {
		fprintf(stderr, "tap: failed to attach to %s: %s\n",
			ifr.ifr_name, strerror(errno));
		close(fd);
		return NULL;
	}
	if (vnet_hdr_sz && ioctl(fd, TUNSETVNETHDRSZ, &vnet_hdr_sz) != 0) {
		fprintf(stderr, "tap: failed to TUNSETVNETHDRSZ to %s: %s\n",
			ifr.ifr_name, strerror(errno));
		close(fd);
		return NULL;
	}
	if (tap_arg && ioctl(fd, TUNSETOFFLOAD, tap_arg) != 0) {
		fprintf(stderr, "macvtap: failed to TUNSETOFFLOAD to %s: %s\n",
			path, strerror(errno));
		close(fd);
		return NULL;
	}
	nd = lkl_register_netdev_linux_fdnet(fd);
	if (!nd) {
		perror("failed to register to.");
		return NULL;
	}
	nd->dev.has_vnet_hdr = (vnet_hdr_sz != 0);
	return (struct lkl_netdev *)nd;
}
