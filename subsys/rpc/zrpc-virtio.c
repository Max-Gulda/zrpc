/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup zrpc_backend
 * @brief zRPC hypervisorless virtio backend implementation.
 */

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/rpc/zrpc.h>
#include <zephyr/rpc/zrpc-tlv.h>
#include <zephyr/rpc/zrpc-channels.h>
#include <zephyr/sys/slist.h>

#include <openamp/open_amp.h>
#include <openamp/rpmsg_virtio.h>

/**
 * @brief zRPC hypervisorless virtio backend.
 * @defgroup zrpc_virtio zRPC virtio backend.
 * @ingroup zrpc_backend
 * @{
 *
 * The virtio zRPC backend leverages OpenAMP's RPmsg implementation to
 * allow RPCs between CPUs on systems using asymmetric multiprocessing.
 * In brief terms, RPCs are sent via single-producer, single-consumer ring
 * buffers located in shared memory.
 */


/** @cond ZEPHYR_INTERNALS */
LOG_MODULE_REGISTER(zrpc_virtio, CONFIG_ZRPC_VIRTIO_LOG_LEVEL);
/** @endcond */


/** compatible = zrpc,virtio-channel */
#define DT_DRV_COMPAT zrpc_virtio_channel

/** @cond ZEPHYR_INTERNALS */
#define VDEV_LOG(lvl, vdev, fmt, ...)					\
	do {								\
		_Generic((vdev),					\
			struct virtio_device *: (void)0,		\
			struct virtio_device const *: (void)0		\
		);							\
									\
		LOG_ ## lvl("(%s) " fmt,				\
			(vdev)->role == RPMSG_HOST ?			\
				"host" : "remote" 			\
			__VA_OPT__(,)					\
			__VA_ARGS__					\
		);							\
	} while (0);

#define VDEV_DBG(...) VDEV_LOG(DBG, __VA_ARGS__)
#define VDEV_INF(...) VDEV_LOG(INF, __VA_ARGS__)
#define VDEV_WRN(...) VDEV_LOG(WRN, __VA_ARGS__)
#define VDEV_ERR(...) VDEV_LOG(ERR, __VA_ARGS__)

#define VDEV_HEXDUMP(lvl, vdev, data, length, prefix)			\
	do {								\
		_Generic((vdev),					\
			struct virtio_device *: (void)0,		\
			struct virtio_device const *: (void)0		\
		);							\
									\
		LOG_HEXDUMP_ ## lvl(data, length,			\
			(vdev)->role == RPMSG_HOST ?			\
				"(host) " prefix : "(remote) " prefix	\
		);							\
	} while (0);

#define VDEV_HEXDUMP_DBG(...) VDEV_HEXDUMP(DBG, __VA_ARGS__)
#define VDEV_HEXDUMP_INF(...) VDEV_HEXDUMP(INF, __VA_ARGS__)
#define VDEV_HEXDUMP_WRN(...) VDEV_HEXDUMP(WRN, __VA_ARGS__)
#define VDEV_HEXDUMP_ERR(...) VDEV_HEXDUMP(ERR, __VA_ARGS__)

/*
 * Each zRPC virtio device owns exactly one RPMsg endpoint. Give both peers
 * the same fixed address so either side can send first without relying on a
 * one-shot name-service announcement and mailbox wakeup.
 */
#define ZRPC_VIRTIO_ENDPOINT_ADDR RPMSG_RESERVED_ADDRESSES

/** @endcond */

/** Control block */
struct zrpc_virtio_ctrl_blk {
	/** Status byte */
	unsigned char status;
};


/** RX wait node */
struct zrpc_virtio_wait_node {

	/** List head for iteration */
	sys_snode_t head;

	/** Sequence number of the expected reply */
	uint16_t seq;

	/** Point when the message expires */
	k_timepoint_t expiry;

	/** Condition variable to wait on */
	struct k_condvar cv;

	/** Message to pass to reader */
	struct zrpc_msghdr *msghdr;
};


/** Instance-specific data */
struct zrpc_virtio_data {

	/** Whether or not the endpoint is bound */
	bool ept_bound;

	/** Maximum size of RPCs send over this channel */
	uint32_t max_rpc_size;

	/** Work scheduled on IPM callbacks */
	struct k_work ipm_work;

	/** IPM work queue */
	struct k_work_q ipm_work_q;

	/** Shared memory physical address map */
	metal_phys_addr_t shm_physmap;

	/** Shared memory I/O region */
	struct metal_io_region shm_io;

	/** Virtio rings */
	struct virtio_vring_info vrings[2u];

	/** Virtio device */
	struct virtio_device vdev;

	/** Shared memory buffers pool */
	struct rpmsg_virtio_shm_pool shmpool;

	/** Rpmsg endpoint */
	struct rpmsg_endpoint ept;

	/** Rpmsg virtio device */
	struct rpmsg_virtio_device rvdev;

	/** Mutex protecting @c pending_replies */
	struct k_mutex pending_mutex;

	/** Mutex protecting @c rx_queue */
	struct k_mutex rx_mutex;

	/** List of RPCs awaiting replies */
	sys_slist_t pending_replies;

	/** Work executed on RPC reception */
	struct k_work rx_work;

	/** RX work queue */
	struct k_work_q rx_work_q;

	/** Queue for received messages */
	struct k_msgq *rx_queue;

	/** Queue for replies extracted from @c rx_queue */
	struct k_msgq *reply_queue;

