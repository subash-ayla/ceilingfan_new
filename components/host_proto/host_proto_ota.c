/*
 * Copyright 2013-2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include <ayla/utypes.h>
#include <ayla/endian.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/ayla_spi_mcu.h>
#include <ayla/ayla_proto_mcu.h>
#include <ayla/conf.h>
#include <ayla/patch.h>
#include <ayla/clock.h>
#include <ayla/timer.h>
#include <al/al_os_lock.h>
#include <ada/err.h>
#include <ada/client_ota.h>
#include <host_proto/mcu_dev.h>

#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "host_proto_ota.h"
#include "conf_tlv.h"
#include "host_proto_int.h"

#define HOST_PROTO_OTA_NTFY_INTVL 120000 /* ms between notification to MCU */
#define HOST_PROTO_OTA_CHUNK_SIZE (MAX_U8 * 8) /* fetch size for MCU */

struct host_proto_ota_state {
	u32	file_off;	/* next file offset */
	u32	ota_size;	/* image size */
	void	*image_buf;	/* image buffer */
	size_t	image_buf_len;	/* bytes of valid data in buffer */
	u16	buf_off;	/* offset to next data in buffer */
	u8	done;		/* send boot req */
	u8	mcu_err;	/* errcode from host MCU (if any) */
	struct timer notify_timer;

	/* status info */
	enum ayla_cmd_op status_op;	/* notify / status opcode */
	char	*version;		/* host firmware version (malloced) */
	struct al_lock *lock;		/* lock covering this structure */
};
static struct host_proto_ota_state host_proto_ota_state;

static void host_proto_ota_notify_tmo(struct timer *tm);
static enum ada_err host_proto_ota_save_start(void);

/*
 * Free OTA data.
 * Called only in agent_app thread.
 * Called with lock held.
 */
static void host_proto_ota_free(void)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;

	free(ota_state->version);
	ota_state->version = NULL;
	free(ota_state->image_buf);
	ota_state->image_buf = NULL;
}

/*
 * Inform MCU of OTA status.
 * Called in agent_app thread.
 */
static void host_proto_ota_status_cb(struct hp_buf *bp)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;
	struct ayla_cmd *cmd;
	int len;
	int rc;
	u32 sz = ota_state->ota_size;

	switch (ota_state->status_op) {
	case ACMD_MCU_OTA:
		log_put(LOG_INFO "host OTA notificaton. len %u \"%s\"",
		    (unsigned int)sz, ota_state->version);
		break;
	case ACMD_MCU_OTA_BOOT:
		log_put(LOG_INFO "send boot req \"%s\"", ota_state->version);
		sz = 0;
		break;
	default:
		break;
	}

	cmd = (struct ayla_cmd *)bp->payload;
	cmd->protocol = ASPI_PROTO_CMD;
	cmd->opcode = ota_state->status_op;
	put_ua_be16(&cmd->req_id, conf_tlv_next_req_id());

	len = sizeof(*cmd);
	if (ota_state->version) {
		rc = tlv_put_str(bp->payload + len, HP_BUF_LEN - len,
		    ota_state->version);
		if (rc > 0) {
			len += rc;
		}
	}
	if (sz) {
		put_ua_be32(&sz, sz);
		rc = tlv_put(bp->payload + len, HP_BUF_LEN - len,
		    ATLV_LEN, &sz, sizeof(sz));
		if (rc > 0) {
			len += rc;
		}
	}
	bp->len = len;

	mcu_dev->enq_tx(bp);

	al_os_lock_lock(ota_state->lock);
	if (ota_state->status_op == ACMD_MCU_OTA) {
		host_proto_ota_free();
	}
	al_os_lock_unlock(ota_state->lock);
}

/*
 * Receive an OTA-related message from the host MCU.
 * Returns 0 on success or MCU error number AERR_XXX.
 * Called in agent_app thread.
 */
