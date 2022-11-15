/*
 * Copyright 2011-2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <ayla/utypes.h>
#include <ayla/endian.h>
#include <ayla/assert.h>
#include <ayla/tlv.h>
#include <ayla/log.h>
#include <ayla/crc.h>
#include <ayla/clock.h>
#include <ayla/conf.h>
#include <ayla/nameval.h>
#include <ayla/ayla_spi_mcu.h>
#include <ayla/ayla_proto_mcu.h>
#include <ada/err.h>
#include <ada/client.h>
#include <ada/prop.h>
#include <ada/ada_conf.h>
#include <host_proto/mcu_dev.h>

#include "conf_tlv.h"
#include "data_tlv.h"
#include "host_prop.h"
#include "prop_req.h"
#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "hp_buf_tlv.h"

/*
 * The mcu_feature_mask is the set of features given by the MCU
 * ORed with those required by the agent or the transport.
 * For example, UART requires MCU_DATAPOINT_CONFIRM.
 */
u8 mcu_feature_mask;
u8 mcu_feature_mask_min;

static u8 data_tlv_pkt[TLV_MAX_STR_LEN + 1];
static u32 data_tlv_next_off;
static size_t data_tlv_tot_size;
static u16 data_tlv_req_id;
static u16 data_tlv_recv_req_id;
static char data_tlv_recv_name[(PROP_LOC_LEN > PROP_NAME_LEN) ?
    PROP_LOC_LEN : PROP_NAME_LEN];
static char nak_prop_name[PROP_NAME_LEN];
static u8 data_tlv_next_off_val;
static struct prop_dp_meta dp_metadata[PROP_MAX_DPMETA];
static struct prop_dp_meta *dp_meta_off = dp_metadata;
#ifdef AYLA_HOST_PROP_ACK_SUPPORT
static struct prop_ack prop_ack;
#endif
static char echo_fail_name[PROP_NAME_LEN];
static char nak_prop_name[PROP_NAME_LEN];
static u16 nak_req_id;
static u16 confirm_req_id;
static u8 nak_err;
static u8 notify_err;
static u8 nak_fail_mask;
static u8 nak_clear_ads;
static u8 data_tlv_nak_last_err;
static struct hp_buf *data_tlv_nak_buf;
static u8 confirm_needed;
static u8 dp_evt_notified;
static u8 data_tlv_last_connect_mask;

/*
 * Driver set ADS_BUSY
 */
static void data_tlv_set_dev_ads_busy(void)
{
	mcu_dev->set_ads();

	/*
	 * If there was a pending prop get req
	 * then pause the timeout as MCU cannot
	 * respond when ADS is BUSY
	 */
	prop_req_timeout_halt();
}

/*
 * Driver clear ADS_BUSY
 */
static void data_tlv_clear_dev_ads_busy(void)
{
	if (mcu_dev->clear_ads) {
		mcu_dev->clear_ads();
	}

	/*
	 * If there was a pending prop get req
	 * then restart the timeout
	 */
	prop_req_timeout_restart();
}

/*
 * Set command fields in the buffer.
 * The buffer is guaranteed to be large enough to hold a command.
 * This sets the length of the buffer to the length of the command header,
 * so it must be before any hp_buf_tlv_append_xxx() calls.
 */
void data_tlv_cmd_set(struct hp_buf *bp, enum ayla_data_op op, u16 req_id)
{
	hp_buf_tlv_cmd_set(bp, ASPI_PROTO_DATA, op, req_id);
}

/*
 * Set command fields for a request in the buffer.
 * Returns the request ID used.
 */
u16 data_tlv_cmd_req_set(struct hp_buf *bp, enum ayla_data_op op)
{
	u16 req_id;

	req_id = data_tlv_req_id++;
	data_tlv_cmd_set(bp, op, req_id);
	return req_id;
}

/*
 * Send property request to MCU.
 */
u16 data_tlv_prop_req_send(struct hp_buf *bp, const char *name)
{
	u16 req_id;

	req_id = data_tlv_cmd_req_set(bp, AD_SEND_PROP);
	if (name) {
		hp_buf_tlv_append(bp, ATLV_NAME, name, strlen(name));
	}
	mcu_dev->enq_tx(bp);
	return req_id;
}

/*
 * Send get next property request to MCU.
 */
void data_tlv_req_next(struct hp_buf *bp, u32 continuation, u16 *req_id)
{
	*req_id = data_tlv_cmd_req_set(bp, AD_SEND_NEXT_PROP);
	hp_buf_tlv_append_be32(bp, ATLV_CONT, continuation);
	mcu_dev->enq_tx(bp);
}

/*
 * Send property change to MCU.
 */