	/** <tt>struct zrpc_virtio_wait_node</tt> pool */
	struct k_mem_slab *wait_slab;

	/** Optional pool for private copies of incoming messages */
	struct k_mem_slab *rx_copy_slab;

	/** Address of stack used by IPM thread */
	k_thread_stack_t *ipm_stack;

	/** Address of stack used by RX thread */
	k_thread_stack_t *rx_stack;

	/** Virtio queues */
	struct virtqueue *vqueues[2u];

	/** Owning device */
	struct device const *dev;
};


/** Instance-specific config */
struct zrpc_virtio_config {

	/** Whether or not the endpoint should run in host mode */
	bool host;

	/** Whether or not explicit dcache flushes are required */
	bool have_dcache;

	/** Base address of the shared memory section */
	uint32_t shm_addr;

	/** Size of the shared memory section */
	uint32_t shm_size;

	/** Size of the shared memory control block */
	uint32_t ctrl_blk_size;

	/** Number of trailing vring extra descriptors */
	uint32_t num_vq_desc_extra;

	/** ID of the zRPC channel */
	uint32_t channel_id;

	/** Maximum size of each chunk transmitted in the rings */
	uint32_t tx_chunk_size;

	/** Number of milliseconds before a received reply is discarded */
	uint32_t reply_lifetime;

	/** Number of milliseconds t wait for an RPC with the expected seq */
	uint32_t rx_timeout;

	/** Size of the @c ipm_stack in the corresponding data struct */
	size_t ipm_stack_size;

	/** Size of the @c rx_stack in the corresponding data struct */
	size_t rx_stack_size;

	/** Name of the IPM work thread */
	char const *ipm_thread_name;

	/** Name of the RX work thread */
	char const *rx_thread_name;

	/** Inter-process mailbox device */
	struct device const *ipm_dev;
};


/**
 * @brief Get address of the control block associated with this device.
 *
 * @note Entries in this structure should be accessed only using one of the
 *       @c sys_readN or @c sys_writeN functions.
 *
 * @param dev The associated device.
 *
 * @return Address of the control block associated with @c dev.
 */
static inline struct zrpc_virtio_ctrl_blk *zrpc_virtio_ctrl_blk(
						struct device const *dev)
{
	struct zrpc_virtio_config const *cfg = dev->config;

	return (void *)cfg->shm_addr;
}


/**
 * @brief Get vqueue identifier for @c dev.
 *
 * @param dev The device instance.
 *
 * @retval 0 This virtqueue has id 0.
 * @retval 1 This virtqueue has id 1.
 */
static inline uint_fast8_t zrpc_virtio_vqueue_id(struct device const *dev)
{
	struct zrpc_virtio_config const *cfg = dev->config;
	return !cfg->host;
}


/**
 * @brief Get virtqueue associated with @c dev.
 *
 * @param dev The device instance.
 *
 * @return Address of the virtqueue associated with @c dev.
 */
static inline struct virtqueue *zrpc_virtio_this_vqueue(
			struct device const *dev)
{
	struct zrpc_virtio_data *data = dev->data;

	return data->vqueues[zrpc_virtio_vqueue_id(dev)];

}

/**
 * @brief Get virtio device status.
 *
 * @param vdev The <tt>struct virtio_device</tt> instance in the
 *	       <tt>struct zrpc_virtio_data</tt> of the device.
 *
 * @return Status of the device.
 */
static unsigned char zrpc_virtio_get_status(struct virtio_device *vdev)
{
	struct device const *dev;
	struct zrpc_virtio_data *data =
		CONTAINER_OF(vdev, struct zrpc_virtio_data, vdev);
	struct zrpc_virtio_config const *cfg;
	struct zrpc_virtio_ctrl_blk *ctrl_blk;

	dev = data->dev;
	cfg = dev->config;
	if (cfg->host)
		return VIRTIO_CONFIG_STATUS_DRIVER_OK;

	ctrl_blk = zrpc_virtio_ctrl_blk(dev);

	if (cfg->have_dcache)
		sys_cache_data_invd_range(&ctrl_blk->status,
					sizeof(ctrl_blk->status));

	return sys_read8((mem_addr_t)&ctrl_blk->status);
}


/**
 * @brief Set status of device.
 *
 * @param ctrl_blk_addr Address of the control block.
 * @param status        The statust to set.
 * @param have_dcache  Whether or not dcache must be flushed
 */
static inline void zrpc_virtio_set_status_raw(mem_addr_t ctrl_blk_addr,
			unsigned char status, bool have_dcache)
{
	struct zrpc_virtio_ctrl_blk *ctrl_blk = (void *)ctrl_blk_addr;
	sys_write8(status, (mem_addr_t)&ctrl_blk->status);

	if (have_dcache)
		sys_cache_data_flush_range(&ctrl_blk->status,
					sizeof(ctrl_blk->status));
}


/**
 * @brief Set status of the virtio device.
 *
 * @param vdev   The <tt>struct virtio_device</tt> instance in the
 *	         <tt>struct zrpc_virtio_data</tt> of the device.
 * @param status The status to set.
 */
static void zrpc_virtio_set_status(struct virtio_device *vdev,
		unsigned char status)
{
	struct device const *dev;
	struct zrpc_virtio_config const *cfg;
	struct zrpc_virtio_data *data =
		CONTAINER_OF(vdev, struct zrpc_virtio_data, vdev);
	struct zrpc_virtio_ctrl_blk *ctrl_blk;

	dev = data->dev;
	cfg = dev->config;

	ctrl_blk = zrpc_virtio_ctrl_blk(dev);
	zrpc_virtio_set_status_raw((unsigned long)ctrl_blk, status, cfg->have_dcache);
}


