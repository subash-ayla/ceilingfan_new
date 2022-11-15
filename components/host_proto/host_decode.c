/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <string.h>
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/ayla_proto_mcu.h>
#include <ayla/ayla_spi_mcu.h>
#include <ayla/conf.h>
#include <ayla/tlv.h>
#include <ayla/endian.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/tlv_access.h>
#include <ayla/utf8.h>
#include "host_decode.h"

#define HOST_DECODE_MAX_STR	40	/* max length of string to show */
#define HOST_DECODE_MAX_LEN	200	/* max length of decode */
#define HOST_DECODE_LINE_LEN	80	/* line split width */
#define HOST_DECODE_STR_LINE_LEN (HOST_DECODE_LINE_LEN - 30)
#define HOST_DECODE_STR_BUF_LEN 256
#define HOST_DECODE_TLV_LEN	16	/* display TLV len longer than this */

static const char *host_decode_proto[] = {
	[ASPI_PROTO_CMD] = "cmd",
	[ASPI_PROTO_DATA] = "data",
	[ASPI_PROTO_PING] = "ping",
	[ASPI_PROTO_LOG] = "log",
};

static const char *host_decode_cmd[] = {
	[ACMD_NOP] =		"NOP",
	[ACMD_RESP] =		"response",
	[ACMD_GET_CONF] =	"get_conf",
	[ACMD_SET_CONF] =	"set_conf",
	[ACMD_SAVE_CONF] =	"save_conf",
	[ACMD_GET_STAT] =	"get_stat",
	[ACMD_NAK] =		"nak",
	[ACMD_LOAD_STARTUP] =	"load_startup",
	[ACMD_LOAD_FACTORY] =	"load_factory",
	[ACMD_OTA_STAT] =	"ota_stat",
	[ACMD_OTA_CMD] =	"ota_cmd",
	[ACMD_COMMIT] =		"commit",
	[ACMD_LOG] =		"log",
	[ACMD_MCU_OTA] =	"mcu_ota",
	[ACMD_MCU_OTA_LOAD] =	"mcu_ota_load",
	[ACMD_MCU_OTA_STAT] =	"mcu_ota_stat",
	[ACMD_MCU_OTA_BOOT] =	"mcu_ota_boot",
	[ACMD_CONF_UPDATE] =	"conf_update",
	[ACMD_WIFI_JOIN] =	"wifi_join",
	[ACMD_WIFI_DELETE] =	"wifi_delete",
	[ACMD_HOST_RESET] =	"host_reset",
	[ACMD_WIFI_ONBOARD] =	"wifi_onboard",
};

static const char *host_decode_data[] = {
	[AD_SEND_TLV_V1] =	"send_tlv_v1",
	[AD_REQ_TLV] =		"req_tlv",
	[AD_RECV_TLV] =		"recv_tlv",
	[AD_NAK] =		"nak",
	[AD_SEND_PROP] =	"send_prop",
	[AD_SEND_PROP_RESP] =	"send_prop_resp",
	[AD_SEND_NEXT_PROP] =	"send_next_prop",
	[AD_SEND_TLV] =		"send_tlv",
	[AD_DP_REQ] =		"dp_req",
	[AD_DP_RESP] =		"dp_resp",
	[AD_DP_CREATE] =	"dp_create",
	[AD_DP_FETCHED] =	"dp_fetched",
	[AD_DP_STOP] =		"dp_stop",
	[AD_DP_SEND] =		"dp_send",
	[AD_CONNECT] =		"connect",
	[AD_ECHO_FAIL] =	"echo_fail",
	[AD_LISTEN_ENB] =	"listen_enb",
	[AD_ERROR] =		"error",
	[AD_CONFIRM] =		"confirm",
	[AD_PROP_NOTIFY] =	"prop_notify",
	[AD_EVENT] =		"event",
	[AD_BATCH_SEND] =	"batch_send",
	[AD_BATCH_STATUS] =	"batch_status",
};