enum ada_err data_tlv_send(struct hp_buf *bp, const char *name, const void *val,
	size_t val_len, enum ayla_tlv_type type, u32 *offset,
	u8 src, u16 req_id, u8 use_req_id,
	const char *ack_id, const struct prop_dp_meta *dp_meta)
{
	u8 send_partials = 0;
	size_t curr_val_len;
	const struct prop_dp_meta *meta = dp_meta;
	enum ayla_tlv_type msg_type = 0;

	/*
	 * Integer and boolean types are stored as 32-bit values.
	 */
	switch (type) {
	case ATLV_INT:
	case ATLV_UINT:
	case ATLV_CENTS:
		val_len = 4;
		break;
	case ATLV_BOOL:
		val_len = 1;
		break;
	case ATLV_MSG_JSON:
	case ATLV_MSG_UTF8:
	case ATLV_MSG_BIN:
		msg_type = type;
		type = ATLV_LOC;
		break;
	default:
		break;
	}
	if (val_len > TLV_MAX_LEN) {
		if (type != ATLV_UTF8 && type != ATLV_BIN) {
			/* no support for > 255 size for non-strs and */
			/* non-binaries */
			return AE_INVAL_VAL;
		}
		send_partials = 1;
	}
	val_len -= *offset;
	val = (char *)val + *offset;
	do {
		curr_val_len = val_len;
		if (curr_val_len > TLV_MAX_LEN) {
			curr_val_len = TLV_MAX_LEN;
		}
		if (use_req_id) {
			data_tlv_cmd_set(bp, AD_RECV_TLV, req_id);
		} else {
			data_tlv_cmd_req_set(bp, AD_RECV_TLV);
		}
		if (src > 1) {
			hp_buf_tlv_append_u8(bp, ATLV_NODES, src);
		}
		meta = dp_meta;
		while (meta && meta->key[0] != '\0' &&
		    meta->value[0] != '\0') {
			/*
			 * Metadata must be sent before prop name/val
			 * because property value can be split over multiple
			 * packets
			 */
			hp_buf_tlv_append_str(bp, ATLV_DPMETA, meta->key);
			hp_buf_tlv_append_str(bp, ATLV_UTF8, meta->value);
			meta++;
		}

		hp_buf_tlv_append_str(bp, ATLV_NAME, name);

#ifdef AYLA_HOST_PROP_ACK_SUPPORT
		if (ack_id && ack_id[0] != '\0' && type != ATLV_FILE) {
			hp_buf_tlv_append_str(bp, ATLV_ACK_ID, ack_id);
		}
#endif

		if (send_partials && !*offset) {
			/*
			 * this pkt needs to be sent in partials.
			 * since this is the first partial, include the
			 * total length.
			 */
			hp_buf_tlv_append_be32(bp, ATLV_LEN, val_len);
		} else if (*offset) {
			hp_buf_tlv_append_be32(bp, ATLV_OFF, *offset);
		}

		if (msg_type) {
			hp_buf_tlv_append_u8(bp, ATLV_MSG_TYPE, msg_type);
		}

		switch (type) {
		case ATLV_INT:
		case ATLV_UINT:
		case ATLV_CENTS:
			/* could use shorter lengths depending on value */
			hp_buf_tlv_append_be32(bp, type, *(u32 *)val);
			break;
		case ATLV_BOOL:
			hp_buf_tlv_append_u8(bp, type, *(u8 *)val);
			break;
		case ATLV_BIN:
		case ATLV_SCHED:
		case ATLV_UTF8:
		case ATLV_LOC:
			hp_buf_tlv_append(bp, type, val, curr_val_len);
			break;
		default:
			log_put(LOG_WARN "%s: name %s unknown type %#x",
			    __func__, name, type);
			hp_buf_free(bp);
			return AE_INVAL_VAL;
		}
		mcu_dev->enq_tx(bp);
		*offset += curr_val_len;
		val_len -= curr_val_len;
		val = (char *)val + curr_val_len;

		if (val_len) {
			bp = hp_buf_alloc(0);
			if (!bp) {
				return AE_BUF;
			}
		}
	} while (val_len);
	return AE_OK;
}


/*
 * Callback if data_tlv_nak_req fails
 * This is for NAK with a property name.
 */
static void data_tlv_nak_req_cb(struct hp_buf *bp)
{
	data_tlv_cmd_set(bp, AD_NAK, nak_req_id);
	hp_buf_tlv_append_u8(bp, ATLV_ERR, nak_err);
	hp_buf_tlv_append_str(bp, ATLV_NAME, nak_prop_name);
	hp_buf_tlv_append_u8(bp, ATLV_NODES, nak_fail_mask);

	mcu_dev->enq_tx(bp);

	if (nak_clear_ads) {
		data_tlv_clear_dev_ads_busy();
	}
}

/*
 * Send NAK for prop get/send request fails. Send with a name and the failed
 * destinations.
 */
void data_tlv_nak_req(u16 req_id, u8 err,
		const char *name, u8 failed_dests)
{
	nak_req_id = req_id;
	nak_err = err;
	strncpy(nak_prop_name, name, sizeof(nak_prop_name) - 1);
	nak_fail_mask = failed_dests;
	nak_clear_ads = 1;

	log_put(LOG_DEBUG "%s: prop '%s' req %#x err %#x",
	    __func__, name, req_id, err);
	hp_buf_callback_pend(data_tlv_nak_req_cb);
}

/*
 * Internal NAK.
 * This is allowed to fail if low on buffers, otherwise the host MCU
 * could cause the module to run out of resources.
 */
static void data_tlv_internal_nak(u16 req_id, u8 err)
{
	struct hp_buf *bp;

	bp = hp_buf_alloc(0);
	if (!bp) {
		log_put(LOG_WARN "%s: req %#x err %#x (not sent)",
		    __func__, req_id, err);
		return;
	}

	data_tlv_cmd_set(bp, AD_NAK, req_id);
	hp_buf_tlv_append_u8(bp, ATLV_ERR, err);

	log_put(LOG_DEBUG "%s: req %#x err %#x", __func__, req_id, err);

	mcu_dev->enq_tx(bp);
}

/*
 * Callback if data_tlv_nak fails
 * This is to send NAK without a property name, possibly with a backoff time.
 */
static void data_tlv_nak_cb(struct hp_buf *bp)
{
	u8 err = nak_err;

	data_tlv_cmd_set(bp, AD_NAK, nak_req_id);

	hp_buf_tlv_append_u8(bp, ATLV_ERR, err);
	if (err == AERR_BUSY_RETRY) {
		hp_buf_tlv_append_be32(bp, ATLV_ERR_BACKOFF,
		    host_prop_backoff_time_remaining());
	}

	mcu_dev->enq_tx(bp);

	if (nak_clear_ads) {
		data_tlv_clear_dev_ads_busy();
	}
}

