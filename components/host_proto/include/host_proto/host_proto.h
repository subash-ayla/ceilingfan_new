/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_HOST_PROTO_H__
#define __AYLA_HOST_PROTO_H__

struct net_callback;
struct timer;

/*
 * Functions called by host protocol to the outer application.
 */
struct host_proto_ops {
	void (*timer_set)(struct timer *, u32);	/* set timer */
	void (*timer_cancel)(struct timer *);	/* cancel timer */

	void (*callback_pend)(struct net_callback *cb);

	/*
	 * Call a function while blocking callbacks.
	 */
	void *(*call_blocking)(void *(*func)(void *arg), void *arg);
};

void host_proto_init(const struct host_proto_ops *ops);

/*
 * Send the host MCU a message to reset it.
 */
void host_proto_reset_send(void);

/*
 * Get an opaque identifier for the current thread.
 * Supplied by application and used for assertions.
 */
void *host_app_curthread(void);

#endif /* __AYLA_HOST_PROTO_H__ */
