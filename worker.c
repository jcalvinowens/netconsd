/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <urcu/urcu-qsbr.h>
#include <urcu/rculfhash.h>
#include <urcu/list.h>

#include <ncrx.h>

#include "include/common.h"
#include "include/msgbuf-struct.h"
#include "include/output.h"
#include "include/worker.h"

static const struct ncrx_param ncrx_param = {
	.nr_slots = 512,
	.retx_intv = NETCONS_RTO,
	.msg_timeout = NETCONS_RTO,
	.oos_timeout = NETCONS_RTO,
};

static unsigned long hash_srcaddr(struct in6_addr *addr)
{
	uint32_t *addrptr = (uint32_t *)addr;

	return jhash2(addrptr, sizeof(*addr) / sizeof(*addrptr), WORKER_SEED);
}

static int lfht_match_srcaddr(struct cds_lfht_node *node, const void *key)
{
	struct bucket *bkt = caa_container_of(node, struct bucket, lfht_node);
	struct in6_addr *src = key;

	return memcmp(bkt->src, src) == 0;
}

static struct bucket *hlookup(struct cds_lfht *hashtable, struct in6_addr *src)
{
	struct cds_lfht_node *ret;

	// FIXME
}

/*
 * Use -1 to represent "no wake needed"
 */
static void reset_waketime(struct ncrx_worker *cur)
{
	cur->wake.tv_sec = -1;
}

static uint64_t ms_from_timespec(struct timespec *t)
{
	return t->tv_sec * 1000LL + t->tv_nsec / 1000000L;
}

/*
 * Update the waketime if @when is before the current waketime.
 *
 * We assume that CLOCK_MONOTONIC cannot wrap: strictly speaking this is wrong,
 * since POSIX allows the MONOTONIC clock to start from any arbitrary value; but
 * since it starts from zero on Linux I'm not going to jump through the hoops.
 */
static void maybe_update_wake(struct ncrx_worker *cur, uint64_t when)
{
	uint64_t curwake = ms_from_timespec(&cur->wake);
	if ((int64_t)curwake >= 0LL && curwake <= when)
		return;

	cur->wake.tv_sec = when / 1000LL;
	cur->wake.tv_nsec = (when % 1000LL) * 1000000L;
}

static const struct timespec end_of_time = {
	.tv_sec = (time_t)((1ULL << ((sizeof(time_t) << 3) - 1)) - 1),
};

static const struct timespec *next_waketime(struct ncrx_worker *cur)
{
	if (cur->wake.tv_sec == -1)
		return &end_of_time;

	return &cur->wake;
}

static struct bucket *bucket_from_timernode(struct cds_list_head *node)
{
	return caa_container_of(node, struct bucket, timer_node);
}

/*
 * Return the callback time of the newest item on the list
 */
static uint64_t timerlist_peek(struct cds_list_head *list)
{
	if (cds_list_empty(list))
		return 0;

	return list->prev->timer_expiry;
}

static struct cds_list_head *create_timerlists(void)
{
	struct cds_list_head *ret;
	int i;

	ret = calloc(NETCONS_RTO, sizeof(*ret));
	if (!ret)
		fatal("Unable to allocate timerlist\n");

	for (i = 0; i < NETCONS_RTO; i++)
		CDS_INIT_LIST_HEAD(&ret[i]);

	return ret;
}

static void destroy_timerlists(struct cds_list_head *timerlist)
{
	free(timerlist);
}

static void schedule_ncrx_callback(struct ncrx_worker *cur, struct bucket *bkt,
		uint64_t when)
{
	struct cds_list_head *tgtlist;
	uint64_t now;

	if (when == UINT64_MAX) {
		/*
		 * No callback needed. If we had one we no longer need it, so
		 * just remove ourselves from the timerlist.
		 */
		if (!cds_list_empty(&bkt->timer_node))
			cds_list_del_init(&bkt->timer_node);

		return;
	}

	/*
	 * Never queue messages outside the current window. This clamp() is what
	 * guarantees that the callbacks in the timerlists are strictly ordered
	 * from least to most recent: at any given moment only one callback time
	 * corresponds to each bucket, and time cannot go backwards.
	 */
	now = now_mono_ms();
	when = clamp(when, now + 1, now + NETCONS_RTO);

	/*
	 * If the bucket is already on a timerlist, we only requeue it if the
	 * callback needs to happen earlier than the one currently queued.
	 */
	if (!cds_list_empty(&bkt->timer_node)) {
		if (when > bkt->timernode.when)
			return;

		cds_list_del_init(&bkt->timer_node);
	}

	tgtlist = &cur->tlist[when % NETCONS_RTO];
	fatal_on(when < timerlist_peek(tgtlist), "Timerlist ordering broken\n");

	bkt->timernode.when = when;
	cds_list_add_tail(&bkt->timer_node, tgtlist);
	maybe_update_wake(cur, when);
}

/*
 * Read any pending messages out of the bucket, and invoke the output pipeline
 * with the extended metadata.
 */
