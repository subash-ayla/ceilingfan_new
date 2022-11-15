/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/tlv.h>
#include <ayla/log.h>
#include <ayla/conf.h>
#include <ayla/ayla_proto_mcu.h>
#include <ada/err.h>
#include <adw/wifi.h>
#include <host_proto/mcu_dev.h>
#include "data_tlv.h"
#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "hp_buf_tlv.h"

static u8 adw_tlv_wifi_status_seq;	/* last seen status sequence */

static void data_tlv_wifi_status_append(struct hp_buf *bp,
	enum ayla_tlv_type tlv_type, struct adw_wifi_status *sp)
{
	u8 msg[sizeof(sp->ssid) + 2];	/* 2 bytes for SSID len + event byte */

	/*
	 * The status TLV msg includes the SSID with a prepended length.
	 * The event byte follows immediately after the SSID.
	 */
	ASSERT(sp->ssid_len < sizeof(msg) - 2);
	msg[0] = sp->ssid_len;
	memcpy(&msg[1], sp->ssid, sp->ssid_len);
	msg[1 + sp->ssid_len] = (u8)sp->error;

	hp_buf_tlv_append(bp, tlv_type, msg, sp->ssid_len + 2);
}

/*
 * Send wifi notifications via an event message
 */
static void data_tlv_wifi_notify(struct hp_buf *bp)
{
	struct adw_wifi_status status;
	struct adw_wifi_status *sp = &status;

	if (adw_wifi_status_get(sp) || sp->seq == adw_tlv_wifi_status_seq) {
		hp_buf_free(bp);
		return;
	}
	adw_tlv_wifi_status_seq = sp->seq;

	data_tlv_cmd_req_set(bp, AD_EVENT);

	/*
	 * Previous production agents sent two TLVS on final status.
	 * It seems OK to send only one of them.
	 */
	data_tlv_wifi_status_append(bp,
	    sp->final ? ATLV_WIFI_STATUS_FINAL : ATLV_WIFI_STATUS, sp);
	mcu_dev->enq_tx(bp);
}

/*
 * Handle events from Wi-Fi.
 */
static void data_tlv_wifi_event(enum adw_wifi_event_id event, void *arg)
{
	switch (event) {
	case ADW_EVID_STATUS:
		hp_buf_callback_pend(data_tlv_wifi_notify);
		break;
	default:
		break;
	}
}

void data_tlv_wifi_init(void)
{
	adw_wifi_event_register(data_tlv_wifi_event, NULL);
}
