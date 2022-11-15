/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_HOST_PROTO_INT_H__
#define __AYLA_HOST_PROTO_INT_H__

/*
 * Schedule callback.
 *
 * Callbacks run from host_proto_event(), usually called from the main thread.
 * This is reliable.  The callback may be already pending.
 */
void host_proto_callback_pend(struct net_callback *cb);

/*
 * Call function while blocking callbacks.
 */
void *host_proto_call_blocking(void *(*func)(void *arg), void *arg);

/*
 * Schedule a timer.
 */
void host_proto_timer_set(struct timer *tm, u32 delay_ms);

/*
 * Cancel a timer.
 */
void host_proto_timer_cancel(struct timer *tm);

#endif /* __AYLA_HOST_PROTO_INT_H__ */