/**
 * @brief Get endpoint features.
 *
 * @param vdev, The virtio device associated with the driver instance.
 *
 * @return A bitmask of supported features.
 */
static uint32_t zrpc_virtio_get_features(struct virtio_device *vdev)
{
	return BIT(VIRTIO_RPMSG_F_NS);
}


/**
 * @brief Notify endpoint of incoming data.
 *
 * @param vqueue The virtqueue on which data is available.
 */
static void zrpc_virtio_notify(struct virtqueue *vqueue)
{
	int ret;
	struct device const *dev;
	struct zrpc_virtio_data *data =
		CONTAINER_OF(vqueue->vq_dev, struct zrpc_virtio_data, vdev);
	struct zrpc_virtio_config const *cfg;

	dev = data->dev;
	cfg = dev->config;

#if defined CONFIG_SOC_AN521 || defined CONFIG_SOC_MUSCA_B1
	uint32_t core = sse_200_platform_get_cpu_id();

	ret = ipm_send(cfg->ipm_dev, 0, !core, 0, 1);
#elif defined CONFIG_IPM_STM32_HSEM
	/* Payload not supported */
	ret = ipm_send(cfg->ipm_dev, 0, 0, NULL, 0);
#else
	ret = ipm_send(cfg->ipm_dev, 0, 0, &(uint32_t){ 0 }, sizeof(uint32_t));
#endif

	if (ret)
		VDEV_ERR(&data->vdev, "Error on ipm_send: %d", -ret);
}


/** Virtio dispatchers */
struct virtio_dispatch const zrpc_virtio_dispatch = {
	.get_status = zrpc_virtio_get_status,
	.set_status = zrpc_virtio_set_status,
	.get_features = zrpc_virtio_get_features,
	.notify = zrpc_virtio_notify,
};


/**
 * @brief Attempt to find a node with matching sequence number in pending list.
 *
 * @pre The @c pending_mutex must be held.
 *
 * If the node is found, it is removed from the pending list.
 *
 * @param dev The device instance.
 * @param seq The sequence number to search for.
 *
 * @retval >0   Address of the entry with matching sequence number.
 * @retval NULL No entry with matchin sequence number found.
 */
static struct zrpc_virtio_wait_node *zrpc_virtio_find_and_extract_reply(
		struct device const *dev, uint16_t seq)
{
	sys_snode_t *prev;
	sys_slist_t *pending;
	struct zrpc_virtio_wait_node *node, *next;
	struct zrpc_virtio_data *data = dev->data;

	prev = NULL;
	pending = &data->pending_replies;
	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(pending, node, next, head) {
		if (node->seq == seq) {
			sys_slist_remove(pending, prev, &node->head);
			return node;
		}

		prev = &node->head;
	}

	return NULL;
}


/**
 * @brief Await reply with provided @p seq.
 *
 * @pre The @c pending_mutex must be held.
 *
 * @param dev The device instance.
 * @param seq The sequence number of the reply.
 *
 * @retval 0      Reply received, msghdr available in @p node.
 * @retval -errno An error occurred.
 */
static int zrpc_virtio_await_reply(struct device const *dev, uint16_t seq,
		struct zrpc_virtio_wait_node *node)
{
	int ret;
	struct zrpc_virtio_data *data = dev->data;
	struct zrpc_virtio_config const *cfg = dev->config;

	node->seq = seq;
	node->msghdr = NULL;
	node->expiry = sys_timepoint_calc(K_FOREVER);

	ret = k_condvar_init(&node->cv);
	if (ret)
		return ret;

	sys_slist_append(&data->pending_replies, &node->head);
	do {
		ret = k_condvar_wait(&node->cv, &data->pending_mutex,
			K_MSEC(cfg->rx_timeout));
	}  while (!ret && !node->msghdr);
	sys_slist_find_and_remove(&data->pending_replies, &node->head);

	return ret;
}


/**
 * @brief Send RPC message to peer.
 *
 * @param dev    The device instance.
 * @param msghdr The encoded RPC to send.
 *
 * @retval 0        Message successfully sent.
 * @retval -EIO     Message couldn not be sent.
 * @retval -ENOBUFS Only part of the message could be sent.
 */
static int zrpc_virtio_send(struct device const *dev,
		struct zrpc_msghdr const *msghdr)
{
	int ret;
	size_t len;
	struct zrpc_virtio_data *data = dev->data;

	if (unlikely(!data->ept_bound)) {
		VDEV_ERR(&data->vdev, "Endpoint not yet bound");
		return -EAGAIN;
	}

	if (IS_ENABLED(CONFIG_ZRPC_PEDANTIC)) {
		if (sizeof(*msghdr) + msghdr->len > (size_t)INT_MAX)
			return -EOVERFLOW;
	}

	len = sizeof(*msghdr) + msghdr->len;

	if (unlikely(data->max_rpc_size && len > data->max_rpc_size))
		return -ENOBUFS;

	VDEV_HEXDUMP_DBG(&data->vdev, msghdr, len, "TX: ");
	ret = rpmsg_send(&data->ept, msghdr, len);
	if (ret < 0)
		return -EIO;

