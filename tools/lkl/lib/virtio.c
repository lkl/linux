#include <string.h>
#include <stdio.h>
#include <lkl_host.h>
#include "iomem.h"
#include "virtio.h"
#include "endian.h"

#define VIRTIO_DEV_MAGIC		0x74726976
#define VIRTIO_DEV_VERSION		2

#define VIRTIO_MMIO_MAGIC_VALUE		0x000
#define VIRTIO_MMIO_VERSION		0x004
#define VIRTIO_MMIO_DEVICE_ID		0x008
#define VIRTIO_MMIO_VENDOR_ID		0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL		0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
#define VIRTIO_MMIO_QUEUE_NUM		0x038
#define VIRTIO_MMIO_QUEUE_READY		0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
#define VIRTIO_MMIO_STATUS		0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW	0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH	0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW	0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH	0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION	0x0fc
#define VIRTIO_MMIO_CONFIG		0x100
#define VIRTIO_MMIO_INT_VRING		0x01
#define VIRTIO_MMIO_INT_CONFIG		0x02

#define BIT(x) (1ULL << x)

#ifdef DEBUG
#define bad_driver(msg) do {					\
		lkl_printf("LKL virtio error: %s\n", msg);	\
		lkl_host_ops.panic();				\
	} while (0)
#else
#define bad_driver(msg) do { } while (0)
#endif /* DEBUG */

static inline uint16_t virtio_get_used_event(struct virtio_queue *q)
{
	return q->avail->ring[q->num];
}

static inline void virtio_set_avail_event(struct virtio_queue *q, uint16_t val)
{
	*((uint16_t *)&q->used->ring[q->num]) = val;
}

static inline void virtio_deliver_irq(struct virtio_dev *dev)
{
	dev->int_status |= VIRTIO_MMIO_INT_VRING;
	lkl_trigger_irq(dev->irq);
}

void virtio_req_complete(struct virtio_req *req, uint32_t len)
{
	struct virtio_queue *q = req->q;
	struct virtio_dev *dev = req->dev;
	uint16_t idx = le16toh(q->used->idx) & (q->num - 1);
	int send_irq = 0;

	q->used->ring[idx].id = htole16(req->idx);
	q->used->ring[idx].len = htole16(len);
	if (virtio_get_used_event(q) == q->used->idx)
		send_irq = 1;
	q->used->idx = htole16(le16toh(q->used->idx) + 1);

	if (send_irq)
		virtio_deliver_irq(dev);
}

/* Grab the vring_desc from the queue at the appropriate index in the
 * queue's circular buffer, converting from little-endian to
 * the host's endianness. */
static inline struct lkl_vring_desc *vring_desc_at_le_idx(struct virtio_queue *q,
							__lkl__virtio16 le_idx)
{
	return &q->desc[le16toh(le_idx) & (q->num -1)];
}

/* Initialize buf to hold the same info as the vring_desc */
static void init_dev_buf_from_vring_desc(struct lkl_dev_buf *buf,
					struct lkl_vring_desc *vring_desc)
{
	buf->addr = (void *)(uintptr_t)le64toh(vring_desc->addr);
	buf->len = le32toh(vring_desc->len);

	if (!(buf->addr && buf->len))
		bad_driver("bad vring_desc\n");
}

static int virtio_process_one(struct virtio_dev *dev, struct virtio_queue *q,
			      int idx)
{
	int q_buf_cnt = 0, ret = -1;
	struct virtio_req req = {
		.dev = dev,
		.q = q,
		.idx = q->avail->ring[idx & (q->num - 1)],
	};
	uint16_t prev_flags = LKL_VRING_DESC_F_NEXT;
	struct lkl_vring_desc *curr_vring_desc = vring_desc_at_le_idx(q, req.idx);

	while ((prev_flags & LKL_VRING_DESC_F_NEXT) &&
		(q_buf_cnt < VIRTIO_REQ_MAX_BUFS)) {
		prev_flags = le16toh(curr_vring_desc->flags);
		init_dev_buf_from_vring_desc(&req.buf[q_buf_cnt++], curr_vring_desc);
		curr_vring_desc = vring_desc_at_le_idx(q, curr_vring_desc->next);
	}

	/* Somehow, we've built a request that's too long to fit onto our device */
	if (q_buf_cnt == VIRTIO_REQ_MAX_BUFS &&
		(prev_flags & LKL_VRING_DESC_F_NEXT))
		bad_driver("enqueued too many request bufs");

	req.buf_count = q_buf_cnt;
	ret = dev->ops->enqueue(dev, &req);
	if (ret < 0)
		return ret;
	q->last_avail_idx++;
	return 0;
}