/*
 * Callback if an error occurs thats not tied to a specific request from mcu.
 */
static void data_tlv_error_cb(struct hp_buf *bp)
{
	data_tlv_cmd_req_set(bp, AD_ERROR);
	hp_buf_tlv_append_u8(bp, ATLV_ERR, notify_err);

	mcu_dev->enq_tx(bp);
}

/*
 * Callback to send confirmation message to MCU of a successful datapoint post.
 */
static void data_tlv_confirmation_cb(struct hp_buf *bp)
{
	if (!confirm_needed) {
		hp_buf_free(bp);
		return;
	}
	data_tlv_cmd_set(bp, AD_CONFIRM, confirm_req_id);

	mcu_dev->enq_tx(bp);
	confirm_needed = 0;
	data_tlv_clear_dev_ads_busy();
}

/*
 * Send NAK
 */
void data_tlv_nak(u16 req_id, u8 err, u8 clear_ads)
{
	nak_req_id = req_id;
	nak_err = err;
	nak_clear_ads = clear_ads;

	log_put(LOG_DEBUG "%s: req %#x err %#x", __func__, req_id, err);
	hp_buf_callback_pend(data_tlv_nak_cb);
}

static void data_tlv_batch_final(struct hp_buf *bp)
{
	if (data_tlv_nak_buf) {
		hp_buf_free(bp);
	} else {
		data_tlv_nak_buf = bp;
	}
	data_tlv_batch_status(0, 0, 1, 0);
}

static void data_tlv_batch_status_send(struct hp_buf *bp)
{
	mcu_dev->enq_tx(bp);
	data_tlv_nak_buf = NULL;
}

/*
 * Send Batch Status
 *
 * If buffer is not available, a callback will be made when one might be
 * available.
 *
 * On the final status, no batch_id or status is expected to be provided.
 */
void data_tlv_batch_status(u16 batch_id, u8 status, u8 final,
		void (*callback)(struct hp_buf *bp))
{
	struct hp_buf *bp;
	size_t len_needed;
	size_t rlen;

	for (;;) {
		bp = data_tlv_nak_buf;
		if (!bp) {
			bp = hp_buf_alloc(ASPI_LEN_MAX);
			if (!bp) {
				hp_buf_callback_pend(callback ? callback :
				    data_tlv_batch_final);
				return;
			}

			data_tlv_cmd_req_set(bp, AD_BATCH_STATUS);

			data_tlv_nak_buf = bp;
			data_tlv_nak_last_err = ~0;	/* invalid status */
		}
		rlen = ASPI_LEN_MAX - bp->len;
		ASSERT(rlen < ASPI_LEN_MAX);	/* failure means overflow */

		if (final) {
			len_needed = sizeof(struct ayla_tlv);
		} else {
			len_needed = sizeof(struct ayla_tlv) + sizeof(u16);
			if (status != data_tlv_nak_last_err) {
				len_needed +=
				    sizeof(struct ayla_tlv) + sizeof(u8);
			}
		}

		/*
		 * If there's not room to append the status we want,
		 * send the accumulated status and allocate a new buf.
		 */
		if (hp_buf_tlv_space(bp) < len_needed) {
			data_tlv_batch_status_send(bp);
			continue;
		}
		break;
	}

	if (final) {
		hp_buf_tlv_append(bp, ATLV_BATCH_END, NULL, 0);
		data_tlv_batch_status_send(bp);
	} else {
		if (status != data_tlv_nak_last_err) {
			data_tlv_nak_last_err = status;
			hp_buf_tlv_append_u8(bp, ATLV_ERR, status);
		}

		hp_buf_tlv_append_be16(bp, ATLV_BATCH_ID, batch_id);
	}
}

/*
 * Send MCU an error
 */
void data_tlv_error(u8 err)
{
	notify_err = err;
	hp_buf_callback_pend(data_tlv_error_cb);
}

/*
 * Send the location of a dp_create request to MCU.
 */
enum ada_err data_tlv_dp_create(u16 req_id, const char *location)
{
	struct hp_buf *bp;

	bp = hp_buf_alloc(0);
	if (!bp) {
		return AE_BUF;
	}

	data_tlv_cmd_set(bp, AD_DP_RESP, req_id);
	hp_buf_tlv_append_str(bp, ATLV_LOC, location);
	data_tlv_next_off = 0;
	mcu_dev->enq_tx(bp);

	return AE_OK;
}

static int data_tlv_check_offsets(u16 req_id, struct ayla_tlv *off_tlv,
	struct ayla_tlv *len_tlv, const char *loc_or_name)
{
	u32 off = 0;
	u32 tot_tlv_len;

	if (off_tlv) {
		if (get_ua_with_len(off_tlv + 1, off_tlv->len, &off)) {
			return AERR_INVAL_OFF;
		}
	}
	if (!data_tlv_next_off_val) {
		if (off) {
			return AERR_INVAL_OFF;
		}
		data_tlv_next_off = off;
		data_tlv_next_off_val = 1;
		data_tlv_recv_req_id = req_id;
		strncpy(data_tlv_recv_name, loc_or_name,
		    sizeof(data_tlv_recv_name) - 1);
		if (len_tlv) {
			if (get_ua_with_len(len_tlv + 1, len_tlv->len,
			    &tot_tlv_len)) {
				return AERR_LEN_ERR;
			}
			if (tot_tlv_len > TLV_MAX_STR_LEN) {
				return AERR_PROP_LEN;
			}
			data_tlv_tot_size = tot_tlv_len;
		} else if (!data_tlv_next_off) {
			data_tlv_tot_size = TLV_MAX_STR_LEN;
		}
	} else if (data_tlv_recv_req_id != req_id ||
	    strcmp(data_tlv_recv_name, loc_or_name)) {
		return AERR_INVAL_REQ;
	}
	/*
	* Check that transfer is contiguous with previous transfers.
	* Generate NAK if needed.
	*/
	if (off != data_tlv_next_off) {
		log_put(LOG_WARN "%s: req_id=%x bad offset",
		    __func__, req_id);
		return AERR_INVAL_OFF;
	}
	if (off && len_tlv) {
		log_put(LOG_WARN "%s: req_id=%x bad len TLV",
		    __func__, req_id);
		return AERR_INVAL_REQ;
	}

