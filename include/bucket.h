/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef __NCRX_BUCKET_H__
#define __NCRX_BUCKET_H__

#include <urcu/rculfhash.h>
#include <urcu/list.h>

struct bucket {
	struct in6_addr src;
	struct ncrx *ncrx;
	uint64_t last_seen;
	uint64_t timer_expiry;
	struct cds_list_head timer_node;
	struct cds_lfht_node hash_node;
	struct rcu_head rcu;
};

#endif /* __NCRX_BUCKET_H__ */
