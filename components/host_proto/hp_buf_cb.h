/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_HP_BUF_CB_H__
#define __AYLA_HP_BUF_CB_H__

#define HP_BUF_CB_COUNT	20	/* number of callbacks available */

/*
 * Allocate a buffer and invoke a callback with the buffer when done.
 * The function called back will return non-zero if it did not use the buffer
 * and needs to be called again.
 */
void hp_buf_callback_pend(void (*func)(struct hp_buf *));

/*
 * Invoke pending callbacks.
 * Called when a buffer becomes free.
 */
void hp_buf_callback_invoke(void);

/*
 * Initialize the pool of callbacks.
 */
void hp_buf_callback_init(void);

#endif /* __AYLA_HP_BUF_CB_H__ */
