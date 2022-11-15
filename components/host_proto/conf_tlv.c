/*
 * Copyright 2011-2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/endian.h>
#include <ayla/tlv.h>
#include <ayla/conf_token.h>
#include <ayla/conf.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/endian.h>
#include <ayla/clock.h>
#include <ayla/utf8.h>
#include <ayla/timer.h>
#include <ayla/tlv_access.h>
#include <ada/err.h>
#include <ada/ada_conf.h>
#include <adb/adb.h>
#include <adb/al_bt.h>
#include <adw/wifi.h>
#include <net/net.h>
#include <libapp/libapp_ota.h>
#include <host_proto/mcu_dev.h>
#include "conf_tlv.h"
#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "hp_buf_tlv.h"
#include "host_proto_int.h"
#include "host_proto_ota.h"

static void conf_tlv_ota_ready(void);
static const struct libapp_app_ota_ops conf_tlv_module_ota_ops = {
	.ota_ready = conf_tlv_ota_ready
};

static u16 conf_tlv_req_id;		/* last request ID used */
static u8 conf_tlv_reset_factory;
static struct timer conf_tlv_reset_timer;

u16 conf_tlv_next_req_id(void)
{
	return ++conf_tlv_req_id;
}

/*
 * Set command fields in the buffer.
 * The buffer is guaranteed to be large enough to hold a command.
 */
static void conf_tlv_cmd_set(struct hp_buf *bp,
	enum ayla_data_op op, u16 req_id)
{
	hp_buf_tlv_cmd_set(bp, ASPI_PROTO_CMD, op, req_id);
}

/*
 * Set command fields for a request in the buffer.
 * Returns the request ID used.
 */
static u16 conf_tlv_cmd_req_set(struct hp_buf *bp, enum ayla_cmd_op op)
{
	u16 req_id = conf_tlv_next_req_id();

	conf_tlv_cmd_set(bp, op, req_id);
	return req_id;
}

/*
 * Send NAK
 */
static void conf_tlv_nak(struct hp_buf *bp, be16 req_id, u8 err)
{
	conf_tlv_cmd_set(bp, ACMD_NAK, req_id);
	hp_buf_tlv_append_u8(bp, ATLV_ERR, err);
	mcu_dev->enq_tx(bp);
}

/*
 * Send NAK after allocating buffer.
 */
static void conf_tlv_nak_alloc(be16 req_id, u8 err)
{
	struct hp_buf *bp;

	bp = hp_buf_alloc(0);
	if (!bp) {
		return;
	}
	conf_tlv_nak(bp, req_id, err);
}

/*
 * Put integer TLVs in host order for conf_entry_set().
 *
 * If a 4-byte Boolean is received, it is not swapped, to be compatible with bc.
 * Booleans are assumed to be 1-byte by bc, and we follow that here.
 */