static void drain_bucket_ncrx(struct ncrx_worker *cur, struct bucket *bkt)
{
	struct ncrx_msg *out;
	uint64_t when;

	while ((out = ncrx_next_msg(bkt->ncrx))) {
		execute_output_pipeline(cur->thread_nr, &bkt->src, NULL, out);
		free(out);
	}

	when = ncrx_invoke_process_at(bkt->ncrx);
	schedule_ncrx_callback(cur, bkt, when);
}

/*
 * Execute callbacks for a specific timerlist, until either the list is empty or
 * we reach an entry that was queued for a time in the future.
 */
static void do_ncrx_callbacks(struct ncrx_worker *cur,
			      struct cds_list_head *list)
{
	uint64_t now = now_mono_ms();
	struct cds_list_head *tnode, *tmp;
	struct bucket *bkt;

	cds_list_for_each_safe(tnode, tmp, list) {
		if (tnode->when > now)
			break;

		/*
		 * Remove the bucket from the list first, since it might end up
		 * being re-added to another timerlist by drain_bucket_ncrx().
		 */
		cds_list_del_init(tnode);

		bkt = bucket_from_timernode(tnode);
		ncrx_process(NULL, now, 0, bkt->ncrx);
		drain_bucket_ncrx(cur, bkt);
	}
}

/*
 * We have no idea how large the queue we just processed was: it could have
 * taken tens of seconds. So we must handle wraparound in the tlist array.
 */
static uint64_t run_ncrx_callbacks(struct ncrx_worker *cur, uint64_t lastrun)
{
	uint64_t i, now = now_mono_ms();

	if (now == lastrun)
		goto out;

	fatal_on(now < lastrun, "Time went backwards\n");

	/*
	 * It's possible we wrapped: in that case, we simply iterate over the
	 * entire wheel and drain each list until we hit a callback after now.
	 * Otherwise, we only iterate over the buckets that lie on [last,now].
	 */
	for (i = max(lastrun, now - NETCONS_RTO + 1); i <= now; i++)
		do_ncrx_callbacks(cur, &cur->tlist[i % NETCONS_RTO]);

out:
	return now;
}

static void consume_msgbuf(struct ncrx_worker *cur, struct msg_buf *buf)
{
	struct bucket *ncrx_bucket;

	ncrx_bucket = hlookup(cur->ht, &buf->src.sin6_addr);
	if (!ncrx_bucket->ncrx) {
		ncrx_bucket->ncrx = ncrx_create(&ncrx_param);
		cds_list_init(&ncrx_bucket->timer_node);
		memcpy(&ncrx_bucket->src, &buf->src.sin6_addr,
				sizeof(ncrx_bucket->src));
		cur->ht->load++;
	}

	ncrx_bucket->last_seen = buf->rcv_time;

	buf->buf[buf->rcv_bytes] = '\0';
	if (!ncrx_process(buf->buf, now_mono_ms(), buf->rcv_time,
			ncrx_bucket->ncrx)) {
		drain_bucket_ncrx(cur, ncrx_bucket);
		return;
	}

	execute_output_pipeline(cur->thread_nr, &ncrx_bucket->src, buf, NULL);
}

static struct msg_buf *grab_prequeue(struct ncrx_worker *cur)
{
	struct msg_buf *ret;

	assert_pthread_mutex_locked(&cur->queuelock);
	ret = cur->queue_head;
	cur->queue_head = NULL;

	return ret;
}

void *ncrx_worker_thread(void *arg)
{
	struct ncrx_worker *cur = arg;
	struct msg_buf *curbuf, *tmp;
	uint64_t lastrun = now_mono_ms();
	int nr_dequeued;

	urcu_qsbr_register_thread();
	cur->tlist = create_timerlists();

	reset_waketime(cur);
	pthread_mutex_lock(&cur->queuelock);
	while (!cur->stop) {
		urcu_qsbr_thread_offline();
		pthread_cond_timedwait(&cur->cond, &cur->queuelock,
				next_waketime(cur));
		urcu_qsbr_thread_online();
		reset_waketime(cur);
morework:
		curbuf = grab_prequeue(cur);
		nr_dequeued = cur->nr_queued;
		cur->nr_queued = 0;
		pthread_mutex_unlock(&cur->queuelock);

		while ((tmp = curbuf)) {
			consume_msgbuf(cur, curbuf);
			curbuf = curbuf->next;
			free(tmp);

			cur->processed++;
		}

		if (!cur->stop)
			lastrun = run_ncrx_callbacks(cur, lastrun);

		urcu_qsbr_quiescent_state();

		pthread_mutex_lock(&cur->queuelock);
		if (cur->queue_head)
			goto morework;
	}

	assert_pthread_mutex_locked(&cur->queuelock);
	fatal_on(cur->queue_head != NULL, "Worker queue not empty at exit\n");

	cur->hosts_seen = cur->ht->load;
	destroy_timerlists(cur->tlist);
	urcu_qsbr_unregister_thread();
	return NULL;
}