int host_proto_ota_rx(const void *buf, int len)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;
	struct ayla_cmd *cmd;
	struct ayla_tlv *tlv;
	u8 err;

	cmd = (struct ayla_cmd *)buf;
	switch (cmd->opcode) {
	case ACMD_MCU_OTA:
		log_info("MCU_OTA: starting");
		host_proto_timer_cancel(&ota_state->notify_timer);
		if (host_proto_ota_save_start()) {
			return AERR_INTERNAL;
		}
		ada_ota_fetch_len_set(HOST_PROTO_OTA_CHUNK_SIZE);
		ada_ota_start();
		break;
	case ACMD_MCU_OTA_STAT:
		if (len < sizeof(*cmd) + sizeof(*tlv) + sizeof(u8)) {
			return AERR_INVAL_TLV;
		}
		tlv = (struct ayla_tlv *)(cmd + 1);
		if (tlv->len != sizeof(u8) || tlv->type != ATLV_ERR) {
			return AERR_INVAL_TLV;
		}
		err = *(u8 *)(tlv + 1);
		ota_state->mcu_err = err;
		if (err) {
			log_warn(LOG_MOD_DEFAULT, "host OTA error %d", err);
			host_proto_timer_cancel(&ota_state->notify_timer);
		} else {
			log_info("host OTA finished");
		}
		switch (err) {
		case 0:
			break;
		case AERR_INVAL_TLV:
		case AERR_INVAL_OFF:
			/* something botched internally, should retry */
			err = PB_ERR_READ; /* xXXXx */
			break;
		case AERR_LEN_ERR:
			err = PB_ERR_OP_LEN;
			break;
		case AERR_CHECKSUM:
			/* invalid image */
			err = PB_ERR_FILE_CRC;
			break;
		case AERR_BOOT:
			/*
			 * MCU tried to boot to image, and failed due to
			 * unknown reason.
			 */
			err = PB_ERR_BOOT;
			break;
		case AERR_ALREADY:
			/*
			 * MCU already running this version.
			 */
			err = PB_DONE;
			break;
		case AERR_INTERNAL:
			/*
			 * MCU internal error while writing flash
			 */
			err = PB_ERR_ERASE;
			break;
		default:
			err = PB_ERR_READ;
			log_warn(LOG_MOD_DEFAULT, "MCU OTA, unknown errcode");
			break;
		}
		ada_ota_report(err);
		al_os_lock_lock(ota_state->lock);
		host_proto_ota_free();
		al_os_lock_unlock(ota_state->lock);
		break;
	default:
		break;
	}
	return 0;
}

/*
 * Notify timeout.
 * Called in agent_app thread.
 */
static void host_proto_ota_notify_tmo(struct timer *tm)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;

	al_os_lock_lock(ota_state->lock);
	if (!ota_state->done) {
		ada_ota_report(PB_ERR_NOTIFY);
		host_proto_ota_free();
	} else {
		ota_state->status_op = ACMD_MCU_OTA;
		hp_buf_callback_pend(host_proto_ota_status_cb);
		host_proto_timer_set(&ota_state->notify_timer,
		    HOST_PROTO_OTA_NTFY_INTVL);
	}
	al_os_lock_unlock(ota_state->lock);
}

/*
 * Handle notificaiton of available OTA.
 * Called in client thread.
 */
static enum patch_state host_proto_ota_notify(const struct ada_ota_info *info)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;
	size_t len;

	al_os_lock_lock(ota_state->lock);
	free(ota_state->version);
	len = strlen(info->version) + 1;
	ota_state->version = malloc(len);
	if (!ota_state->version) {
		al_os_lock_unlock(ota_state->lock);
		return PB_ERR_MEM;
	}
	memcpy(ota_state->version, info->version, len);

	ota_state->ota_size = info->length;
	ota_state->status_op = ACMD_MCU_OTA;
	hp_buf_callback_pend(host_proto_ota_status_cb);
	host_proto_timer_set(&ota_state->notify_timer,
	    HOST_PROTO_OTA_NTFY_INTVL);
	al_os_lock_unlock(ota_state->lock);
	return PB_DONE;
}

/*
 * Download starts.
 * Called in agent_app thread.
 */
static enum ada_err host_proto_ota_save_start(void)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;

	al_os_lock_lock(ota_state->lock);
	ota_state->image_buf = malloc(HOST_PROTO_OTA_CHUNK_SIZE);
	if (!ota_state->image_buf) {
		al_os_lock_unlock(ota_state->lock);
		log_put(LOG_ERR "host OTA: malloc error");
		return AE_ALLOC;
	}
	ota_state->done = 0;
	ota_state->mcu_err = 0;
	al_os_lock_unlock(ota_state->lock);
	return AE_OK;
}

/*
 * Send OTA LOAD message to MCU.
 * Called in agent_app thread.
 */
