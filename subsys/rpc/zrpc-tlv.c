/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup zrpc_tlv
 * @brief zRPC generic TLV implementation.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include <zephyr/rpc/zrpc-tlv.h>

/**
 * @ingroup zrpc_tlv
 * @{
 */

void zrpc_tlvb_init(struct zrpc_tlvb *restrict tlvb, uint32_t capacity,
		void *restrict buf);

uint32_t zrpc_tlvb_space(struct zrpc_tlvb const *tlvb);
int zrpc_tlvb_skip(struct zrpc_tlvb *tlvb, size_t len);

struct zrpc_attr *zrpc_tlvb_head_(struct zrpc_tlvb *tlvb);
struct zrpc_attr const *zrpc_tlvb_chead_(struct zrpc_tlvb const *tlvb);

size_t zrpc_tlvb_len(struct zrpc_tlvb const *tlvb);

struct zrpc_attr *zrpc_tlvb_get_(struct zrpc_tlvb *tlvb);
struct zrpc_attr const *zrpc_tlvb_cget_(struct zrpc_tlvb const *tlvb);

struct zrpc_attr *zrpc_tlvb_start_(struct zrpc_tlvb *tlvb);
struct zrpc_attr const *zrpc_tlvb_cstart_(struct zrpc_tlvb const *tlvb);

bool zrpc_tlvb_empty(struct zrpc_tlvb const *tlvb);

uint32_t zrpc_attr_len(struct zrpc_attr const *attr);
uint32_t zrpc_attr_size(struct zrpc_attr const *attr);
uint32_t zrpc_attr_total_size(struct zrpc_attr const *attr);

uint8_t *zrpc_attr_data_(struct zrpc_attr *attr);
uint8_t const *zrpc_attr_cdata_(struct zrpc_attr const *attr);

uint8_t zrpc_attr_get_u8(struct zrpc_attr const *attr);
uint16_t zrpc_attr_get_u16(struct zrpc_attr const *attr);
uint32_t zrpc_attr_get_u32(struct zrpc_attr const *attr);
char const *zrpc_attr_get_string(struct zrpc_attr const *attr);

struct zrpc_attr *zrpc_attr_next_(struct zrpc_attr *attr);
struct zrpc_attr const *zrpc_attr_cnext_(struct zrpc_attr const *attr);

int zrpc_tlvb_put_u8(struct zrpc_tlvb *tlvb, zrpc_tag tag, uint8_t v);
int zrpc_tlvb_put_u16(struct zrpc_tlvb *tlvb, zrpc_tag tag, uint16_t v);
int zrpc_tlvb_put_u32(struct zrpc_tlvb *tlvb, zrpc_tag tag, uint32_t v);
int zrpc_tlvb_put_u64(struct zrpc_tlvb *tlvb, zrpc_tag tag, uint64_t v);
int zrpc_tlvb_put_string(struct zrpc_tlvb *tlvb, zrpc_tag tag, char const *s);

int zrpc_attr_get_u64(struct zrpc_attr const *attr)
{
	uint64_t aligned;
	unsigned long d;

	if (unlikely(zrpc_attr_len(attr) != sizeof(uint64_t)))
		return -EINVAL;

	d = (unsigned long)zrpc_attr_data(attr);
	if (d & (alignof(aligned) - 1u))
		memcpy(&aligned, zrpc_attr_data(attr), sizeof(aligned));
	else
		aligned = *(uint64_t const *)zrpc_attr_data(attr);

	return aligned;
}


int zrpc_tlvb_put(struct zrpc_tlvb *restrict tlvb, zrpc_tag tag,
		void const *restrict data, uint32_t n)
{
	struct zrpc_attr *attr = zrpc_tlvb_head(tlvb);
	size_t const totsz = sizeof(*attr) + zrpc_align(n);

#ifdef CONFIG_ZRPC_PEDANTIC
	/* Safe to align up? */
	if (unlikely(SIZE_MAX - (n & ~(zrpc_alignto - 1u)) < zrpc_alignto))
		return -EOVERFLOW;

	/* Can total size be represented as size_t */
	if (unlikely(SIZE_MAX - zrpc_align(n) < sizeof(*attr)))
		return -EOVERFLOW;

	/* Does total size fit in u16? */
	if (unlikely(totsz > UINT16_MAX))
		return -EOVERFLOW;
#endif

	if (unlikely(zrpc_tlvb_space(tlvb) < totsz))
		return -ENOBUFS;

	if (likely(n))
		memcpy(zrpc_attr_data(attr), data, n);

	attr->tag = tag;
	attr->len = sizeof(*attr) + n;
	tlvb->head += zrpc_align(attr->len);
	return 0;
}

size_t zrpc_tlvb_map_(struct zrpc_tlvb *tlvb, struct zrpc_attr **attrs,
	size_t nattrs)
{
	size_t n = 0u;
	struct zrpc_attr *attr;

	for (size_t i = 0u; i < nattrs; ++i)
		attrs[i] = NULL;

	zrpc_tlvb_foreach(tlvb, attr) {
		if (unlikely(attr->tag >= nattrs))
			continue;
		attrs[attr->tag] = attr;
		++n;
	}

	return n;
}

size_t zrpc_tlvb_cmap_(struct zrpc_tlvb const *tlvb,
	struct zrpc_attr const **attrs, size_t nattrs)
{
	size_t n = 0u;
	struct zrpc_attr const *attr;

	for (size_t i = 0u; i < nattrs; ++i)
		attrs[i] = NULL;

	zrpc_tlvb_foreach(tlvb, attr) {
		if (unlikely(attr->tag >= nattrs))
			continue;
		attrs[attr->tag] = attr;
		++n;
	}

	return n;
}

/** @} */