static const char *host_decode_err[]  = {
	[0] =			"none",
	[AERR_TIMEOUT] =	"timeout",
	[AERR_LEN_ERR] =	"len_err",
	[AERR_UNK_TYPE] =	"unk_type",
	[AERR_UNK_VAR] =	"unk_var",
	[AERR_INVAL_TLV] =	"inval_tlv",
	[AERR_INVAL_OP] =	"inval_op",
	[AERR_INVAL_DP] =	"inval_dp",
	[AERR_INVAL_OFF] =	"inval_off",
	[AERR_INVAL_REQ] =	"inval_req",
	[AERR_INVAL_NAME] =	"inval_name",
	[AERR_CONN_ERR] =	"conn_err",
	[AERR_ADS_BUSY] =	"ads_busy",
	[AERR_INTERNAL] =	"internal",
	[AERR_CHECKSUM] =	"checksum",
	[AERR_ALREADY] =	"already",
	[AERR_BOOT] =		"boot",
	[AERR_OVERFLOW] =	"overflow",
	[AERR_BAD_VAL] =	"bad_val",
	[AERR_PROP_LEN] =	"prop_len",
	[AERR_UNEXP_OP] =	"unexp_op",
	[AERR_DPMETA] =		"dpmeta",
	[AERR_INVAL_ACK] =	"inval_ack",
	[AERR_UNK_PROP] =	"unk_prop",
	[AERR_INVAL_FMT] =	"inval_fmt",
	[AERR_BUSY_RETRY] =	"busy_retry",
	[AERR_RETRY] =		"retry",
	[AERR_TIME_UNK] =	"time_unk",
	[AERR_PARTIAL] =	"partial",
	[AERR_UNK_STAT] =	"unk_stat",
	[AERR_TLV_MISSING] =	"tlv_missing",
};

static const char *host_decode_tlv_type[] = AYLA_TLV_TYPE_NAMES;

struct host_decode_context {
	const char *prefix;	/* initial prefix for log lines */
	char buf[HOST_DECODE_MAX_LEN];		/* line buffer */
	size_t off;		/* offset into line buffer for next put */
	enum ayla_data_op data_op;
	u8 line_nr;
};

static void host_decode_tlvs(struct host_decode_context *ctxt,
		const void *tlvs, size_t tlvs_len);

/*
 * Flush any buffered output for the decode.
 */
static void host_decode_flush(struct host_decode_context *ctxt)
{
	log_put_mod(MOD_LOG_IO, LOG_DEBUG "%s:%s%s",
	    ctxt->prefix, ctxt->line_nr ? " ... " : "", ctxt->buf);
	ctxt->off = 0;
	ctxt->line_nr++;
}

/*
 * Add tokens to the decode output.
 * This handles line-wraps.
 */
static void host_decode_put(struct host_decode_context *ctxt,
	const char *fmt, ...) ADA_ATTRIB_FORMAT(2, 3);

static void host_decode_put(struct host_decode_context *ctxt,
	const char *fmt, ...)
{
	ADA_VA_LIST args;
	int rc;

	if (ctxt->off > HOST_DECODE_LINE_LEN) {
		host_decode_flush(ctxt);
	}

	for (;;) {
		ADA_VA_START(args, fmt);
		rc = vsnprintf(ctxt->buf + ctxt->off,
		    sizeof(ctxt->buf) - ctxt->off, fmt, args);
		ADA_VA_END(args);
		if (rc < 0) {
			break;		/* can't happen */
		}

		/*
		 * On the first loop, where offset is non-zero,
		 * see if the new output would be better on the next line.
		 */
		if (ctxt->off && rc + ctxt->off > HOST_DECODE_LINE_LEN) {
			if (ctxt->off < sizeof(ctxt->buf)) {
				ctxt->buf[ctxt->off] = '\0';
			}
			host_decode_flush(ctxt);
			continue;
		}
		ctxt->off += rc;
		break;
	}
}

static void host_decode_lookup(struct host_decode_context *ctxt,
		const char **table, size_t table_len, unsigned int val)
{
	if (val >= table_len || table[val] == NULL) {
		host_decode_put(ctxt, "unknown(0x%x)", val);
		return;
	}
	host_decode_put(ctxt, "%s", table[val]);
}

static size_t host_decode_tokens(char *buf, size_t len, u8 *tk, size_t tklen)
{
	enum conf_token token[CONF_PATH_MAX];
	u32 wchar[CONF_PATH_MAX];
	int ntk = 0;
	unsigned int i;
	int rc;

	ntk = utf8_gets(wchar, ARRAY_LEN(wchar), tk, tklen);
	if (ntk <= 0 || ntk > ARRAY_LEN(token)) {
		rc = snprintf(buf, len, "invalid");
	} else {
		for (i = 0; i < ntk; i++) {
			token[i] = (enum conf_token)wchar[i];
		}
		rc = conf_tokens_to_str(token, ntk, buf, len);
	}
	if (rc < 0) {
		buf[0] = '\0';
		rc = 0;
	}
	return (size_t)rc;
}