	if ((size_t)ret < msghdr->len)
		return -ENOBUFS;

	return 0;
}


/**
 * @brief Release a queued receive message.
 *
 * Messages either remain in their held RPMsg buffer or are copied to an
 * instance-local slab in the endpoint callback.
 *
 * @param data   Instance owning @p msghdr.
 * @param msghdr Message to release.
 */
static void zrpc_virtio_release_rx_message(struct zrpc_virtio_data *data,
		struct zrpc_msghdr *msghdr)
{
	if (data->rx_copy_slab)
		k_mem_slab_free(data->rx_copy_slab, msghdr);
	else
		rpmsg_release_rx_buffer(&data->ept, msghdr);
}


/**
 * @brief Receive reply with sequence number @p seq.
 *
 * @param dev      The device instance.
 * @param seq      The sequence number of the reply.
 * @param msghdr   Buffer to copy the received reply to.
 * @param msg_size Size of the buffer at @p msghdr.
 *
 * @retval 0        Reply received and availalbe in @p msghdr.
 * @retval -ENOBUFS Insufficient space at @p msghdr.
 * @retval -errno   An error occurred.
 */
static int zrpc_virtio_recv(struct device const *dev, uint16_t seq,
		struct zrpc_msghdr *msghdr, size_t msg_size)
{
	int ret;
	void *mem;
	struct zrpc_msghdr *mhdr;
	struct zrpc_virtio_wait_node *node;
	struct zrpc_virtio_data *data = dev->data;

	if (unlikely(!data->ept_bound)) {
		VDEV_ERR(&data->vdev, "Endpoint not yet bound");
		return -EAGAIN;
	}

	ret = k_mutex_lock(&data->pending_mutex, K_MSEC(3000));
	if (ret)
		return ret;

	node = zrpc_virtio_find_and_extract_reply(dev, seq);
	if (!node) {
		ret = k_mem_slab_alloc(data->wait_slab, &mem, K_MSEC(25));
		node = mem;
		if (likely(!ret))
			ret = zrpc_virtio_await_reply(dev, seq, node);
		else
			node = NULL;
	}

	if (node) {
		mhdr = node->msghdr;
		if (unlikely(!ret && msg_size < mhdr->len + sizeof(*mhdr)))
			ret = -ENOBUFS;
		if (!ret)
			memcpy(msghdr, node->msghdr,
				sizeof(*msghdr) + node->msghdr->len);
		if (node->msghdr)
			zrpc_virtio_release_rx_message(data, node->msghdr);
		k_mem_slab_free(data->wait_slab, node);
	}
	k_mutex_unlock(&data->pending_mutex);

	if (!ret)
		VDEV_HEXDUMP_DBG(&data->vdev, msghdr,
			sizeof(*msghdr) + msghdr->len, "Extracted:");

	return ret;

}


struct zrpc_driver_api const zrpc_virtio_api = {
	.send = zrpc_virtio_send,
	.recv = zrpc_virtio_recv,
};


/**
 * @brief Callback invoked when an rpmsg endpoint is unbound.
 *
 * @param ept The just-unbound endpoint.
 */
static void zrpc_virtio_rp_unbind_cb(struct rpmsg_endpoint *ept)
{
	rpmsg_destroy_ept(ept);
}


/**
 * @brief Callback invoked whenever data is received on the endpoint.
 *
 * @param ept    The endpoint on which the data is received.
 * @param rpdata Pointer to the data received.
 * @param len    Length of the data received.
 * @param src    Sender id.
 * @param priv   Private data
 *
 * @retval RPMSG_SUCCESS Regardless of success or failure (required by OpenAMP).
 */
static int zrpc_virtio_rp_ept_cb(struct rpmsg_endpoint *ept, void *rpdata,
		size_t len, uint32_t src, void *priv)
{
	int ret;
	void *copy;
	struct zrpc_msghdr *msghdr = rpdata;
	struct zrpc_virtio_data *data =
		CONTAINER_OF(ept, struct zrpc_virtio_data, ept);
	struct zrpc_virtio_config const *cfg = data->dev->config;

	VDEV_DBG(&data->vdev, "Received blob of size %zu", len);
	if (unlikely(len < sizeof(*msghdr))) {
		VDEV_WRN(&data->vdev, "Discarding message of size %zu", len);
		return RPMSG_SUCCESS;
	}

	VDEV_HEXDUMP_DBG(&data->vdev, msghdr, len, "RX: ");

	if (unlikely(msghdr->len + sizeof(*msghdr) != len)) {
		VDEV_WRN(&data->vdev, "Discarding malformed message, "
			"header indicates length %zu, got %u",
			(size_t)(msghdr->len + sizeof(*msghdr)),
			(unsigned int)len);
		/* Rpmsg API requries that RPMSG_SUCCESS is always returned */
		return RPMSG_SUCCESS;
	}

	VDEV_DBG(&data->vdev,
		"Queueing up message with id 0x%04" PRIx16 ", seq 0x%04" PRIx16,
		msghdr->id, msghdr->seq);
	if (data->rx_copy_slab) {
		if (unlikely(len > cfg->tx_chunk_size)) {
			VDEV_WRN(&data->vdev,
				"Discarding message of size %zu; local RX blocks are %u bytes",
				len, cfg->tx_chunk_size);
			return RPMSG_SUCCESS;
		}

		ret = k_mem_slab_alloc(data->rx_copy_slab, &copy, K_NO_WAIT);
		if (ret) {
			VDEV_WRN(&data->vdev,
				"Local RX copy pool exhausted; discarding message");
			return RPMSG_SUCCESS;
		}
		memcpy(copy, msghdr, len);
		msghdr = copy;
	}
	else {
		rpmsg_hold_rx_buffer(ept, msghdr);
	}

	ret = k_mutex_lock(&data->rx_mutex, K_NO_WAIT);
	if (!ret) {
		ret = k_msgq_put(data->rx_queue, &msghdr, K_NO_WAIT);
		k_mutex_unlock(&data->rx_mutex);
	}
	if (!ret) {
		/* Coalesced submissions here may leave audio queued and cause stalls. */
		ret = k_work_submit_to_queue(&data->rx_work_q, &data->rx_work);
	}
	else
		zrpc_virtio_release_rx_message(data, msghdr);
	if (ret < 0)
		VDEV_ERR(&data->vdev,
			"Could not queue up processing of incoming RPC: %d",
			-ret);

	return RPMSG_SUCCESS;
}