	return 0;
}

/*
 * Receive property update from mcu
 */
int data_tlv_recv_tlv(u16 req_id, struct prop *prop, struct ayla_tlv *off_tlv,
		struct ayla_tlv *len_tlv)
{
	u8 err = AE_OK;

	if (host_prop_is_busy(__func__, prop->name)) {
		return AERR_ADS_BUSY;
	}
	err = data_tlv_check_offsets(req_id, off_tlv, len_tlv, prop->name);
	if (err) {
		return err;
	}
	if (data_tlv_next_off + prop->len > data_tlv_tot_size) {
		return AERR_PROP_LEN;
	}
	memcpy(data_tlv_pkt + data_tlv_next_off, prop->val, prop->len);
	data_tlv_next_off += prop->len;

	return err;
}

#ifdef AYLA_HOST_PROP_FILE_SUPPORT
/*
 * Receive a data point value portion from the MCU.
 */
static int data_tlv_dp_rx(u16 req_id, struct prop *prop,
	struct ayla_tlv *loc_tlv, struct ayla_tlv *off_tlv,
	struct ayla_tlv *len_tlv, u8 eof)
{
	u8 err;
	char loc[PROP_LOC_LEN];

	if (!loc_tlv) {
		log_put(LOG_WARN "data_tlv_dp_rx: missing or bad loc TLV");
		return AERR_INVAL_DP;
	}
	if (loc_tlv->len >= sizeof(loc)) {
		return AERR_INVAL_DP;
	}
	strncpy(loc, (char *)(loc_tlv + 1), loc_tlv->len);
	loc[loc_tlv->len] = '\0';
	if (host_prop_is_busy(__func__, loc)) {
		return AERR_ADS_BUSY;
	}
	err = data_tlv_check_offsets(req_id, off_tlv, NULL, loc);
	if (err) {
		return err;
	}
	if (len_tlv && get_ua_with_len(len_tlv + 1, len_tlv->len,
	    (u32 *)&data_tlv_tot_size)) {
		return AERR_LEN_ERR;
	}
	memcpy(data_tlv_pkt, prop->val, prop->len);
	data_tlv_set_dev_ads_busy();
	prop->val = data_tlv_pkt;
	err = prop_dp_put_data(req_id, prop, loc, data_tlv_next_off,
	    data_tlv_tot_size, eof);
	if (!err) {
		data_tlv_next_off += prop->len;
	} else if (err != AERR_ADS_BUSY) {
		data_tlv_clear_dev_ads_busy();
	}

	return err;
}

/*
 * Send the dp stream values to MCU.
 */
enum ada_err data_tlv_dp_stream_resp(u16 req_id, enum ayla_tlv_type type,
	const void *prop_val,
	size_t prop_val_len, u32 data_off, u32 tot_len,
	const char *location, u8 eof)
{
	struct hp_buf *bp;

	bp = hp_buf_alloc(0);
	if (!bp) {
		return AE_BUF;
	}

	data_tlv_cmd_set(bp, AD_DP_RESP, req_id);

	hp_buf_tlv_append_str(bp, ATLV_LOC, location);

	if (type == ATLV_LOC || data_off) {
		hp_buf_tlv_append_be32(bp, ATLV_OFF, data_off);
	} else if (tot_len) {
		hp_buf_tlv_append_be32(bp, ATLV_LEN, tot_len);
	}

	if (prop_val_len > 0) {
		if (type == ATLV_LOC) {
			type = ATLV_BIN;
		}
		hp_buf_tlv_append(bp, type, prop_val, prop_val_len);
	}
	if (eof) {
		hp_buf_tlv_append(bp, ATLV_EOF, NULL, 0);
	}

	mcu_dev->enq_tx(bp);
	return AE_OK;
}
#endif /* AYLA_FILE_PROP_SUPPORT */

/*
 * Send Connectivity Status Info
 */
static void data_tlv_send_connect_mask(struct hp_buf *bp)
{
	u8 connect_mask;

	data_tlv_cmd_req_set(bp, AD_CONNECT);

	connect_mask = client_get_connectivity_mask();
	hp_buf_tlv_append_u8(bp, ATLV_NODES, connect_mask);

	mcu_dev->enq_tx(bp);

	data_tlv_last_connect_mask = connect_mask;
}

/*
 * Notify host mcu of pending prop update
 */
static void data_tlv_send_prop_notify(struct hp_buf *bp)
{
	data_tlv_cmd_req_set(bp, AD_PROP_NOTIFY);
	mcu_dev->enq_tx(bp);
}

/*
 * Send client notifications via an event message.
 * These are for registration events.
 * AP mode events are not included here.
 */