/* NB: we can enter this function two different ways in the case of
 * netdevs --- either through a tx/rx thread poll (which the LKL
 * scheduler knows nothing about) or through virtio_write called
 * inside an interrupt handler, so to be safe, it's not enough to
 * synchronize only the tx/rx polling threads.
 *
 * At the moment, it seems like only netdevs require the
 * synchronization we do here (i.e. locking around operations on a
 * particular virtqueue, with dev->ops->acquire_queue), since they
 * have these two different entry points, one of which isn't managed
 * by the LKL scheduler. So only devs corresponding to netdevs will
 * have non-NULL acquire/release_queue.
 *
 * In the future, this may change. If you see errors thrown in virtio
 * driver code by block/console devices, you should be suspicious of
 * the synchronization going on here.
 */
void virtio_process_queue(struct virtio_dev *dev, uint32_t qidx)
{
	struct virtio_queue *q = &dev->queue[qidx];

	if (!q->ready)
		return;

	if (dev->ops->acquire_queue)
		dev->ops->acquire_queue(dev, qidx);

	virtio_set_avail_event(q, q->avail->idx);

	while (q->last_avail_idx != le16toh(q->avail->idx)) {
		if (virtio_process_one(dev, q, q->last_avail_idx) < 0)
			break;
	}

	if (dev->ops->release_queue)
		dev->ops->release_queue(dev, qidx);
}

static inline uint32_t virtio_read_device_features(struct virtio_dev *dev)
{
	if (dev->device_features_sel)
		return (uint32_t)(dev->device_features >> 32);

	return (uint32_t)dev->device_features;
}

static inline void virtio_write_driver_features(struct virtio_dev *dev,
						uint32_t val)
{
	uint64_t tmp;

	if (dev->driver_features_sel) {
		tmp = dev->driver_features & 0xFFFFFFFF;
		dev->driver_features = tmp | (uint64_t)val << 32;
	} else {
		tmp = dev->driver_features & 0xFFFFFFFF00000000;
		dev->driver_features = tmp | val;
	}
}

static int virtio_read(void *data, int offset, void *res, int size)
{
	uint32_t val;
	struct virtio_dev *dev = (struct virtio_dev *)data;

	if (offset >= VIRTIO_MMIO_CONFIG) {
		offset -= VIRTIO_MMIO_CONFIG;
		if (offset + size > dev->config_len)
			return -LKL_EINVAL;
		memcpy(res, dev->config_data + offset, size);
		return 0;
	}

	if (size != sizeof(uint32_t))
		return -LKL_EINVAL;

	switch (offset) {
	case VIRTIO_MMIO_MAGIC_VALUE:
		val = VIRTIO_DEV_MAGIC;
		break;
	case VIRTIO_MMIO_VERSION:
		val = VIRTIO_DEV_VERSION;
		break;
	case VIRTIO_MMIO_DEVICE_ID:
		val = dev->device_id;
		break;
	case VIRTIO_MMIO_VENDOR_ID:
		val = dev->vendor_id;
		break;
	case VIRTIO_MMIO_DEVICE_FEATURES:
		val = virtio_read_device_features(dev);
		break;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		val = dev->queue[dev->queue_sel].num_max;
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		val = dev->queue[dev->queue_sel].ready;
		break;
	case VIRTIO_MMIO_INTERRUPT_STATUS:
		val = dev->int_status;
		break;
	case VIRTIO_MMIO_STATUS:
		val = dev->status;
		break;
	case VIRTIO_MMIO_CONFIG_GENERATION:
		val = dev->config_gen;
		break;
	default:
		return -1;
	}

	*(uint32_t *)res = htole32(val);

	return 0;
}

static inline void set_ptr_low(void **ptr, uint32_t val)
{
	uint64_t tmp = (uintptr_t)*ptr;

	tmp = (tmp & 0xFFFFFFFF00000000) | val;
	*ptr = (void *)(long)tmp;
}

static inline void set_ptr_high(void **ptr, uint32_t val)
{
	uint64_t tmp = (uintptr_t)*ptr;

	tmp = (tmp & 0x00000000FFFFFFFF) | ((uint64_t)val << 32);
	*ptr = (void *)(long)tmp;
}