static const struct ayla_tlv *conf_tlv_swap(const struct ayla_tlv *tlv,
	struct ayla_tlv *new_tlv)
{
	switch (tlv->type) {
	case ATLV_INT:
	case ATLV_CENTS:
	case ATLV_UINT:
		switch (tlv->len) {
		case sizeof(s16):
			*new_tlv = *tlv;	/* struct copy type and len */
			*(s16 *)TLV_VAL(new_tlv) = get_ua_be16(TLV_VAL(tlv));
			return new_tlv;
		case sizeof(u32):
			*new_tlv = *tlv;	/* struct copy type and len */
			*(s32 *)TLV_VAL(new_tlv) = get_ua_be32(TLV_VAL(tlv));
			return new_tlv;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return tlv;
}

/*
 * Convert UTF-8 buffer to config tokens.
 */
static int conf_tlv_name_to_tokens(enum conf_token *tk, unsigned int max_tokens,
		const char *in, size_t in_len)
{
	u32 wchar[CONF_PATH_MAX];
	int plen;
	unsigned int i;

	plen = utf8_gets(wchar, ARRAY_LEN(wchar), (u8 *)in, in_len);
	if (plen < 0) {
		return -1;
	}
	ASSERT(plen < ARRAY_LEN(wchar));
	if (plen > max_tokens) {
		return -1;
	}
	for (i = 0; i < plen; i++) {
		tk[i] = (enum conf_token)wchar[i];
	}
	return plen;
}

/*
 * Convert a configuration system error code to a host-protocol error code.
 */
static int conf_tlv_err_to_mcu_err(enum conf_error err)
{
	const u8 err_table[] = {
		[CONF_ERR_NONE] = 0,
		[CONF_ERR_LEN] = AERR_PROP_LEN,
		[CONF_ERR_UTF8] = AERR_INVAL_FMT,
		[CONF_ERR_PATH] = AERR_UNK_VAR,
		[CONF_ERR_TYPE] = AERR_INVAL_TLV,
		[CONF_ERR_RANGE] = AERR_INVAL_REQ,
		[CONF_ERR_PERM] = AERR_INVAL_REQ,
	};

	if (err < 0 || err >= ARRAY_LEN(err_table)) {
		return AERR_INTERNAL;
	}
	if (err && !err_table[err]) {
		return AERR_INTERNAL;
	}
	return err_table[err];
}

/*
 * Get config or status value for host interface.
 *
 * Note: this uses non-ADA interfaces into the libayla configuration code.
 */
static int conf_tlv_get(u16 req_id, const struct ayla_tlv *tlv)
{
	struct conf_state *state = &conf_state;
	enum conf_error err;
	struct hp_buf *bp;
	enum conf_token tk[CONF_PATH_MAX];
	int tk_count;
	size_t init_len;

	bp = hp_buf_alloc(0);
	if (!bp) {
		return AERR_INTERNAL;
	}

	tk_count = conf_tlv_name_to_tokens(tk, ARRAY_LEN(tk),
	    TLV_VAL(tlv), tlv->len);
	if (tk_count < 0) {
		conf_tlv_nak(bp, req_id, AERR_UNK_VAR);
		return 0;
	}

	/*
	 * Form command and append name TLV.
	 */
	conf_tlv_cmd_set(bp, ACMD_RESP, req_id);
	hp_buf_tlv_append(bp, tlv->type, TLV_VAL(tlv), tlv->len);

	conf_lock();
	state->error = CONF_ERR_NONE;
	state->next = (u8 *)bp->payload + bp->len;
	init_len = hp_buf_tlv_space(bp);
	state->rlen = init_len;

	/*
	 * Put value in the response.
	 */
	err = conf_entry_get(CONF_OP_SRC_MCU, tk, tk_count);
	bp->len += init_len - state->rlen;
	conf_unlock();

	/*
	 * On error, send NAK.
	 */
	if (err) {
		conf_log(LOG_ERR "conf_tlv_get: conf_error %u", err);
		conf_tlv_nak(bp, req_id, conf_tlv_err_to_mcu_err(err));
		return 0;
	}

	/*
	 * Send response.
	 */
	mcu_dev->enq_tx(bp);
	return 0;
}

/*
 * Set config value for host interface.
 * Returns 0 on success, otherwise MCU error code.
 *
 * There's no response except on error.
 *
 * Note: this uses non-ADA interfaces into the libayla configuration code.
 */
static int conf_tlv_set(u16 req_id, u8 commit,
	const struct ayla_tlv *name_tlv, const struct ayla_tlv *val_tlv)
{
	enum conf_error err;
	enum conf_token tk[CONF_PATH_MAX];
	int tk_count;
	struct {
		struct ayla_tlv tlv;
		u32 val;
	} new_tlv;

	tk_count = conf_tlv_name_to_tokens(tk, ARRAY_LEN(tk),
	    TLV_VAL(name_tlv), name_tlv->len);
	if (tk_count < 0) {
		return AERR_UNK_VAR;
	}

	if (commit) {
		/* TODO: commit just changed tables? */
		conf_commit();
		err = 0;
	} else {
		val_tlv = conf_tlv_swap(val_tlv, &new_tlv.tlv);
		err = conf_entry_set(CONF_OP_SRC_MCU, tk, tk_count,
		    (struct ayla_tlv *)val_tlv);
	}

	/*
	 * On error, send NAK.
	 */
	if (err) {
		conf_log(LOG_ERR "conf_tlv_set: commit %u conf_error %u",
		    commit, err);
		conf_tlv_nak_alloc(req_id, conf_tlv_err_to_mcu_err(err));
		return 0;
	}
	return err;
}

static enum conf_error conf_get_state_path_val(struct conf_state *state)
{
	conf_resp(ATLV_CONF, state->path, 2);
	return conf_sys_conf_entry.get(CONF_OP_SRC_MCU, state->path + 1, 1);
}

/*
 * Send the time information to the MCU.
 * time + timezone_valid + timezone (if valid) +
 * dst_valid + dst_active (if valid) + dst_change (if valid)
 */
void conf_send_mcu_time_info(void)
{
	/* Estimated length for buf based on the sizes of time info */
	u8 length_estimate = 56;
	struct conf_state *state = &conf_state;
	struct hp_buf *bp;

	bp = hp_buf_alloc(length_estimate);
	if (!bp) {
		return;
	}
	conf_tlv_cmd_req_set(bp, ACMD_CONF_UPDATE);
	state->error = CONF_ERR_NONE;
	state->next = (u8 *)bp->payload + bp->len;
	state->rlen = length_estimate - bp->len;

	state->path[0] = CT_sys;
	state->path[1] = CT_time;
	conf_get_state_path_val(state);
	state->path[1] = CT_timezone_valid;
	conf_get_state_path_val(state);
	if (timezone_info.valid) {
		state->path[1] = CT_timezone;
		conf_get_state_path_val(state);
	}
	state->path[1] = CT_dst_valid;
	conf_get_state_path_val(state);
	if (daylight_info.valid) {
		state->path[1] = CT_dst_active;
		conf_get_state_path_val(state);
		state->path[1] = CT_dst_change;
		conf_get_state_path_val(state);
	}
	bp->len = length_estimate - state->rlen;
	mcu_dev->enq_tx(bp);
}

/*
 * Log a message from the host MCU.
 * Severities from the MCU should match our log_sev enum.
 */
static u8 conf_tlv_host_log(const void *buf, size_t len)
{
	s32 val;
	u32 code;
	u8 *cp;
	int tlen;
	ssize_t slen;
	enum log_sev sev = LOG_SEV_INFO;
	struct ayla_tlv *tlv = NULL;
	char msg[TLV_MAX_LEN + 1];
	int err;

	/*
	 * Handle severity TLV if present.
	 */
	err = tlv_getp(&tlv, ATLV_INT, (void *)buf, len);
	if (err != AERR_TLV_MISSING) {
		if (err) {
			return err;
		}
		if (tlv_s32_get(&val, tlv)) {
			return AERR_LEN_ERR;
		}
		if (val >= 0 && val < LOG_SEV_LIMIT) {
			sev = val;	/* ignore out-of-range severities */
		}
	}

	/*
	 * Handle the log message.
	 */
	err = tlv_getp(&tlv, ATLV_UTF8, (void *)buf, len);
	if (err) {
		return err;
	}
	tlen = tlv_utf8_get(msg, sizeof(msg), tlv);
	if (tlen < 0) {
		return AERR_LEN_ERR;	/* cannot happen */
	}

	/*
	 * Validate that the log message is UTF-8.
	 * Change any unprintable ASCII characters to '.'.
	 */
	cp = (u8 *)msg;
	tlen = tlv->len;
	while (tlen) {
		slen = utf8_decode(cp, tlen, &code);
		if (slen <= 0 || slen > tlen) {
			return AERR_BAD_VAL;
		}
		if (code < 0x20 || code == 0x7f) {
			memset(cp, '.', slen);
		}
		tlen -= slen;
		cp += slen;
	}

	/*
	 * Log the message.
	 */
	log_put_mod_sev(MOD_LOG_HOST, sev, "%s", msg);
	return 0;
}

/*
 * Handle receive of ACMD_GET_CONF or ACMD_GET_STAT message.
 */
static int conf_tlv_recv_get(u16 req_id,
		const struct ayla_tlv *tlv, size_t in_len)
{
	int err = 0;
	size_t len;

	for (len = in_len; len > 0; len -= tlv->len, tlv = TLV_NEXT(tlv)) {
		if (len < sizeof(*tlv)) {
			log_put(LOG_ERR "tlv_rx TLV err 1 len %zu", len);
			err = AERR_LEN_ERR;
			break;
		}
		len -= sizeof(*tlv);
		if (len < tlv->len) {
			log_put(LOG_ERR
			    "tlv_rx TLV err 2 type %d tlen %u len %zu",
			    tlv->type, tlv->len, len);
			err = AERR_LEN_ERR;
			break;
		}

		if (tlv->type != ATLV_CONF) {
			err = AERR_INVAL_TLV;
			break;
		}
		err = conf_tlv_get(req_id, tlv);
		if (err) {
			break;
		}
	}
	return err;
}

/*
 * Handle receive fo ACMD_SET message.
 * Loop through all name/value pairs to set them, repeat for commit.
 */
static int conf_tlv_recv_set(u16 req_id,
		const struct ayla_tlv *first_tlv, size_t in_len)
{
	const struct ayla_tlv *tlv;
	const struct ayla_tlv *name_tlv = NULL;
	size_t len;
	u8 commit;
	int err = 0;

	for (commit = 0; commit < 2; commit++) {
		tlv = first_tlv;
		for (len = in_len; len > 0;
		    len -= tlv->len, tlv = TLV_NEXT(tlv)) {
			if (len < sizeof(*tlv)) {
				log_put(LOG_ERR "tlv_rx TLV err 1 len %zu",
				   len);
				return AERR_LEN_ERR;
			}
			len -= sizeof(*tlv);
			if (len < tlv->len) {
				log_put(LOG_ERR
				    "tlv_rx TLV err 2 type %d tlen %u len %zu",
				    tlv->type, tlv->len, len);
				return AERR_LEN_ERR;
			}
			if (tlv->type == ATLV_CONF) {
				name_tlv = tlv;
				continue;
			}
			if (!name_tlv) {
				return AERR_INVAL_TLV;
			}
			err = conf_tlv_set(req_id, commit, name_tlv, tlv);
			if (err) {
				return err;
			}
			name_tlv = NULL;
		}
	}
	return 0;
}

/*
 * Handle receiving a Wi-Fi onboarding message.
 *
 * This starts BLE onboarding for the specified time.
 * TLV may be integer or unsigned integer.
 */
static int conf_tlv_wifi_onboard_rx(const void *buf, size_t len)
{
	struct ayla_tlv *tlv;
	u32 val;
	int err;

	err = tlv_getp(&tlv, ATLV_INT, (void *)buf, len);
	if (err) {
		return err;
	}
	if (tlv_u32_get(&val, tlv) || !val || val >= UINT_MAX / 1000) {
		return AERR_BAD_VAL;
	}
	al_bt_wifi_timeout_set(val * 1000);
	return 0;
}

/*
 * Handle incoming configuration message.
 */
void conf_tlv_msg_recv(const void *buf, size_t len)
{
	const struct ayla_cmd *cmd = buf;
	size_t rlen;
	const struct ayla_tlv *tlv;
	u8 err = 0;
	u16 req_id;

	/*
	 * Ignore short messages.
	 */
	if (len < sizeof(*cmd)) {
		return;
	}
	req_id = get_ua_be16(&cmd->req_id);
	tlv = (struct ayla_tlv *)(cmd + 1);
	rlen = len - sizeof(*cmd);

	switch (cmd->opcode) {
	case ACMD_GET_CONF:
	case ACMD_GET_STAT:
		err = conf_tlv_recv_get(req_id, tlv, rlen);
		break;
	case ACMD_SET_CONF:
		err = conf_tlv_recv_set(req_id, tlv, rlen);
		break;
	case ACMD_SAVE_CONF:
		err = conf_save(conf_state.conf_cur);
		if (err != CONF_ERR_NONE) {
			conf_log(LOG_ERR "saving startup err %d", err);
			err = 0;	/* TODO */
		}
		conf_log(LOG_INFO "config saved by MCU");
		break;

	case ACMD_LOAD_FACTORY:
		conf_tlv_reset_factory = 1;
		/* fall-through */
	case ACMD_LOAD_STARTUP:
		host_proto_timer_set(&conf_tlv_reset_timer, 50);
		break;

	case ACMD_OTA_CMD:
		libapp_ota_apply();
		break;

	case ACMD_LOG:
		err = conf_tlv_host_log(cmd, len);
		break;

	case ACMD_MCU_OTA:
	case ACMD_MCU_OTA_STAT:
		err = host_proto_ota_rx(buf, len);
		break;

	case ACMD_WIFI_JOIN:
		err = adw_wifi_join_rx((void *)buf, len);
		break;

	case ACMD_WIFI_DELETE:
		err = adw_wifi_delete_rx((void *)buf, len);
		break;

	case ACMD_WIFI_ONBOARD:
		err = conf_tlv_wifi_onboard_rx((void *)buf, len);
		break;

	default:
		err = AERR_INVAL_OP;
		break;
	}
	if (err) {
		conf_tlv_nak_alloc(req_id, err);
	}
}

static void conf_tlv_reset_timeout(struct timer *tm)
{
	log_put(LOG_INFO "host requests %sreset",
	    conf_tlv_reset_factory ? "factory " : "");
	ada_conf_reset(conf_tlv_reset_factory);
}

/*
 * Send reset command to MCU.
 */
static void conf_tlv_reset_cb(struct hp_buf *bp)
{
	conf_tlv_cmd_req_set(bp, ACMD_HOST_RESET);
	mcu_dev->enq_tx(bp);
}

void host_proto_reset_send(void)
{
	hp_buf_callback_pend(conf_tlv_reset_cb);
}

/*
 * Send OTA ready message to MCU.
 */
static void conf_tlv_ota_ready_cb(struct hp_buf *bp)
{
	conf_tlv_cmd_req_set(bp, ACMD_OTA_STAT);
	mcu_dev->enq_tx(bp);
}

static void conf_tlv_ota_ready(void)
{
	hp_buf_callback_pend(conf_tlv_ota_ready_cb);
}

void conf_tlv_msg_init(void)
{
	ayla_timer_init(&conf_tlv_reset_timer, conf_tlv_reset_timeout);
	libapp_ota_register(&conf_tlv_module_ota_ops);
}
