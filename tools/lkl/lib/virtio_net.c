#include <string.h>
#include <lkl_host.h>
#include "virtio.h"
#include "endian.h"

#include <lkl/linux/virtio_net.h>

#define netdev_of(x) (container_of(x, struct virtio_net_dev, dev))
#define BIT(x) (1ULL << x)

#define NUM_QUEUES (TX_QUEUE_IDX + 1)
#define QUEUE_DEPTH 128

/* In fact, we'll hit the limit on the devs string below long before
 * we hit this, but it's good enough for now. */
#define MAX_NET_DEVS 16

#ifdef DEBUG
#define bad_request(s) do {			\
		lkl_printf("%s\n", s);		\
		panic();			\
	} while (0)
#else
#define bad_request(s) lkl_printf("virtio_net: %s\n", s);
#endif /* DEBUG */

struct virtio_net_dev {
	struct virtio_dev dev;
	struct lkl_virtio_net_config config;
	struct lkl_dev_net_ops *ops;
	struct lkl_netdev *nd;
	struct lkl_mutex **queue_locks;
};

static int net_check_features(struct virtio_dev *dev)
{
	if (dev->driver_features == dev->device_features)
		return 0;

	return -LKL_EINVAL;
}

static void net_acquire_queue(struct virtio_dev *dev, int queue_idx)
{
	lkl_host_ops.mutex_lock(netdev_of(dev)->queue_locks[queue_idx]);
}

static void net_release_queue(struct virtio_dev *dev, int queue_idx)
{
	lkl_host_ops.mutex_unlock(netdev_of(dev)->queue_locks[queue_idx]);
}

/* The buffers passed through "req" from the virtio_net driver always
 * starts with a vnet_hdr. We need to check the backend device if it
 * expects vnet_hdr and adjust buffer offset accordingly.
 */
static int net_enqueue(struct virtio_dev *dev, struct virtio_req *req)
{
	struct lkl_virtio_net_hdr_v1 *header;
	struct virtio_net_dev *net_dev;
	int ret, len, i;
	struct lkl_dev_buf *iov;

	header = req->buf[0].addr;
	net_dev = netdev_of(dev);
	if (!net_dev->nd->has_vnet_hdr) {
		/* The backend device does not expect a vnet_hdr so adjust
		 * buf accordingly. (We make adjustment to req->buf so it
		 * can be used directly for the tx/rx call but remember to
		 * undo the change after the call.
		 * Note that it's ok to pass iov with entry's len==0.
		 * The caller will skip to the next entry correctly.
		 */
		req->buf[0].addr += sizeof(*header);
		req->buf[0].len -= sizeof(*header);
	}
	iov = req->buf;

	/* Pick which virtqueue to send the buffer(s) to */
	if (is_tx_queue(dev, req->q)) {
		ret = net_dev->ops->tx(net_dev->nd, iov, req->buf_count);
		if (ret < 0)
			return -1;
		i = 1;
	} else if (is_rx_queue(dev, req->q)) {
		ret = net_dev->ops->rx(net_dev->nd, iov, req->buf_count);
		if (ret < 0)
			return -1;
		if (net_dev->nd->has_vnet_hdr) {

			/* if the number of bytes returned exactly matches
			 * the total space in the iov then there is a good
			 * chance we did not supply a large enough buffer for
			 * the whole pkt, i.e., pkt has been truncated.
			 * This is only likely to happen under mergeable RX
			 * buffer mode.
			 */
			if (req->mergeable_rx_len == (unsigned int)ret)
				lkl_printf("PKT is likely truncated! len=%d\n",
				    ret);
		} else {
			header->flags = 0;
			header->gso_type = LKL_VIRTIO_NET_HDR_GSO_NONE;
		}
		/* Have to compute how many descriptors we've consumed (really
		 * only matters to the the mergeable RX mode) and return it
		 * through "num_buffers".
		 */
		for (i = 0, len = ret; len > 0; i++)
			len -= req->buf[i].len;
		req->buf_count = header->num_buffers = i;
		/* Need to set "buf_count" to how many we really used in
		 * order for virtio_req_complete() to work.
		 */
		if (dev->device_features & BIT(LKL_VIRTIO_NET_F_GUEST_CSUM))
			header->flags = LKL_VIRTIO_NET_HDR_F_DATA_VALID;
	} else {
		bad_request("tried to push on non-existent queue");
		return -1;
	}
	if (!net_dev->nd->has_vnet_hdr) {
		/* Undo the adjustment */
		req->buf[0].addr -= sizeof(*header);
		req->buf[0].len += sizeof(*header);
		ret += sizeof(struct lkl_virtio_net_hdr_v1);
	}
	virtio_req_complete(req, ret);
	return i;
}

static struct virtio_dev_ops net_ops = {
	.check_features = net_check_features,
	.enqueue = net_enqueue,
	.acquire_queue = net_acquire_queue,
	.release_queue = net_release_queue,
};

