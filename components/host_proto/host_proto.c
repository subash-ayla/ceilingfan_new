/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */

/*
 * Application protocol with host MCU.
 *
 * Layers:
 *	host_prop
 *	host_req
 *	(data_tlv, conf_tlv)
 *	host_proto
 *	mcu_uart (PPP)
 *	host_proto (adaptation to UART)
 *	app_ops
 *	uart_driver
 *
 * This interfaces to the uart driver and to the mcu_uart layer and connects
 * those two together.
 *
 * It also handles messages between the mcu_uart layer and
 * the mcu_data_tlv and mcu_ctl_tlv layers
 */
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ada/err.h>
#include <net/net.h>
#include <host_proto/mcu_dev.h>
#include <host_proto/host_proto.h>
#include "mcu_uart_int.h"
#include "host_prop.h"
#include "conf_tlv.h"
#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "host_proto_ota.h"

const struct mcu_dev *mcu_dev;

static const struct host_proto_ops *host_proto_app_ops;

/*
 * Open MCU UART and protocol to upper layer.
 */
void host_proto_init(const struct host_proto_ops *app_ops)
{
	host_proto_app_ops = app_ops;

	mcu_dev = mcu_uart_init();
	if (!mcu_dev) {
		log_put_mod(MOD_LOG_IO, LOG_ERR
		    "host_proto_init mcu_init failed");
		return;
	}

	hp_buf_callback_init();
	hp_buf_init();
	host_prop_init();
	conf_tlv_msg_init();
	host_proto_ota_init();
}

void host_proto_callback_pend(struct net_callback *cb)
{
	host_proto_app_ops->callback_pend(cb);
}

void *host_proto_call_blocking(void *(*func)(void *arg), void *arg)
{
	return host_proto_app_ops->call_blocking(func, arg);
}

void host_proto_timer_set(struct timer *tm, u32 delay_ms)
{
	host_proto_app_ops->timer_set(tm, delay_ms);
}

void host_proto_timer_cancel(struct timer *tm)
{
	host_proto_app_ops->timer_cancel(tm);
}