static void data_tlv_send_client_notify(struct hp_buf *bp)
{
	struct ada_conf *cf = &ada_conf;
	u8 tlv_ev_mask;

	/* Get any events stored in the client event mask */
	tlv_ev_mask = cf->event_mask;
	if (!(tlv_ev_mask & (CLIENT_EVENT_REG | CLIENT_EVENT_UNREG))) {
		hp_buf_free(bp);	/* nothing to report */
		return;
	}
	cf->event_mask = 0;

	data_tlv_cmd_req_set(bp, AD_EVENT);

	/* append a TLV for each client notification that requires it */
	if (tlv_ev_mask & CLIENT_EVENT_UNREG) {
		hp_buf_tlv_append_u8(bp, ATLV_REGINFO, 0);
	}
	if (tlv_ev_mask & CLIENT_EVENT_REG) {
		hp_buf_tlv_append_u8(bp, ATLV_REGINFO, 1);
	}
	mcu_dev->enq_tx(bp);
}

/*
 * Send Connectivity Info Interface for Client
 */
void data_tlv_send_connectivity(u8 mask)
{
	/*
	 * If being notified of ADS reconnect before the down event was sent,
	 * re-enable listen here.
	 */
	if ((mask & data_tlv_last_connect_mask & NODES_ADS) != 0) {
		host_prop_enable_listen();
	}
	hp_buf_callback_pend(data_tlv_send_connect_mask);
}

/*
 * Setup a callback to notify host mcu of a pending update.
 */
void data_tlv_send_prop_notification(void)
{
	dp_evt_notified = 1;
	hp_buf_callback_pend(data_tlv_send_prop_notify);
}

/*
 * Send a registration change event notfication to host MCU.
 * This function should be linked instead of the one in libapp_conf.c.
 */
void adap_conf_reg_changed(void)
{
	struct ada_conf *cf = &ada_conf;

	log_put(LOG_INFO "data_tlv: user reg %u", cf->reg_user);
	hp_buf_callback_pend(data_tlv_send_client_notify);
}

/*
 * Send an EOF packet to MCU.
 */
enum ada_err data_tlv_send_eof(u16 req_id)
{
	struct hp_buf *bp;

	bp = hp_buf_alloc(0);
	if (!bp) {
		return AE_BUF;
	}
	data_tlv_cmd_set(bp, AD_RECV_TLV, req_id);
	hp_buf_tlv_append(bp, ATLV_EOF, NULL, 0);

	mcu_dev->enq_tx(bp);

	return AE_OK;
}

/*
 * Send Host MCU Echo failure for a property
 */
static void data_tlv_send_echo_failure(struct hp_buf *bp)
{
	data_tlv_cmd_req_set(bp, AD_ECHO_FAIL);
	hp_buf_tlv_append_str(bp, ATLV_NAME, echo_fail_name);
	mcu_dev->enq_tx(bp);
}

/*
 * Send Connectivity Info Interface for Client
 */
void data_tlv_send_echofail(char *prop_name)
{
	strncpy(echo_fail_name, prop_name, sizeof(echo_fail_name) - 1);
	hp_buf_callback_pend(data_tlv_send_echo_failure);
}

static int data_tlv_check_dp_metadata(struct prop *prop)
{
	int i, rc;

	if (dp_meta_off > dp_metadata) {
		/* dp metadata is present */
		i = 0;
		while (&dp_metadata[i] < dp_meta_off) {
			rc = host_prop_check_val_json(dp_metadata[i].key);
			if (rc < 0) {
				return -1;
			}
			rc = host_prop_check_val_json(dp_metadata[i].value);
			if (rc < 0) {
				return -1;
			}
			i++;
		}
		prop->dp_meta = dp_metadata;
	}
	return 0;
}

#ifdef AYLA_HOST_PROP_ACK_SUPPORT
/*
 * Process ack TLV to find out ack_id, ack_status
 * and ack_message.
 */
static int data_tlv_process_ack(struct ayla_tlv *tlv)
{
	size_t rlen = tlv->len;
	size_t tlen = 0;
	u8 err = 0;
	void *vp;

	tlv = (struct ayla_tlv *)((char *)(tlv + 1));

	while (rlen > 0) {
		if (rlen < sizeof(*tlv)) {
			log_put("tlv_rx TLV len err rlen %u", rlen);
			err = AERR_LEN_ERR;
			break;
		}
		rlen -= sizeof(*tlv);
		tlen = tlv->len;
		if (rlen < tlen) {
			log_put("tlv_rx TLV len err type %d tlen %u rlen %u",
			    tlv->type, tlen, rlen);
			err = AERR_LEN_ERR;
			break;
		}
		vp = tlv + 1;
		rlen -= tlen;

		switch (tlv->type) {
		case ATLV_UTF8:
			if (tlen > PROP_ACK_ID_LEN) {
				err = AERR_LEN_ERR;
				break;
			}
			memcpy(prop_ack.id, vp, tlen);
			prop_ack.id[tlen] = '\0';
			break;
		case ATLV_INT:
			if (tlv->len > sizeof(s32)) {
				err = AERR_BAD_VAL;
				break;
			}
			prop_ack.msg = (s32)get_ua_be32(vp);
			break;
		case ATLV_ERR:
			if (tlv->len != 1) {
				err = AERR_LEN_ERR;
				break;
			}
			prop_ack.status = *(u8 *)vp;
			break;
		default:
			err = AERR_INVAL_TLV;
		}
		tlv = (struct ayla_tlv *)((char *)(vp + tlen));
	}
	return err;
}
#endif

/*
 * Handle incoming TLV message.
 *
 * Queue TLV updates thru the client.
 * Keep the last N TLV updates for display on the server.
 */