static void host_decode_tlv(struct host_decode_context *ctxt,
		 const struct ayla_tlv *tlv)
{
	u32 uval;
	s32 ival;
	char sval[HOST_DECODE_STR_BUF_LEN];
	const char ellipsis[] = "...";
	const char quote[] = "\"";
	int rc;
	size_t tlen;
	size_t off;
	char saved[2];

	/*
	 * Insert type into result.
	 */
	host_decode_put(ctxt, "(");
	host_decode_lookup(ctxt,
	    host_decode_tlv_type, ARRAY_LEN(host_decode_tlv_type), tlv->type);
	host_decode_put(ctxt, ",");

	/*
	 * If the length is long, show it.
	 */
	if (tlv->len >= HOST_DECODE_TLV_LEN) {
		host_decode_put(ctxt, "len=%u,", tlv->len);
	}

	/*
	 * Insert length and/or value.
	 */
	switch (tlv->type) {
	case ATLV_ERR:
		if (tlv_u32_get(&uval, tlv)) {
			host_decode_put(ctxt, " (tlv_u32_get err)");
			break;
		}
		host_decode_lookup(ctxt,
		    host_decode_err, ARRAY_LEN(host_decode_err), uval);
		break;
	case ATLV_UINT:
	case ATLV_FORMAT:
	case ATLV_BOOL:
	case ATLV_CONT:
	case ATLV_OFF:
	case ATLV_LEN:
	case ATLV_REGINFO:
	case ATLV_TIME_MS:
	case ATLV_ERR_BACKOFF:
		if (tlv_u32_get(&uval, tlv)) {
			host_decode_put(ctxt, "tlv_u32_get err");
			break;
		}
		host_decode_put(ctxt, "%lu", uval);
		break;
	case ATLV_EVENT_MASK:
	case ATLV_FEATURES:
	case ATLV_NODES:
		if (tlv_u32_get(&uval, tlv)) {
			host_decode_put(ctxt, "tlv_u32_get err");
			break;
		}
		host_decode_put(ctxt, "%#lx", uval);
		break;
	case ATLV_INT:
	case ATLV_CENTS:
		if (tlv_s32_get(&ival, tlv)) {
			host_decode_put(ctxt, "tlv_s32_get err");
			break;
		}
		host_decode_put(ctxt, "%ld", ival);
		break;
	case ATLV_ACK_ID:
		/*
		 * On AD_SEND_TLV, the ATLV_ACK_ID contains the ID, status, and
		 * message as separate TLVS.
		 * For others, it is just a string with the ACK-ID.
		 */
		if (ctxt->data_op == AD_SEND_TLV) {
			host_decode_tlvs(ctxt, TLV_VAL(tlv), tlv->len);
			break;
		}
		/* fall through */
	case ATLV_NAME:
	case ATLV_UTF8:
	case ATLV_DPMETA:
		rc = tlv_utf8_get(sval, sizeof(sval), tlv);
		if (rc < 0) {
			host_decode_put(ctxt, "tlv_utf8_get err");
			break;
		}

		/*
		 * If debug2 is not enabled, cut off string at max length.
		 * This could invalidate a UTF-8 character, but accept that.
		 */
		if (!log_mod_sev_is_enabled(MOD_LOG_IO, LOG_SEV_DEBUG2) &&
		    rc > HOST_DECODE_MAX_STR &&
		    HOST_DECODE_MAX_STR < sizeof(sval) - sizeof(ellipsis)) {
			memcpy(sval + HOST_DECODE_MAX_STR,
			    ellipsis, sizeof(ellipsis));
			rc = HOST_DECODE_MAX_STR + sizeof(ellipsis) - 1;
		}

		if (rc == 0) {
			host_decode_put(ctxt, "\"\"");
			break;
		}

		/*
		 * Put string in separate portions so it can be split.
		 */
		for (off = 0; off < rc; off += tlen) {
			tlen = HOST_DECODE_STR_LINE_LEN;
			if (tlen > rc - off) {
				tlen = rc - off;
				host_decode_put(ctxt, "\"%s", sval + off);
			} else {
				memcpy(saved, sval + off + tlen, sizeof(saved));
				memcpy(sval + off + tlen, quote, sizeof(quote));
				host_decode_put(ctxt, "\"%s", sval + off);
				memcpy(sval + off + tlen, saved, sizeof(saved));
			}
		}
		host_decode_put(ctxt, "\"");
		break;
	case ATLV_CONF:
		host_decode_tokens(sval, sizeof(sval),
		    (u8 *)TLV_VAL(tlv), tlv->len);
		host_decode_put(ctxt, "%s", sval);
		break;
	case ATLV_WIFI_STATUS:
	case ATLV_WIFI_STATUS_FINAL:
		tlen = *(u8 *)TLV_VAL(tlv);
		if (tlen >= 32) {
			host_decode_put(ctxt, "invalid_SSID");
			break;
		}
		memcpy(sval, (u8 *)TLV_VAL(tlv) + 1, tlen);
		sval[tlen] = '\0';
		host_decode_put(ctxt, "ssid=\"%s\",wifi_err=%u",
		    sval, *((u8 *)TLV_VAL(tlv) + 1 + tlen));
		break;
	default:
		if (tlv->len < HOST_DECODE_TLV_LEN) {
			host_decode_put(ctxt, "len=%u", tlv->len);
		}
		break;
	}
	host_decode_put(ctxt, ")");
}