static inline void set_status(struct virtio_dev *dev, uint32_t val)
{
	if ((val & LKL_VIRTIO_CONFIG_S_FEATURES_OK) &&
	    (!(dev->driver_features & BIT(LKL_VIRTIO_F_VERSION_1)) ||
	     !(dev->driver_features & BIT(LKL_VIRTIO_RING_F_EVENT_IDX)) ||
	     dev->ops->check_features(dev)))
		val &= ~LKL_VIRTIO_CONFIG_S_FEATURES_OK;
	dev->status = val;
}

static int virtio_write(void *data, int offset, void *res, int size)
{
	struct virtio_dev *dev = (struct virtio_dev *)data;
	struct virtio_queue *q = &dev->queue[dev->queue_sel];
	uint32_t val;
	int ret = 0;

	if (offset >= VIRTIO_MMIO_CONFIG) {
		offset -= VIRTIO_MMIO_CONFIG;

		if (offset + size >= dev->config_len)
			return -LKL_EINVAL;
		memcpy(dev->config_data + offset, res, size);
		return 0;
	}

	if (size != sizeof(uint32_t))
		return -LKL_EINVAL;

	val = le32toh(*(uint32_t *)res);

	switch (offset) {
	case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
		if (val > 1)
			return -LKL_EINVAL;
		dev->device_features_sel = val;
		break;
	case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
		if (val > 1)
			return -LKL_EINVAL;
		dev->driver_features_sel = val;
		break;
	case VIRTIO_MMIO_DRIVER_FEATURES:
		virtio_write_driver_features(dev, val);
		break;
	case VIRTIO_MMIO_QUEUE_SEL:
		dev->queue_sel = val;
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		dev->queue[dev->queue_sel].num = val;
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		dev->queue[dev->queue_sel].ready = val;
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		virtio_process_queue(dev, val);
		break;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		dev->int_status = 0;
		break;
	case VIRTIO_MMIO_STATUS:
		set_status(dev, val);
		break;
	case VIRTIO_MMIO_QUEUE_DESC_LOW:
		set_ptr_low((void **)&q->desc, val);
		break;
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:
		set_ptr_high((void **)&q->desc, val);
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
		set_ptr_low((void **)&q->avail, val);
		break;
	case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
		set_ptr_high((void **)&q->avail, val);
		break;
	case VIRTIO_MMIO_QUEUE_USED_LOW:
		set_ptr_low((void **)&q->used, val);
		break;
	case VIRTIO_MMIO_QUEUE_USED_HIGH:
		set_ptr_high((void **)&q->used, val);
		break;
	default:
		ret = -1;
	}

	return ret;
}

static const struct lkl_iomem_ops virtio_ops = {
	.read = virtio_read,
	.write = virtio_write,
};

char lkl_virtio_devs[256];
static char *devs = lkl_virtio_devs;

int virtio_dev_setup(struct virtio_dev *dev, int queues, int num_max)
{
	int qsize = queues * sizeof(*dev->queue);
	int avail, mmio_size;
	int i;

	dev->irq = lkl_get_free_irq("virtio");
	if (dev->irq < 0)
		return dev->irq;

	dev->device_features |= BIT(LKL_VIRTIO_F_VERSION_1) |
		BIT(LKL_VIRTIO_RING_F_EVENT_IDX);
	dev->queue = lkl_host_ops.mem_alloc(qsize);
	if (!dev->queue)
		return -LKL_ENOMEM;

	memset(dev->queue, 0, qsize);
	for (i = 0; i < queues; i++)
		dev->queue[i].num_max = num_max;

	mmio_size = VIRTIO_MMIO_CONFIG + dev->config_len;
	dev->base = register_iomem(dev, mmio_size, &virtio_ops);
	if (!dev->base) {
		lkl_host_ops.mem_free(dev->queue);
		return -LKL_ENOMEM;
	}

	avail = sizeof(lkl_virtio_devs) - (devs - lkl_virtio_devs);
	devs += snprintf(devs, avail, " virtio_mmio.device=%d@0x%lx:%d",
			 mmio_size, (uintptr_t)dev->base, dev->irq);

	return 0;
}

void virtio_dev_cleanup(struct virtio_dev *dev)
{
	lkl_put_irq(dev->irq, "virtio");
	unregister_iomem(dev->base);
	lkl_host_ops.mem_free(dev->queue);
}

