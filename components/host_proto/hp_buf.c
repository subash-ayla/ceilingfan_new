/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/ayla_proto_mcu.h>
#include <host_proto/host_proto.h>
#include "hp_buf.h"
#include "hp_buf_cb.h"

static struct hp_buf *hp_buf_free_list;
static void *hp_buf_thread;	/* thread that should be used for everything */

struct hp_buf *hp_buf_alloc(size_t len)
{
	struct hp_buf *bp;

	ASSERT(host_app_curthread() == hp_buf_thread);
	if (len > HP_BUF_LEN) {
		return NULL;
	}
	bp = hp_buf_free_list;
	if (!bp) {
		return NULL;
	}
	hp_buf_free_list = bp->next;
	bp->payload = bp + 1;
	bp->len = len;
	bp->next = NULL;
	return bp;
}

static void hp_buf_free_int(struct hp_buf *bp)
{
	bp->next = hp_buf_free_list;
	hp_buf_free_list = bp;
}

void hp_buf_free(struct hp_buf *bp)
{
	ASSERT(host_app_curthread() == hp_buf_thread);
	if (bp) {
		hp_buf_free_int(bp);
		hp_buf_callback_invoke();
	}
}

static struct hp_buf *hp_buf_new(size_t len)
{
	struct hp_buf *bp;

	ASSERT(host_app_curthread() == hp_buf_thread);
	bp = malloc(sizeof(*bp) + len);
	if (bp) {
		bp->payload = bp + 1;
		bp->len = len;
		bp->next = NULL;
	}
	return bp;
}

/*
 * Initialize a pool of buffers.
 */
int hp_buf_init(void)
{
	unsigned int i;
	struct hp_buf *bp;

	hp_buf_thread = host_app_curthread();
	for (i = 0; i < HP_BUF_COUNT; i++) {
		bp = hp_buf_new(HP_BUF_LEN);
		if (!bp) {
			ASSERT_NOTREACHED();
			return -1;
		}
		hp_buf_free_int(bp);
	}
	return 0;
}
