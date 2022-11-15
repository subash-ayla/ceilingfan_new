/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_HP_BUF_H__
#define __AYLA_HP_BUF_H__

#include <ayla/ayla_proto_mcu.h>

/*
 * Size of all buffers in the pool.
 */
#define	HP_BUF_LEN	ASPI_LEN_MAX

/*
 * Number of buffers in the pool.
 * These are used for transmit and six should be plenty for one
 * outstanding message and acks.
 */
#define HP_BUF_COUNT	6

/*
 * Buffer for host MCU communication.
 */
struct hp_buf {
	void	*payload;
	size_t	len;
	struct hp_buf *next;
};

/*
 * Allocate a buffer of the specified length, and initialize it.
 */
struct hp_buf *hp_buf_alloc(size_t len);

/*
 * Free a buffer.
 * It is valid to call this with a NULL pointer to skip checks in the caller.
 */
void hp_buf_free(struct hp_buf *bp);

/*
 * Initialize a pool of buffers.
 */
int hp_buf_init(void);

#endif /* __AYLA_HP_BUF_H__ */