/**
 * @brief Process RPC reply.
 *
 * Iterate through the list of pending replies in search of a reply with a
 * sequence number matching that in @p msghdr. If one is found, signal the
 * waiting receiver via its condition variable. If no waiter is found,
 * add the entry to the reply list for the waiter to pick up later.
 *
 * @param dev    The device instance.
 * @param msghdr The received RPC message.
 *
 * @retval 0      Reply successfully inserted in the pending list.
 * @retval -errno An error occurred.
 */
static int zrpc_virtio_process_reply(struct device const *dev,
		struct zrpc_msghdr *msghdr)
{
	int ret;
	void *mem;
	bool signaled;
	sys_snode_t *prev;
	sys_slist_t *pending;
	struct zrpc_virtio_wait_node *node, *next;
	struct zrpc_virtio_data *data = dev->data;
	struct zrpc_virtio_config const *cfg = dev->config;

	ret = k_mutex_lock(&data->pending_mutex, K_MSEC(200));
	if (ret) {
		VDEV_ERR(&data->vdev, "Could not lock pending mutex: %d", -ret);
		return ret;
	}

	prev = NULL;
	signaled = false;
	pending = &data->pending_replies;
	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(pending, node, next, head) {
		if (node->seq == msghdr->seq) {
			node->msghdr = msghdr;
			k_condvar_signal(&node->cv);
			signaled = true;
			break;
		}

		VDEV_DBG(&data->vdev, "Seq 0x%" PRIx16 " remains in queue",
								node->seq);

		if (sys_timepoint_expired(node->expiry)) {
			VDEV_INF(&data->vdev,
				"RPC with seq 0x%" PRIx16 " expired",
				node->seq);

			sys_slist_remove(pending, prev, &node->head);
			if (node->msghdr)
				zrpc_virtio_release_rx_message(
					data, node->msghdr);
			k_mem_slab_free(data->wait_slab, node);
		}
		else
			prev = &node->head;
	}

	if (!signaled) {
		ret = k_mem_slab_alloc(data->wait_slab, &mem, K_MSEC(1000));
		node = mem;
		if (!ret) {
			node->seq = msghdr->seq;
			node->msghdr = msghdr;
			node->expiry = sys_timepoint_calc(
				K_MSEC(cfg->reply_lifetime)
			);
			sys_slist_append(&data->pending_replies, &node->head);
		}
	}

	k_mutex_unlock(&data->pending_mutex);
	return ret;
}


/**
 * @brief Work scheduled on endpoint callback.
 *
 * @param work The work struct used to schedule the function.
 */
static void zrpc_virtio_rp_ept_work(struct k_work *work)
{
	int ret;
	bool is_reply;
	struct device const *dev;
	struct zrpc_msghdr *msghdr;
	struct zrpc_virtio_data *data =
		CONTAINER_OF(work, struct zrpc_virtio_data, rx_work);
	struct zrpc_virtio_config const *cfg;

	dev = data->dev;
	cfg = dev->config;

	do {
		ret = k_mutex_lock(&data->rx_mutex, K_MSEC(50));
		if (ret) {
			VDEV_ERR(&data->vdev, "Error locking RX mutex: %d",
				-ret);
			return;
		}
		ret = k_msgq_get(data->rx_queue, &msghdr, K_NO_WAIT);
		k_mutex_unlock(&data->rx_mutex);
		if (ret) {
			if (ret != -ENOMSG) {
				VDEV_ERR(&data->vdev,
					"Error extracting from RX queue: %d",
					-ret);
			}
			return;
		}

		is_reply = msghdr->flags & ZRPC_FLAG_REPLY;
		if (is_reply)
			ret = zrpc_virtio_process_reply(dev, msghdr);
		else {
			ret = zrpc_rx_dispatch(cfg->channel_id, msghdr);
			zrpc_virtio_release_rx_message(data, msghdr);
		}
		if (ret && is_reply)
			zrpc_virtio_release_rx_message(data, msghdr);
		if (ret)
			VDEV_ERR(&data->vdev, "Error processing %s: %d",
				is_reply ? "reply" : "incoming RPC", -ret);
	} while (true);
}


/**
 * @brief Notify virtqueue of IPM receiver.
 *
 * Scheduled from the IPM callback.
 *
 * @param work The work which caused the function to be invoked.
 */
