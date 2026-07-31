/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup zrpc
 * @brief zRPC core source file.
 */

#include <inttypes.h>
#include <stdint.h>

#include <zephyr/random/random.h>
#include <zephyr/rpc/zrpc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/toolchain.h>

/**
 * @ingroup zrpc
 * @{
 */

uint16_t zrpc_seq_next(void)
{
	static atomic_t next = ATOMIC_INIT(UINT16_MAX + 1u);
	atomic_val_t v = atomic_get(&next);

	if (unlikely(v == UINT16_MAX + 1u)) {
		v = sys_rand16_get();
		atomic_cas(&next, UINT16_MAX + 1u, v);
	}

	while (!atomic_cas(&next, v, (v + 1) & UINT16_MAX))
		v = atomic_get(&next);

	return (uint16_t)v;
}

/** @} */