static void host_proto_ota_send_chunk(struct hp_buf *bp)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;
	struct ayla_cmd *cmd;
	struct ayla_tlv *tlv;
	size_t len;

	al_os_lock_lock(ota_state->lock);
	if (!ota_state->image_buf) {
		hp_buf_free(bp);
		al_os_lock_unlock(ota_state->lock);
		return;			/* OTA may have failed */
	}
	len = ota_state->image_buf_len - ota_state->buf_off;
	if (len > MAX_U8) {
		len = MAX_U8;
	}

	cmd = (struct ayla_cmd *)bp->payload;
	cmd->protocol = ASPI_PROTO_CMD;
	cmd->opcode = ACMD_MCU_OTA_LOAD;
	put_ua_be16(&cmd->req_id, conf_tlv_next_req_id());

	tlv = (struct ayla_tlv *)(cmd + 1);
	tlv->type = ATLV_OFF;
	tlv->len = sizeof(u32);
	put_ua_be32(TLV_VAL(tlv), ota_state->file_off);

	tlv = TLV_NEXT_LEN(tlv, sizeof(u32));
	tlv->type = ATLV_BIN;
	tlv->len = (u8)len;
	memcpy(TLV_VAL(tlv),
	    (char *)ota_state->image_buf + ota_state->buf_off, len);

	bp->len = sizeof(*cmd) + sizeof(*tlv) + sizeof(u32) +
	    sizeof(*tlv) + len;

	ota_state->buf_off += len;
	ota_state->file_off += len;
	mcu_dev->enq_tx(bp);

	if (ota_state->file_off >= ota_state->ota_size) {
		ota_state->done = 1;
	} else if (ota_state->buf_off >= ota_state->image_buf_len) {
		ota_state->buf_off = 0;
		ota_state->image_buf_len = 0;
		ada_ota_continue();
	} else {
		hp_buf_callback_pend(host_proto_ota_send_chunk);
	}
	al_os_lock_unlock(ota_state->lock);
}

/*
 * New blob of image came from AIS. Store it and start forwarding it
 * to MCU.
 * Called in client thread.
 */
static enum patch_state host_proto_ota_save(unsigned int foff,
		const void *data, size_t len)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;
	unsigned int next_off;

	log_put(LOG_DEBUG "host OTA save: off %u (%#x) len %zu",
	    foff, foff, len);

	al_os_lock_lock(ota_state->lock);
	next_off = ota_state->file_off + ota_state->image_buf_len;
	if (foff != next_off) {
		log_put(LOG_WARN "host OTA save: offset skips from %u to %u",
		    next_off, foff);
		goto fatal_err;
	}
	if (!ota_state->image_buf) {
		log_put(LOG_ERR "host OTA save: no buffer");
		goto fatal_err;
	}
	if (len > HOST_PROTO_OTA_CHUNK_SIZE - ota_state->image_buf_len) {
		log_put(LOG_ERR "host OTA save: len %zu exceeds buffer",
		    len);
		goto fatal_err;
	}
	memcpy(ota_state->image_buf + ota_state->image_buf_len, data, len);
	ota_state->image_buf_len += len;
	ota_state->buf_off = 0;

	if (ota_state->image_buf_len < HOST_PROTO_OTA_CHUNK_SIZE &&
	    ota_state->file_off + ota_state->image_buf_len <
	    ota_state->ota_size) {
		al_os_lock_unlock(ota_state->lock);
		return PB_DONE;
	}
	hp_buf_callback_pend(host_proto_ota_send_chunk);
	al_os_lock_unlock(ota_state->lock);
	return PB_ERR_STALL;

fatal_err:
	host_proto_ota_free();
	al_os_lock_unlock(ota_state->lock);
	return PB_ERR_FATAL;
}

/*
 * Report OTA download complete.
 * Indicates the host MCU can reboot to the new image.
 * Called in client thread.
 */
static void host_proto_ota_done(void)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;

	al_os_lock_lock(ota_state->lock);
	log_put(LOG_DEBUG "host OTA done: flag %u err %u",
	    ota_state->done, ota_state->mcu_err);
	if (!ota_state->done || ota_state->mcu_err) {
		al_os_lock_unlock(ota_state->lock);
		return;
	}
	ota_state->status_op = ACMD_MCU_OTA_BOOT;
	al_os_lock_unlock(ota_state->lock);
	hp_buf_callback_pend(host_proto_ota_status_cb);
}

static const struct ada_ota_ops host_proto_ota_ops = {
	.notify = host_proto_ota_notify,
	.save = host_proto_ota_save,
	.save_done = host_proto_ota_done,
};

void host_proto_ota_init(void)
{
	struct host_proto_ota_state *ota_state = &host_proto_ota_state;

	ota_state->lock = al_os_lock_create();
	ASSERT(ota_state->lock);
	ayla_timer_init(&ota_state->notify_timer, host_proto_ota_notify_tmo);
	ada_ota_register(OTA_HOST, &host_proto_ota_ops);
}