static void data_tlv_recv(const void *buf, size_t len)
{
	struct prop prop;
	const struct ayla_cmd *cmd = buf;
	size_t rlen = len;
	size_t tlen;
	struct ayla_tlv *tlv;
	u32 continuation = 0;
	u32 uval;
	s32 sval;
	void *vp;
	char name[PROP_NAME_LEN] = { '\0' };
#ifdef AYLA_HOST_PROP_FILE_SUPPORT
	struct ayla_tlv *loc_tlv = NULL;
#endif
	struct ayla_tlv *off_tlv = NULL;
	struct ayla_tlv *len_tlv = NULL;
#ifdef AYLA_HOST_PROP_BATCH_SUPPORT
	struct ayla_tlv *batch_id_tlv = NULL;
	struct ayla_tlv *batch_end_tlv = NULL;
	unsigned long long timestamp = 0;
#endif
#ifdef AYLA_HOST_PROP_MSG_SUPPORT
	enum ayla_tlv_type msg_type = ATLV_LOC;
#endif
	u8 eof = 0;
	u8 err = 0;
	u16 req_id;
	u8 node_mask = 0;
	u8 nak = 0;
	u8 features_given = 0;
	u8 features = 0;
	u8 dp_meta_recvd = 0;
	int rc;

	ASSERT(rlen >= sizeof(*cmd));
	rlen -= sizeof(*cmd);
	tlv = (struct ayla_tlv *)(cmd + 1);
	req_id = get_ua_be16(&cmd->req_id);

	memset(&prop, 0, sizeof(prop));
	prop.name = name;

	switch (cmd->opcode) {
	case AD_SEND_PROP_RESP:
		if (rlen == 0) {
			prop_req_get_resp(req_id, NULL, continuation, 0);
			return;
		}
		break;
	case AD_SEND_TLV:
		break;
#ifdef AYLA_HOST_PROP_BATCH_SUPPORT
	case AD_BATCH_SEND:
#endif
#ifdef AYLA_HOST_PROP_FILE_SUPPORT
	case AD_DP_CREATE:
	case AD_DP_SEND:
	case AD_DP_FETCHED:
	case AD_DP_REQ:
		break;
	case AD_DP_STOP:
		dp_evt_notified = 0;
		prop_abort_dp_operation(req_id);
		return;
#endif
	case AD_NAK:
		nak = 1;
		break;
	case AD_REQ_TLV:
		if (!rlen) {
			data_tlv_set_dev_ads_busy();
			err = host_prop_get_to_device(req_id);
		}
		break;
	case AD_LISTEN_ENB:
		host_prop_enable_listen();
		return;
	case AD_SEND_TLV_V1:
	default:
		data_tlv_internal_nak(req_id, AERR_INVAL_OP);
		return;
	}

	while (rlen > 0) {
		if (rlen < sizeof(*tlv)) {
			log_put("tlv_rx TLV len err rlen %u", rlen);
			err = AERR_LEN_ERR;
			break;
		}
		rlen -= sizeof(*tlv);
		tlen = tlv->len;
		if (rlen < tlen) {
			log_put("tlv_rx TLV len err type %d tlen %u rlen %u",
			    tlv->type, tlen, rlen);
			err = AERR_LEN_ERR;
			break;
		}
		vp = tlv + 1;
		rlen -= tlen;

		if (dp_meta_recvd && tlv->type != ATLV_UTF8) {
			err = AERR_DPMETA;
			break;
		}

		switch (tlv->type) {
		case ATLV_NAME:
			if (tlen > sizeof(name) - 1) {
				memcpy(name, vp, sizeof(name) - 1);
				name[sizeof(name) - 1] = '\0';
			} else {
				memcpy(name, vp, tlen);
				name[tlen] = '\0';
			}
			if (!prop_name_valid(name)) {
				err = AERR_INVAL_NAME;
				break;
			}
			prop.fmt_flags = 0;
			prop.type = ATLV_INVALID;
			prop.val = NULL;
			break;
		case ATLV_FORMAT:
			prop.fmt_flags = *(u8 *)vp;
			break;
		case ATLV_INT:
		case ATLV_CENTS:
			switch (tlv->len) {
			case 1:
				sval = *(s8 *)vp;
				break;
			case 2:
				sval = (s16)get_ua_be16(vp);
				break;
			case 4:
				sval = (s32)get_ua_be32(vp);
				break;
			default:
				err = AERR_LEN_ERR;
				break;
			}
			prop.type = tlv->type;
			prop.val = &sval;
			prop.len = sizeof(sval);
			break;
		case ATLV_CONT:
			if (tlen != sizeof(continuation)) {
				err = AERR_LEN_ERR;
				break;
			}
			continuation = get_ua_be32(vp);
			break;

		case ATLV_UINT:
			err = get_ua_with_len(vp, tlv->len, &uval);
			prop.type = tlv->type;
			prop.val = &uval;
			prop.len = sizeof(uval);
			break;

		case ATLV_BOOL:
			if (tlv->len != 1) {
				err = AERR_LEN_ERR;
				break;
			}
			uval = *(u8 *)vp;
			if (uval > 1) {
				err = AERR_INVAL_TLV;
				break;
			}
			prop.type = tlv->type;
			prop.val = &uval;
			prop.len = sizeof(uval);
			break;

		case ATLV_BIN:
		case ATLV_SCHED:
		case ATLV_UTF8:
			if (dp_meta_recvd) {
				/* Parse the dp metadata value */
				dp_meta_recvd = 0;
				if (!tlv->len ||
				    tlv->len > PROP_DPMETA_VAL_LEN) {
					err = AERR_DPMETA;
					break;
				}
				memcpy(dp_meta_off->value, vp, tlv->len);
				dp_meta_off->value[tlv->len] = '\0';
				dp_meta_off++;
				break;
			}
			prop.type = tlv->type;
			prop.val = vp;
			prop.len = tlv->len;
			break;
#ifdef AYLA_HOST_PROP_FILE_SUPPORT
		case ATLV_LOC:
			loc_tlv = tlv;
			break;
#endif
		case ATLV_LEN:
			len_tlv = tlv;
			break;
		case ATLV_OFF:
			off_tlv = tlv;
			break;
		case ATLV_NODES:
			/* bit mask length limit is 1 */
			if (tlv->len != 1) {
				err = AERR_LEN_ERR;
				break;
			}
			node_mask = *(u8 *)vp;
			if (!node_mask) {
				/* dest mask can't be 0 */
				err = AERR_INVAL_TLV;
			}
			break;
		case ATLV_EOF:
			eof = 1;
			break;
		case ATLV_ECHO:
			prop.echo = 1;
			break;
		case ATLV_ERR:
			if (nak == 1) {
				if (tlv->len != 1) {
					break;
				}
				/* received nak from MCU */
				log_put(LOG_DEBUG "rx nak %#x for req_id %#x",
				    *(u8 *)vp, req_id);
			}
			nak = 1;   /* received AD_PROP_RESP err (feat mask) */
			break;
		case ATLV_FEATURES:
			/* bit mask length limit is 1 */
			if (tlv->len != 1) {
				err = AERR_LEN_ERR;
				break;
			}
			features_given = 1;
			features = *(u8 *)vp;
			break;
		case ATLV_DPMETA:
			/*
			 * Parse this TLV for dp metadata key
			 */
			if (!tlv->len || tlv->len > PROP_DPMETA_KEY_LEN ||
			    dp_meta_off >= dp_metadata + PROP_MAX_DPMETA ||
			    prop.name[0] != '\0') {
				err = AERR_DPMETA;
				break;
			}
			memcpy(dp_meta_off->key, vp, tlv->len);
			dp_meta_off->key[tlv->len] = '\0';
			dp_meta_recvd = 1;
			break;
#ifdef AYLA_HOST_PROP_ACK_SUPPORT
		case ATLV_ACK_ID:
			err = data_tlv_process_ack(tlv);
			if (err) {
				err = AERR_INVAL_ACK;
				break;
			}
			prop.ack = &prop_ack;
			prop.type = ATLV_ACK_ID;
			break;
#endif
#ifdef AYLA_HOST_PROP_MSG_SUPPORT
		case ATLV_MSG_TYPE:
			if (tlv->len != 1) {
				err = AERR_LEN_ERR;
				break;
			}
			msg_type = (enum ayla_tlv_type)*(u8 *)TLV_VAL(tlv);
			break;
#endif
#ifdef AYLA_HOST_PROP_BATCH_SUPPORT
		case ATLV_BATCH_ID:
			batch_id_tlv = tlv;
			break;
		case ATLV_BATCH_END:
			batch_end_tlv = tlv;
			break;
		case ATLV_TIME_MS:
			if (tlv->len != sizeof(u64)) {
				err = AERR_INVAL_TLV;
				break;
			}
			timestamp = get_ua_be64(TLV_VAL(tlv));
			break;
#endif
		default:
			err = AERR_UNK_TYPE;
			break;
		}
		switch (cmd->opcode) {
		case AD_SEND_TLV:
		case AD_BATCH_SEND:
			if (nak || err || rlen) {
				break;
			}
#ifdef AYLA_HOST_PROP_ACK_SUPPORT
			if (prop.type == ATLV_ACK_ID) {
				/*
				 * Work-around for host_lib bug.
				 * The host should not send a value with
				 * the ACK ID.  Ignore if it does.
				 * Compatible with legacy production agents.
				 */
				prop.val = NULL;
				prop.len = 0;
				prop_ack.src = node_mask;
				err = host_prop_send(req_id, &prop, node_mask);
				break;
			}
#endif
send_to_dests:
			if (!prop.val) {
				err = AERR_INVAL_TLV;
				break;
			}
			err = data_tlv_recv_tlv(req_id, &prop,
			    off_tlv, len_tlv);
			if (err) {
				break;
			}
			if (eof || data_tlv_next_off == data_tlv_tot_size ||
			    (!off_tlv && !len_tlv)) {
				prop.val = data_tlv_pkt;
				prop.len = data_tlv_next_off;
				data_tlv_pkt[data_tlv_next_off] = '\0';
				rc = data_tlv_check_dp_metadata(&prop);
				if (rc < 0) {
					err = AERR_DPMETA;
					break;
				}
				if (prop.type == ATLV_UTF8) {
					rc = host_prop_check_val_json(prop.val);
					if (rc) {
						log_put(LOG_WARN
						     "%s: tlv UTF8 check rc %d",
						     __func__, rc);
					}
					if (rc == -1) {
						err = AERR_PROP_LEN;
						break;
					} else if (rc == -2) {
						err = AERR_BAD_VAL;
						break;
					}
				}
				data_tlv_set_dev_ads_busy();
				if (cmd->opcode == AD_SEND_PROP_RESP) {
					err = prop_req_get_resp(req_id, &prop,
					    continuation, 0);
					if (err && err != AERR_ADS_BUSY) {
						data_tlv_clear_dev_ads_busy();
					}
				} else if (cmd->opcode == AD_BATCH_SEND) {
#ifdef AYLA_HOST_PROP_BATCH_SUPPORT
					if (!batch_id_tlv ||
					    batch_id_tlv->len != sizeof(u16)) {
						err = AERR_INVAL_TLV;
						break;
					}
					err = prop_send_batch(req_id, &prop,
					    get_ua_be16(TLV_VAL(batch_id_tlv)),
					    batch_end_tlv != NULL,
					    timestamp);
#else
					err = AERR_INVAL_OP;
#endif
				} else {
					err = host_prop_send(req_id,
					    &prop, node_mask);
				}
			}
			break;
		case AD_SEND_PROP_RESP:
			if (err || rlen) {
				break;
			}
			if (nak) {
				if (features_given) {
					mcu_feature_mask = features |
					    mcu_feature_mask_min;
					log_put(LOG_INFO
					    "host features rx %x effective %x",
					    features, mcu_feature_mask);
				}
				prop_req_get_resp(req_id, NULL, 0, nak);
				break;
			}
			prop_req_resp_reset_timeout(req_id);
			goto send_to_dests;
			break;
#ifdef AYLA_HOST_PROP_FILE_SUPPORT
		case AD_DP_CREATE:
			if (nak || err || rlen) {
				break;
			}
			dp_evt_notified = 0;
			rc = data_tlv_check_dp_metadata(&prop);
			if (rc < 0) {
				err = AERR_DPMETA;
				break;
			}
			prop.type = msg_type;
			data_tlv_set_dev_ads_busy();
			err = prop_get_dp_loc(req_id, &prop);
			break;
		case AD_DP_SEND:
			if (nak || err || rlen) {
				break;
			}
			if (!prop.val) {
				err = AERR_INVAL_TLV;
				break;
			}
			err = data_tlv_dp_rx(req_id, &prop,
			    loc_tlv, off_tlv, len_tlv, eof);
			if (client_check_np_event() && !dp_evt_notified) {
				/*
				 * If an ANS notification is pending
				 * send an DP_NOTIFY to the MCU
				 */
				data_tlv_send_prop_notification();
			}
			break;
		case AD_DP_FETCHED:
			if (nak || err || rlen) {
				break;
			}
			if (!loc_tlv) {
				log_put(LOG_WARN
				    "missing or bad loc TLV");
				err = AERR_INVAL_DP;
				break;
			}
			data_tlv_set_dev_ads_busy();
			err = prop_set_dp_fetched(req_id, (char *)(loc_tlv + 1),
			    loc_tlv->len);
			break;
		case AD_DP_REQ:
			if (nak || err || rlen) {
				break;
			}
			if (!loc_tlv) {
				log_put(LOG_WARN
				    "missing or bad loc TLV");
				err = AERR_INVAL_DP;
				break;
			}
			if (host_prop_is_busy(NULL, NULL)) {
				err = AERR_ADS_BUSY;
				break;
			}
			data_tlv_set_dev_ads_busy();
			if (off_tlv && off_tlv->len ==
			    sizeof(data_tlv_next_off)) {
				data_tlv_next_off = get_ua_be32(off_tlv + 1);
			} else {
				data_tlv_next_off = 0;
			}
			err = prop_get_dp_req(req_id, msg_type,
			    (char *)TLV_VAL(loc_tlv),
			    loc_tlv->len, data_tlv_next_off);
			if (err) {
				data_tlv_clear_dev_ads_busy();
			}
			break;
#endif /* AYLA_HOST_PROP_DP_SUPPORT */
		case AD_REQ_TLV:
			if (nak || err || rlen) {
				break;
			}
			data_tlv_set_dev_ads_busy();
			err = host_prop_get(req_id, prop.name);
			break;
		default:
			break;
		}
		tlv = (struct ayla_tlv *)((char *)(vp + tlen));
	}
	if (err) {
		if (cmd->opcode == AD_DP_SEND) {
#ifdef AYLA_HOST_PROP_DP_SUPPORT
			prop_abort_dp_operation(req_id);
#endif
		} else if (cmd->opcode == AD_SEND_TLV ||
		    cmd->opcode == AD_SEND_PROP_RESP) {
			data_tlv_next_off_val = 0;
			dp_meta_off = dp_metadata;
		}
		data_tlv_internal_nak(req_id, err);
	}
}