/*
 * Iterate through TLVs and decode them.
 */
static void host_decode_tlvs(struct host_decode_context *ctxt,
		const void *tlvs, size_t tlvs_len)
{
	struct ayla_tlv *tlv = (struct ayla_tlv *)tlvs;
	size_t rlen = tlvs_len;

	while (rlen) {
		host_decode_put(ctxt, " ");
		if (rlen < sizeof(*tlv) || rlen < sizeof(*tlv) + tlv->len) {
			host_decode_put(ctxt, "(short tlv)");
			break;
		}
		host_decode_tlv(ctxt, tlv);
		rlen -= (sizeof(*tlv) + tlv->len);
		tlv = TLV_NEXT(tlv);
	}
}

/*
 * Decode Ayla command or data operation.
 */
void host_decode_log(const char *msg, const void *cmd_buf, size_t cmd_len)
{
	struct ayla_cmd *cmd;
	struct host_decode_context ctxt;

	if (!log_mod_sev_is_enabled(MOD_LOG_IO, LOG_SEV_DEBUG)) {
		return;
	}

	if (cmd_len < sizeof(*cmd)) {
		log_put_mod(MOD_LOG_IO, LOG_DEBUG "%s: len %zu", msg, cmd_len);
		return;
	}
	cmd = (struct ayla_cmd *)cmd_buf;

	memset(&ctxt, 0, sizeof(ctxt));
	ctxt.prefix = msg;

	host_decode_put(&ctxt, " ");
	host_decode_lookup(&ctxt,
	    host_decode_proto, ARRAY_LEN(host_decode_proto), cmd->protocol);
	host_decode_put(&ctxt, " %#x ", get_ua_be16(&cmd->req_id));

	switch (cmd->protocol) {
	case ASPI_PROTO_CMD:
		host_decode_lookup(&ctxt,
		    host_decode_cmd, ARRAY_LEN(host_decode_cmd),
		    cmd->opcode);
		host_decode_tlvs(&ctxt,
		    (void *)(cmd + 1), cmd_len - sizeof(*cmd));
		break;
	case ASPI_PROTO_DATA:
		host_decode_lookup(&ctxt,
		    host_decode_data, ARRAY_LEN(host_decode_data),
		    cmd->opcode);

		ctxt.data_op = cmd->opcode;

		/*
		 * Special case for features request.
		 */
		if  (cmd_len == sizeof(*cmd) && cmd->opcode == AD_SEND_PROP) {
			host_decode_put(&ctxt,
			    " (reboot notice / request for features)");
			break;
		}

		/*
		 * Special case for request all to-device properties.
		 */
		if  (cmd_len == sizeof(*cmd) && cmd->opcode == AD_REQ_TLV) {
			host_decode_put(&ctxt, " (all to-device properties)");
			break;
		}

		host_decode_tlvs(&ctxt,
		    (void *)(cmd + 1), cmd_len - sizeof(*cmd));
		break;
	default:
		break;
	}
	host_decode_flush(&ctxt);
}