static void zrpc_virtio_ipm_work(struct k_work *work)
{
	struct zrpc_virtio_data *data =
		CONTAINER_OF(work, struct zrpc_virtio_data, ipm_work);

	virtqueue_notification(zrpc_virtio_this_vqueue(data->dev));
}


/**
 * @brief Callback invoked by IPM subsystem.
 *
 * @param ipm_dev   The IPM device.
 * @param user_data Address of the associated virtio device.
 * @param id        Message type identifier.
 * @param ipmdata   Message data pointer.
 */
static void zrpc_virtio_ipm_cb(struct device const *ipm_dev, void *user_data,
		uint32_t id, void volatile *ipmdata)
{
	int ret;
	struct zrpc_virtio_data *data = user_data;

	ret = k_work_submit_to_queue(&data->ipm_work_q, &data->ipm_work);
	if (ret < 0)
		VDEV_ERR(&data->vdev, "Could not submit IPM work: %d", -ret);
}


/**
 * @brief Get address of the shared I/O region.
 *
 * @param dev The device instance.
 *
 * @return Address of the shared I/O region associated with @c dev.
 */
static inline unsigned long zrpc_virtio_io_addr(struct device const *dev)
{
	struct zrpc_virtio_config const *cfg = dev->config;

	return cfg->shm_addr + cfg->ctrl_blk_size;
}


/**
 * @brief Get size of the shared I/O region.
 *
 * @param dev The device instance.
 *
 * @return Size of the shared I/O region associated with @c dev.
 */
static inline uint32_t zrpc_virtio_io_size(struct device const *dev)
{
	struct zrpc_virtio_config const *cfg = dev->config;
	return cfg->shm_size - cfg->ctrl_blk_size;
}


/**
 * @brief Initialize shared memory region.
 *
 * @param dev The device instance.
 *
 * @retval 0       Region successfully initialized.
 * @retval -EFAULT Shared memory region could not be created/accessed.
 */
static int zrpc_virtio_init_metal(struct device const *dev)
{
	int ret;
	struct zrpc_virtio_data *data = dev->data;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;

	ret = metal_init(&metal_params);
	if (ret)
		return -EFAULT;

	data->shm_physmap = zrpc_virtio_io_addr(dev);
	metal_io_init(&data->shm_io, (void *)data->shm_physmap,
		&data->shm_physmap, zrpc_virtio_io_size(dev), -1, 0, NULL);

	return 0;
}


/**
 * @brief Allocate virtqueues for @c dev.
 *
 * @param dev The device instance.
 *
 * @retval 0       Queues successfully allocated.
 * @retval -ENOMEM Allocation failure.
 */
static int zrpc_virtio_alloc_vqueues(struct device const *dev)
{
	int ret;
	unsigned int i;
	struct zrpc_virtio_data *data = dev->data;
	struct zrpc_virtio_config const *cfg = dev->config;

	ret = 0;
	for (i = 0u; !ret && i < ARRAY_SIZE(data->vqueues); ++i) {

		data->vqueues[i] = virtqueue_allocate(cfg->num_vq_desc_extra);
		if (!data->vqueues[i])
			ret = -ENOMEM;
	}

	for (unsigned int j = 0u; ret && j < i; ++j)
		virtqueue_free(data->vqueues[j]);

	return ret;
}


/**
 * @brief Initialize virtio rings.
 *
 * @param dev The device instance.
 */
static void zrpc_virtio_init_vrings(struct device const *dev)
{
	struct zrpc_virtio_data *data = dev->data;
	struct zrpc_virtio_config const *cfg = dev->config;

	for (unsigned int i = 0u; i < ARRAY_SIZE(data->vrings); ++i) {
		data->vrings[i].io = &data->shm_io;
		data->vrings[i].info.vaddr =
			(void *)(cfg->shm_addr + zrpc_virtio_io_size(dev) -
					!i * cfg->ctrl_blk_size);
		data->vrings[i].info.num_descs = cfg->num_vq_desc_extra;
		data->vrings[i].info.align = zrpc_alignto;
		data->vrings[i].vq = data->vqueues[i];
	}
}


/**
 * @brief Initialize virtio device
 *
 * @param dev The device instance.
 *
 * @retval 0       Rings successfully initialized.
 * @retval -ENOMEM Virtqueue allocation failure.
 */
static int zrpc_virtio_init_vdev(struct device const *dev)
{
	int ret;
	struct virtio_device *vdev;
	struct zrpc_virtio_data *data = dev->data;
	unsigned int const roles[] = {
		[0] = RPMSG_HOST,
		[1] = RPMSG_REMOTE,
	};


	vdev = &data->vdev;

	ret = zrpc_virtio_alloc_vqueues(dev);
	if (ret)
		return ret;

	zrpc_virtio_init_vrings(dev);

	vdev->role = roles[zrpc_virtio_vqueue_id(dev)];
	vdev->vrings_num = ARRAY_SIZE(data->vrings);
	vdev->vrings_info = data->vrings;
	vdev->func = &zrpc_virtio_dispatch;

	return 0;
}


/**
 * @brief Initialize device's inter-process mailbox.
 *
 * @param dev The device instance.
 *
 * @retval 0       IPM successfully initialized.
 * @retval -ENODEV IPM device not ready.
 * @retval -errno  Some other error occurred.
 */
