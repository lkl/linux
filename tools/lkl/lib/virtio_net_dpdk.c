/*
 * Intel DPDK based virtual network interface feature for NUSE
 * Copyright (c) 2015,2016 Ryo Nakamura, Hajime Tazaki
 *
 * Author: Ryo Nakamura <upa@wide.ad.jp>
 *         Hajime Tazaki <tazaki@sfc.wide.ad.jp>
 */


#include <stdio.h>
#ifdef LKL_DPDK
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_config.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_pci.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_byteorder.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>


#undef st_atime
#undef st_mtime
#undef st_ctime
#include <lkl_host.h>

static char * const ealargs[] = {
	"nuse_vif_dpdk",
	"-c 1",
	"-n 1",
};

#define MAX_PKT_BURST           16
#define MEMPOOL_CACHE_SZ        32
#define MAX_PACKET_SZ           2048
#define MBUF_NUM                512
#define MBUF_SIZ        \
	(MAX_PACKET_SZ + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define NUMDESC         512	/* nb_min on vmxnet3 is 512 */
#define NUMQUEUE        1

static int portid;

struct lkl_netdev_dpdk {
	struct lkl_dev_net_ops *ops;
	int portid;
	struct rte_mempool *rxpool, *txpool; /* rin buffer pool */
	char txpoolname[16], rxpoolname[16];
	/* burst receive context by rump dpdk code */
	struct rte_mbuf *rms[MAX_PKT_BURST];
	int npkts;
	int bufidx;
};

static int net_tx(struct lkl_netdev *nd, void *data, int len)
{
	void *pkt;
	struct rte_mbuf *rm;
	struct lkl_netdev_dpdk *nd_dpdk;

	nd_dpdk = (struct lkl_netdev_dpdk *) nd;

	rm = rte_pktmbuf_alloc(nd_dpdk->txpool);
	pkt = rte_pktmbuf_append(rm, len);
	memcpy(pkt, data, len);

	/* XXX: should be bulk-trasmitted !! */
	rte_eth_tx_burst(nd_dpdk->portid, 0, &rm, 1);

	return 0;
}

static int net_rx(struct lkl_netdev *nd, void *data, int *len)
{
	struct lkl_netdev_dpdk *nd_dpdk;
	int i, nb_rx, read = 0;

	nd_dpdk = (struct lkl_netdev_dpdk *) nd;

	nb_rx = rte_eth_rx_burst(nd_dpdk->portid, 0,
					  nd_dpdk->rms, MAX_PKT_BURST);
	nd_dpdk->bufidx = 0;
	if (nb_rx <= 0) {
		/* XXX: need to implement proper poll()
		 * or interrupt mode PMD of dpdk, which is only availbale
		 * on ixgbe/igb/e1000 (as of Jan. 2016)
		 */
		usleep(10*1000);
		return -1;
	}

	nd_dpdk->npkts = nb_rx;
	while (nd_dpdk->npkts > 0) {
		struct rte_mbuf *rm, *rm0;
		void *r_data;
		uint32_t r_size;

		rm0 = nd_dpdk->rms[nd_dpdk->bufidx];
		nd_dpdk->npkts--;
		nd_dpdk->bufidx++;

		for (rm = rm0; rm; rm = rm->next) {
			r_data = rte_pktmbuf_mtod(rm, void *);
			r_size = rte_pktmbuf_data_len(rm);

			*len -= r_size;
			if (*len < 0) {
				fprintf(stderr, "dpdk: buffer full. skip it\n");
				goto end;
			}

#ifdef DEBUG
			fprintf(stderr, "dpdk: copy pkt len=%d\n", r_size);
#endif
			/* XXX */
			memcpy(data, r_data, r_size);

			read += r_size;
			data += r_size;
		}

	}

end:
	for (i = 0; i < nb_rx; i++)
		rte_pktmbuf_free(nd_dpdk->rms[i]);

	*len = read;
	return 0;
}

static int net_poll(struct lkl_netdev *nd, int events)
{
	int ret = 0;

	if (events & LKL_DEV_NET_POLL_RX)
		ret |= LKL_DEV_NET_POLL_RX;
	if (events & LKL_DEV_NET_POLL_TX)
		ret |= LKL_DEV_NET_POLL_TX;

	return ret;
}

struct lkl_dev_net_ops dpdk_net_ops = {
	.tx = net_tx,
	.rx = net_rx,
	.poll = net_poll,
};


static int dpdk_init;
struct lkl_netdev *lkl_netdev_dpdk_create(const char *ifname)
{
	int ret = 0;
	struct rte_eth_conf portconf;
	struct rte_eth_link link;
	struct lkl_netdev_dpdk *nd;
	struct rte_eth_dev_info dev_info;
	struct ether_addr mac_addr;

	if (!dpdk_init) {
		ret = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
				   (void *)(uintptr_t)ealargs);
		if (ret < 0)
			fprintf(stderr, "dpdk: failed to initialize eal\n");

		dpdk_init = 1;
	}

	nd = malloc(sizeof(struct lkl_netdev_dpdk));
	memset(nd, 0, sizeof(struct lkl_netdev_dpdk));
	nd->ops = &dpdk_net_ops;
	nd->portid = portid++;
	snprintf(nd->txpoolname, 16, "%s%s", "tx-", ifname);
	snprintf(nd->rxpoolname, 16, "%s%s", "rx-", ifname);

	nd->txpool =
		rte_mempool_create(nd->txpoolname,
				   MBUF_NUM, MBUF_SIZ, MEMPOOL_CACHE_SZ,
				   sizeof(struct rte_pktmbuf_pool_private),
				   rte_pktmbuf_pool_init, NULL,
				   rte_pktmbuf_init, NULL, 0, 0);

	if (!nd->txpool) {
		fprintf(stderr, "dpdk: failed to allocate tx pool\n");
		free(nd);
		return 0;
	}


	nd->rxpool =
		rte_mempool_create(nd->rxpoolname, MBUF_NUM, MBUF_SIZ, 0,
				   sizeof(struct rte_pktmbuf_pool_private),
				   rte_pktmbuf_pool_init, NULL,
				   rte_pktmbuf_init, NULL, 0, 0);
	if (!nd->rxpool) {
		fprintf(stderr, "dpdk: failed to allocate rx pool\n");
		free(nd);
		return 0;
	}

	memset(&portconf, 0, sizeof(portconf));
	ret = rte_eth_dev_configure(nd->portid, NUMQUEUE, NUMQUEUE,
				    &portconf);
	if (ret < 0) {
		fprintf(stderr, "dpdk: failed to configure port\n");
		free(nd);
		return 0;
	}

	rte_eth_dev_info_get(nd->portid, &dev_info);

	ret = rte_eth_rx_queue_setup(nd->portid, 0, NUMDESC, 0,
				     &dev_info.default_rxconf, nd->rxpool);
	if (ret < 0) {
		fprintf(stderr, "dpdk: failed to setup rx queue\n");
		free(nd);
		return 0;
	}

	ret = rte_eth_tx_queue_setup(nd->portid, 0, NUMDESC, 0,
				     &dev_info.default_txconf);
	if (ret < 0) {
		fprintf(stderr, "dpdk: failed to setup tx queue\n");
		free(nd);
		return 0;
	}

	ret = rte_eth_dev_start(nd->portid);
	/* XXX: this function returns positive val (e.g., 12)
	 * if there's an error
	 */
	if (ret != 0) {
		fprintf(stderr, "dpdk: failed to start device\n");
		free(nd);
		return 0;
	}

	rte_eth_macaddr_get(nd->portid, &mac_addr);
	printf("Port %d: %02X:%02X:%02X:%02X:%02X:%02X\n", nd->portid,
	       mac_addr.addr_bytes[0], mac_addr.addr_bytes[1],
	       mac_addr.addr_bytes[2], mac_addr.addr_bytes[3],
	       mac_addr.addr_bytes[4], mac_addr.addr_bytes[5]);

	rte_eth_dev_set_link_up(nd->portid);

	rte_eth_link_get(nd->portid, &link);
	if (!link.link_status)
		fprintf(stderr, "dpdk: interface state is down\n");

	/* should be promisc ? */
	rte_eth_promiscuous_enable(nd->portid);

	return (struct lkl_netdev *) nd;
}

#else
#include <stdlib.h>

struct lkl_netdev *lkl_netdev_dpdk_create(const char *ifname)
{
	fprintf(stderr,
		"lkl: dpdk is not built. please build LKL to enable dpdk.\n");
	exit(0);
}
#endif /* LKL_DPDK */