void poll_thread(void *arg)
{
	struct virtio_net_dev *dev = arg;
	int ret;

	/* Synchronization is handled in virtio_process_queue */
	while ((ret = dev->nd->ops->poll(dev->nd)) >= 0) {
		if (ret & LKL_DEV_NET_POLL_RX)
			virtio_process_queue(&dev->dev, 0);
		if (ret & LKL_DEV_NET_POLL_TX)
			virtio_process_queue(&dev->dev, 1);
	}
}

struct virtio_net_dev *registered_devs[MAX_NET_DEVS];
static int registered_dev_idx = 0;

static int dev_register(struct virtio_net_dev *dev)
{
	if (registered_dev_idx == MAX_NET_DEVS) {
		lkl_printf("Too many virtio_net devices!\n");
		/* This error code is a little bit of a lie */
		return -LKL_ENOMEM;
	} else {
		/* registered_dev_idx is incremented by the caller */
		registered_devs[registered_dev_idx] = dev;
		return 0;
	}
}

static void free_queue_locks(struct lkl_mutex **queues, int num_queues)
{
	int i = 0;
	if (!queues)
		return;

	for (i = 0; i < num_queues; i++)
		lkl_host_ops.mutex_free(queues[i]);

	lkl_host_ops.mem_free(queues);
}

static struct lkl_mutex **init_queue_locks(int num_queues)
{
	int i;
	struct lkl_mutex **ret = lkl_host_ops.mem_alloc(
		sizeof(struct lkl_mutex*) * num_queues);
	if (!ret)
		return NULL;

	for (i = 0; i < num_queues; i++) {
		ret[i] = lkl_host_ops.mutex_alloc();
		if (!ret[i]) {
			free_queue_locks(ret, i);
			return NULL;
		}
	}

	return ret;
}

int lkl_netdev_add(struct lkl_netdev *nd, struct lkl_netdev_args* args)
{
	struct virtio_net_dev *dev;
	int ret = -LKL_ENOMEM;

	dev = lkl_host_ops.mem_alloc(sizeof(*dev));
	if (!dev)
		return -LKL_ENOMEM;

	memset(dev, 0, sizeof(*dev));

	dev->dev.device_id = LKL_VIRTIO_ID_NET;
	if (args) {
		if (args->mac) {
			dev->dev.device_features |= BIT(LKL_VIRTIO_NET_F_MAC);
			memcpy(dev->config.mac, args->mac, LKL_ETH_ALEN);
		}
		dev->dev.device_features |= args->offload;

	}
	dev->dev.config_data = &dev->config;
	dev->dev.config_len = sizeof(dev->config);
	dev->dev.ops = &net_ops;
	dev->ops = nd->ops;
	dev->nd = nd;
	dev->queue_locks = init_queue_locks(NUM_QUEUES);

	if (!dev->queue_locks)
		goto out_free;

	/* MUST match the number of queue locks we initialized. We
	 * could init the queues in virtio_dev_setup to help enforce
	 * this, but netdevs are the only flavor that need these
	 * locks, so it's better to do it here. */
	ret = virtio_dev_setup(&dev->dev, NUM_QUEUES, QUEUE_DEPTH);

	if (ret)
		goto out_free;

	nd->poll_tid = lkl_host_ops.thread_create(poll_thread, dev);
	if (nd->poll_tid == 0)
		goto out_cleanup_dev;

	ret = dev_register(dev);
	if (ret < 0)
		goto out_cleanup_dev;

	return registered_dev_idx++;

out_cleanup_dev:
	virtio_dev_cleanup(&dev->dev);

out_free:
	if (dev->queue_locks)
		free_queue_locks(dev->queue_locks, NUM_QUEUES);
	lkl_host_ops.mem_free(dev);

	return ret;
}

/* Return 0 for success, -1 for failure. */
static int lkl_netdev_remove(struct virtio_net_dev *dev)
{
	if (!dev->nd->ops->close)
		/* Can't kill the poll threads, so we can't do
		 * anything safely. */
		return -1;

	if (dev->nd->ops->close(dev->nd) < 0)
		/* Something went wrong */
		return -1;

	virtio_dev_cleanup(&dev->dev);

	lkl_host_ops.mem_free(dev->nd);
	free_queue_locks(dev->queue_locks, NUM_QUEUES);
	lkl_host_ops.mem_free(dev);

	return 0;
}

int lkl_netdevs_remove(void)
{
	int i = 0, failure_count = 0;

	for (; i < registered_dev_idx; i++)
		failure_count -= lkl_netdev_remove(registered_devs[i]);

	if (failure_count) {
		lkl_printf("WARN: failed to free %d of %d netdevs.\n",
			failure_count, registered_dev_idx);
		return -1;
	}

	return 0;
}