/*
 * Reset the next expected offset for a datapoint put
 */
void data_tlv_reset_offset(void)
{
	dp_meta_off = dp_metadata;
	data_tlv_next_off_val = 0;
	memset(dp_metadata, 0, sizeof(dp_metadata));
}

/*
 * Clear ADS Busy. Setup a confirmation message for MCU if it needs one.
 */
void data_tlv_clear_ads(u16 req_id, u8 send_confirmation)
{
	if ((mcu_feature_mask & MCU_DATAPOINT_CONFIRM) &&
	    send_confirmation && !confirm_needed) {
		confirm_req_id = req_id;
		confirm_needed = 1;
		hp_buf_callback_pend(data_tlv_confirmation_cb);
	}
	data_tlv_clear_dev_ads_busy();
}

/*
 * Process an incoming message from MCU. Route it either as data
 * or config.
 * Return -1 if bad cmd.
 * Return -2 if len err.
 */
int data_tlv_process_mcu_pkt(u8 *data_ptr, size_t recv_len)
{
	struct ayla_cmd *cmd;

	cmd = (struct ayla_cmd *)data_ptr;
	switch (cmd->protocol) {
	case ASPI_PROTO_CMD:
		if (recv_len < sizeof(*cmd)) {
			return -2;
		}
		conf_tlv_msg_recv(data_ptr, recv_len);
		break;
	case ASPI_PROTO_DATA:
		if (recv_len < sizeof(*cmd)) {
			return -2;
		}
		data_tlv_recv(data_ptr, recv_len);
		break;
	case ASPI_PROTO_PING:
		mcu_dev->ping(data_ptr, recv_len + 1);
		break;
	default:
		/* unknown data packet type. drop */
		return -1;
	}
	return 0;
}

/*
 * Init for callbacks
 */
void data_tlv_init(void)
{
	data_tlv_wifi_init();
}
