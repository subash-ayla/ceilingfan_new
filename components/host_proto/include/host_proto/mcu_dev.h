/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_MCU_DEV_H__
#define __AYLA_MCU_DEV_H__

struct hp_buf;

/*
 * Abstract device to communicate via packets to MCU.
 * Could be SPI or UART or something else.
 *
 * At most one of these is active.
 */
struct mcu_dev {
	void (*enq_tx)(struct hp_buf *bp);
	void (*set_ads)(void);
	void (*clear_ads)(void);
	void (*show)(void);
	void (*ping)(void *, size_t);
};

extern const struct mcu_dev *mcu_dev;

#endif /* __AYLA_MCU_DEV_H__ */
