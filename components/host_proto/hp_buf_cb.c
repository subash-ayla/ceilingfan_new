/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <sys/queue.h>
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <al/al_os_lock.h>
#include <ayla/log.h>
#include <ayla/clock.h>
#include <ada/err.h>
#include <ada/client.h>
#include <net/net.h>
#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "host_proto_int.h"

struct hp_buf_callback {
	void (*handler)(struct hp_buf *);
	STAILQ_ENTRY(hp_buf_callback) list;
};

STAILQ_HEAD(hp_buf_cb_head, hp_buf_callback);	/* struct for list head */

static struct hp_buf_cb_head hp_buf_cb_active =
    STAILQ_HEAD_INITIALIZER(hp_buf_cb_active);	/* list of active callbacks */

static struct hp_buf_cb_head hp_buf_cb_free =
    STAILQ_HEAD_INITIALIZER(hp_buf_cb_free);	/* list of free callbacks */

static struct net_callback hp_buf_callback_net_cb;
static struct al_lock *hp_buf_lock;

/*
 * Find callback in the active list.
 * Called with lock held.
 */
static struct hp_buf_callback *
hp_buf_callback_find(void (*handler)(struct hp_buf *))
{
	struct hp_buf_callback *cb;

	STAILQ_FOREACH(cb, &hp_buf_cb_active, list) {
		if (cb->handler == handler) {
			return cb;
		}
	}
	return NULL;
}

/*
 * Allocate a buffer from the free list or by malloc.
 * If the freelist was empty, a potential error exists.
 * Called with lock held.
 */
static struct hp_buf_callback *hp_buf_callback_alloc(void)
{
	struct hp_buf_callback *cb;

	cb = STAILQ_FIRST(&hp_buf_cb_free);
	if (cb) {
		STAILQ_REMOVE_HEAD(&hp_buf_cb_free, list);
		cb->list.stqe_next = NULL;
	} else {
		log_put(LOG_ERR "%s: cb freelist empty", __func__);
		cb = calloc(1, sizeof(*cb));
		if (!cb) {
			log_put(LOG_ERR "%s: cb malloc fails", __func__);
		}
	}
	return cb;
}

/*
 * Free callback structure back to the freelist.
 * Called with lock held.
 */
static void hp_buf_callback_free(struct hp_buf_callback *cb)
{
	STAILQ_INSERT_TAIL(&hp_buf_cb_free, cb, list);
}

/*
 * Allocate a buffer and invoke a callback with a buffer as the arg when done.
 */
void hp_buf_callback_pend(void (*handler)(struct hp_buf *))
{
	struct hp_buf_callback *cb;

	al_os_lock_lock(hp_buf_lock);
	cb = hp_buf_callback_find(handler);
	if (cb) {
		al_os_lock_unlock(hp_buf_lock);
		return;		/* already pending */
	}
	cb = hp_buf_callback_alloc();
	ASSERT(cb);
	cb->handler = handler;
	STAILQ_INSERT_TAIL(&hp_buf_cb_active, cb, list);
	host_proto_callback_pend(&hp_buf_callback_net_cb);
	al_os_lock_unlock(hp_buf_lock);
}

void hp_buf_callback_invoke(void)
{
	if (STAILQ_FIRST(&hp_buf_cb_active)) {
		al_os_lock_lock(hp_buf_lock);
		host_proto_callback_pend(&hp_buf_callback_net_cb);
		al_os_lock_unlock(hp_buf_lock);
	}
}

static void hp_buf_callback_handle(void *arg)
{
	struct hp_buf_callback *cb;
	void (*handler)(struct hp_buf *bp);
	struct hp_buf *bp;

	for (;;) {
		al_os_lock_lock(hp_buf_lock);
		cb = STAILQ_FIRST(&hp_buf_cb_active);
		if (!cb) {
			al_os_lock_unlock(hp_buf_lock);
			client_reset_mcu_overflow();
			return;
		}
		bp = hp_buf_alloc(HP_BUF_LEN);
		if (!bp) {
			al_os_lock_unlock(hp_buf_lock);
			return;
		}
		STAILQ_REMOVE_HEAD(&hp_buf_cb_active, list);
		handler = cb->handler;
		hp_buf_callback_free(cb);
		al_os_lock_unlock(hp_buf_lock);
		handler(bp);
	}
}

void hp_buf_callback_init(void)
{
	struct hp_buf_callback *cb;
	unsigned int i;

	hp_buf_lock = al_os_lock_create();
	ASSERT(hp_buf_lock);
	net_callback_init(&hp_buf_callback_net_cb,
	    hp_buf_callback_handle, NULL);

	for (i = 0; i < HP_BUF_CB_COUNT; i++) {
		cb = malloc(sizeof(*cb));
		if (!cb) {
			ASSERT_NOTREACHED();
			return;
		}
		cb->handler = NULL;
		STAILQ_INSERT_TAIL(&hp_buf_cb_free, cb, list);
	}
}