static int zrpc_virtio_init_ipm(struct device const *dev)
{
	struct k_work_q *work_q;
	struct zrpc_virtio_data *data = dev->data;
	struct zrpc_virtio_config const *cfg = dev->config;

	work_q = &data->ipm_work_q;

	if (!device_is_ready(cfg->ipm_dev))
		return -ENODEV;

	k_work_queue_start(work_q, data->ipm_stack, cfg->ipm_stack_size,
		K_HIGHEST_THREAD_PRIO, NULL);
	k_thread_name_set(work_q->thread_id, cfg->ipm_thread_name);

	k_work_init(&data->ipm_work, zrpc_virtio_ipm_work);

	ipm_register_callback(cfg->ipm_dev, zrpc_virtio_ipm_cb, data);
	return ipm_set_enabled(cfg->ipm_dev, 1);
}


/**
 * @brief Initialize rpmsg shared memory.
 *
 * Set up shared memory buffers and initialize the vdev.
 *
 * @param dev The device instance.
 *
 * @retval 0      Device syccessfully initialized.
 * @retval -errno An error occurred.
 */
static int zrpc_virtio_init_shm(struct device const *dev)
{
	int ret;
	size_t shmpool_size;
	struct rpmsg_device *rdev;
	struct zrpc_virtio_data *data = dev->data;
	struct rpmsg_virtio_shm_pool *shmpool = NULL;
	struct zrpc_virtio_config const *cfg = dev->config;
	struct rpmsg_virtio_config const rpmsg_cfg = {
		.h2r_buf_size = cfg->tx_chunk_size,
		.r2h_buf_size = cfg->tx_chunk_size,
		.split_shpool = false,
	};

	if (cfg->host) {
		/*
		 * The I/O mapping covers the payload buffers and both vrings.
		 * Keep the trailing, ctrl_blk_size-sized vring areas out of the
		 * allocator or payload buffers will overwrite ring metadata.
		 */
		shmpool_size = zrpc_virtio_io_size(dev) -
			ARRAY_SIZE(data->vrings) * cfg->ctrl_blk_size;
		rpmsg_virtio_init_shm_pool(&data->shmpool,
			(void *)zrpc_virtio_io_addr(dev),
			shmpool_size);
		shmpool = &data->shmpool;
	}

	ret = rpmsg_init_vdev_with_config(&data->rvdev, &data->vdev, NULL,
			&data->shm_io, shmpool, &rpmsg_cfg);
	if (ret)
		return -ENODEV;

	rdev = rpmsg_virtio_get_rpmsg_device(&data->rvdev);
	if (unlikely(!rdev))
		return -ENODEV;
	ret = rpmsg_create_ept(&data->ept, rdev, dev->name,
		ZRPC_VIRTIO_ENDPOINT_ADDR, ZRPC_VIRTIO_ENDPOINT_ADDR,
		zrpc_virtio_rp_ept_cb, zrpc_virtio_rp_unbind_cb);
	if (!ret)
		data->ept_bound = true;
	else
		ret = -EFAULT;
	if (!ret) {
		ret = rpmsg_virtio_get_tx_buffer_size(rdev);
		if (ret > 0)
			data->max_rpc_size = (uint32_t)ret;
		else {
			VDEV_WRN(&data->vdev,
				"Could not determine max RPC size");
			data->max_rpc_size = 0u;
		}
		ret = 0;
	}

	return ret;
}


/**
 * @brief Initialize the virtio backend device.
 *
 * @param dev The device to initialize.
 *
 * @retval 0      Device successfully initialized.
 * @retval -errno An error occurred.
 */
static int zrpc_virtio_init(struct device const *dev)
{
	int ret;
	struct k_work_q *rx_work_q;
	struct zrpc_virtio_data *data = dev->data;
	struct zrpc_virtio_config const *cfg = dev->config;

	data->dev = dev;

	rx_work_q = &data->rx_work_q;
	k_work_queue_start(rx_work_q, data->rx_stack, cfg->rx_stack_size,
		K_HIGHEST_THREAD_PRIO, NULL);
	k_thread_name_set(rx_work_q->thread_id, cfg->rx_thread_name);
	k_work_init(&data->rx_work, zrpc_virtio_rp_ept_work);

	sys_slist_init(&data->pending_replies);
	k_mutex_init(&data->pending_mutex);
	k_mutex_init(&data->rx_mutex);

	ret = zrpc_virtio_init_metal(dev);
	if (!ret)
		ret = zrpc_virtio_init_vdev(dev);
	if (!ret)
		ret = zrpc_virtio_init_ipm(dev);
	if (!ret)
		ret = zrpc_virtio_init_shm(dev);
	return ret;
}


/**
 * @brief Generate virtio channel instance.
 *
 * @param n The device identifier.
 */
#define ZRPC_VIRTIO_INIT(n)						\
	K_THREAD_STACK_DEFINE(						\
		zrpc_virtio_ipm_stack_ ## n,				\
		DT_INST_PROP(n, zrpc_virtio_ipm_stack_size)		\
	);								\
									\
	K_THREAD_STACK_DEFINE(						\
		zrpc_virtio_rx_stack_ ## n,				\
		DT_INST_PROP(n, zrpc_virtio_rx_stack_size)		\
	);								\
									\
	K_MSGQ_DEFINE(							\
		zrpc_virtio_rx_queue_ ## n,				\
		sizeof(struct zrpc_msghdr *),				\
		DT_INST_PROP(n, zrpc_virtio_rx_queue_size),		\
		alignof(struct zrpc_msghdr *)				\
	);								\
									\
	K_MSGQ_DEFINE(							\
		zrpc_virtio_reply_queue_ ## n,				\
		sizeof(struct zrpc_msghdr *),				\
		DT_INST_PROP(n, zrpc_virtio_reply_queue_size),		\
		alignof(struct zrpc_msghdr *)				\
	);								\
									\
	K_MEM_SLAB_DEFINE(						\
		zrpc_virtio_wait_slab_ ## n,				\
		sizeof(struct zrpc_virtio_wait_node),			\
		DT_INST_PROP(n, zrpc_virtio_max_concurrent_replies),	\
		alignof(struct zrpc_virtio_wait_node)			\
	);								\
									\
	COND_CODE_1(							\
		DT_INST_PROP(n, zrpc_virtio_copy_rx),			\
		(K_MEM_SLAB_DEFINE(					\
			zrpc_virtio_rx_copy_slab_ ## n,			\
			DT_INST_PROP(n, zrpc_virtio_tx_chunk_size),	\
			DT_INST_PROP(n, zrpc_virtio_rx_queue_size) +	\
				DT_INST_PROP(				\
					n,				\
					zrpc_virtio_max_concurrent_replies\
				),					\
			alignof(struct zrpc_msghdr)			\
		);),							\
		()							\
	)								\
									\
									\
	static struct zrpc_virtio_data zrpc_virtio_data_ ## n = {	\
		.ipm_stack = zrpc_virtio_ipm_stack_ ## n,		\
		.rx_stack= zrpc_virtio_rx_stack_ ## n,			\
		.rx_queue = &zrpc_virtio_rx_queue_ ## n,		\
		.reply_queue = &zrpc_virtio_reply_queue_ ## n,		\
		.wait_slab = &zrpc_virtio_wait_slab_ ## n,		\
		.rx_copy_slab = COND_CODE_1(				\
			DT_INST_PROP(n, zrpc_virtio_copy_rx),		\
			(&zrpc_virtio_rx_copy_slab_ ## n),		\
			(NULL)						\
		),							\
	};								\
									\
	static_assert(							\
		DT_INST_PROP(n, zrpc_virtio_ctrl_block_size) >		\
			sizeof(struct zrpc_virtio_ctrl_blk),		\
		"Configured control block size is too small"		\
	);								\
	static_assert(							\
		DT_INST_REG_SIZE(n) >					\
			3u * DT_INST_PROP(				\
				n, zrpc_virtio_ctrl_block_size		\
			),						\
		"Shared memory must fit control block and two vrings"	\
	);								\
									\
	static struct zrpc_virtio_config const zrpc_virtio_cfg_ ## n = {\
		.host = DT_INST_PROP(n, zrpc_host),			\
		.have_dcache = DT_INST_PROP(n, zrpc_cpu_has_dcache),	\
		.shm_addr = DT_INST_REG_ADDR(n),			\
		.shm_size = DT_INST_REG_SIZE(n),			\
		.ctrl_blk_size = DT_INST_PROP(				\
			n, zrpc_virtio_ctrl_block_size			\
		),							\
		.num_vq_desc_extra = DT_INST_PROP(			\
			n, zrpc_virtio_virtqueue_num_extra_descs	\
		),							\
		.channel_id = DT_INST_PROP(n, zrpc_channel_id),		\
		.tx_chunk_size = DT_INST_PROP(				\
			n, zrpc_virtio_tx_chunk_size			\
		),							\
		.reply_lifetime = DT_INST_PROP(				\
			n, zrpc_virtio_reply_lifetime			\
		),							\
		.rx_timeout = DT_INST_PROP(				\
			n, zrpc_virtio_rx_timeout			\
		),							\
		.ipm_stack_size = K_THREAD_STACK_SIZEOF(		\
			zrpc_virtio_ipm_stack_ ## n			\
		),							\
		.rx_stack_size = K_THREAD_STACK_SIZEOF(			\
			zrpc_virtio_rx_stack_ ## n			\
		),							\
		.ipm_thread_name = "zRPC virtio IPM " #n,		\
		.rx_thread_name = "zRPC virtio RX " #n,			\
		.ipm_dev = DEVICE_DT_GET(				\
			DT_INST_PHANDLE(n, zrpc_virtio_ipm_handle)	\
		),							\
	};								\
									\
	DEVICE_DT_INST_DEFINE(						\
		n,							\
		zrpc_virtio_init,					\
		NULL,							\
		&zrpc_virtio_data_ ## n,				\
		&zrpc_virtio_cfg_ ## n,					\
		POST_KERNEL,						\
		CONFIG_ZRPC_VIRTIO_INIT_PRIORITY,			\
		&zrpc_virtio_api					\
	)								\
									\
	static int zrpc_virtio_pre_init_ ## n(void)			\
	{								\
		struct zrpc_virtio_config const *cfg =			\
			&zrpc_virtio_cfg_ ## n;				\
									\
		if (DT_INST_PROP(n, zrpc_host))	{			\
			zrpc_virtio_set_status_raw(			\
				cfg->shm_addr,				\
				VIRTIO_CONFIG_STATUS_RESET,		\
				DT_INST_PROP(n, zrpc_cpu_has_dcache)	\
			);						\
		}							\
		return 0;						\
	}								\
									\
	SYS_INIT(							\
		zrpc_virtio_pre_init_ ## n,				\
		PRE_KERNEL_1,						\
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT			\
	)

DT_INST_FOREACH_STATUS_OKAY(ZRPC_VIRTIO_INIT);

/** @} */
