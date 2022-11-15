/*
 * Copyright 2011-2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/ayla_proto_mcu.h>
#include <ayla/tlv.h>
#include <ayla/timer.h>
#include <ayla/clock.h>
#include <ayla/json.h>
#include <ayla/nameval.h>

#include <net/base64.h>
#include <ada/err.h>
#include <ada/prop.h>
#include <ada/prop_mgr.h>
#include <ada/client.h>
#include <net/net.h>
#include "host_proto_int.h"
#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "data_tlv.h"
#include "prop_req.h"

#define MAX_ADS_BUSY_RESETS 2	/* max # of times we'll reset b/c ads busy */

/*
 * property request.
 * This represents an in-progress request from the module to the host.
 */
struct prop_req {
	char	name[PROP_NAME_LEN];
	struct prop_req *next;
	u32	continuation;

	/*
	 * Handler for sending request to MCU, called from a hp_buf callback
	 * when a buffer is available.
	 */
	void	(*handler)(struct hp_buf *bp, struct prop_req *);

	/*
	 * Handler for the completion to the property manager.
	 */
	void	(*callback)(struct prop *, void *arg, u32 cont, int err);
	void	*arg;			/* arg for callback */
	u8	busy_resets;		/* times we reset timeout due to busy */
	u8	use_req_id;
	u16	req_id;
	u32	offset;
	struct timer host_timer;	/* limit the time for MCU response */
	struct prop prop;		/* prop for send or get */
	char ack_id[PROP_ACK_ID_LEN + 1];
};

#ifdef AYLA_HOST_PROP_BATCH_SUPPORT
struct prop_batch {
	u16 id;
	u8 end:1;
	u64 time_ms;
};
static struct prop_batch prop_batch;
#endif

#ifdef AYLA_HOST_PROP_FILE_SUPPORT
static u8 prop_dp_eof;
static size_t prop_dp_len;
static size_t prop_dp_tot_len;
static u32 dp_recv_offset;
#endif

struct prop_req_state {
	u16	req_id;			/* current request ID */
	u16	get_req_id;		/* request ID of last GET request */
	u8	cb_trylater;
	u8	get_rst_timer;
	struct prop_req *req_list;	/* list of waiting get requests */

	/*
	 * Property being requested by/from the host.
	 * The prop_to_send.name is NULL if nothing to send.
	 */
	struct prop prop_to_send;
	union {
		char name[PROP_NAME_LEN];
#ifdef AYLA_HOST_PROP_FILE_SUPPORT
		char loc[PROP_LOC_LEN];
#endif
	} send_name;
};
static struct prop_req_state prop_req_state;

static void prop_req_timeout_start(struct prop_req *, u32);
static void prop_req_timeout_end(struct prop_req *req);
static void prop_req_cb(struct hp_buf *bp);

/*
 * Client finished sending this property.
 * Clear ads, prop_to_send, send nak if needed.
 */
void prop_req_client_finished(enum prop_cb_status status, u8 fail_mask,
		u16 req_id)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop *prop = &reqs->prop_to_send;
	u8 success = 0;
	const char *name = prop->name;

	log_put(LOG_DEBUG "%s: req %x status %u", __func__, req_id, status);

	if (status == PROP_CB_DONE) {
		success = 1;
		data_tlv_clear_ads(req_id, 1);
	} else if (status == PROP_CB_CONTINUE) {
		;		/* will return below */
	} else if (status == PROP_CB_INVAL_OFF_ERR) {
		data_tlv_nak(req_id, AERR_INVAL_OFF, 1);
	} else if (status == PROP_CB_OVERFLOW) {
		data_tlv_nak(req_id, AERR_OVERFLOW, 1);
	} else if (status == PROP_CB_CONN_ERR2) {
		if (prop->name[0] == '\0') {
			goto conn_err;
		}
		data_tlv_nak_req(req_id, AERR_CONN_ERR, prop->name,
		    fail_mask);
	} else if (status == PROP_CB_CONN_ERR) {
conn_err:
		data_tlv_nak(req_id, AERR_CONN_ERR, 1);
	} else if (status == PROP_CB_UNEXP_OP) {
		data_tlv_nak(req_id, AERR_UNEXP_OP, 1);
	} else if (status == PROP_CB_UNK_PROP) {
		data_tlv_nak(req_id, AERR_UNK_PROP, 1);
	} else if (status == PROP_CB_INVAL_FMT) {
		data_tlv_nak(req_id, AERR_INVAL_FMT, 1);
	} else if (status == PROP_CB_TOO_MANY) {
		data_tlv_nak(req_id, AERR_BUSY_RETRY, 1);
	} else if (status == PROP_CB_RETRY) {
		data_tlv_nak(req_id, AERR_RETRY, 1);
	} else {
		log_put(LOG_ERR "%s: req %x unexpected status %u",
		    __func__, req_id, status);
		data_tlv_nak(req_id, AERR_INTERNAL, 1);	/* unexpected case */
	}
	prop->name = NULL;
	prop->dp_meta = NULL;
#ifdef AYLA_HOST_PROP_ACK_SUPPORT
	prop->ack = NULL;
#endif
	if (status == PROP_CB_CONTINUE) {
		/*
		 * this state is used for PUT for long streams
		 * we don't want to reset the offset since more
		 * data is coming.
		 */
		return;
	}
	if (success) {
		log_put(LOG_DEBUG "%s prop %s success", __func__, name);
	} else {
		log_put(LOG_DEBUG "%s prop %s failure status %u dests %x",
		    __func__, name, status, fail_mask);
	}
	data_tlv_reset_offset();
#ifdef AYLA_HOST_PROP_BATCH_SUPPORT
	if (prop_batch.id) {
		if (!prop_batch.end) {
			return;		/* more of batch is expected */
		}
		prop_batch.id = 0;
		log_put(LOG_DEBUG "batch end");
	}
#endif

	if (reqs->cb_trylater) {
		reqs->cb_trylater = 0;
		hp_buf_callback_pend(prop_req_cb);
	} else if (reqs->get_rst_timer) {
		reqs->get_rst_timer = 0;
		if (reqs->req_list) {
			prop_req_timeout_restart();
		}
	}
}

#ifdef AYLA_HOST_PROP_FILE_SUPPORT
/*
 * Callback to send the dp loc post via client interface.
 */
static enum ada_err prop_dp_loc_send(enum prop_cb_status stat, void *arg)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop *prop = &reqs->prop_to_send;
	enum ada_err err = AE_OK;

	ASSERT(prop->name);
	switch (stat) {
	case PROP_CB_BEGIN:
		err = client_send_dp_loc_req(prop);
		break;
	case PROP_CB_DONE:
		if (!arg) {
			stat = PROP_CB_CONN_ERR;
			goto def_case;
		}
		err = data_tlv_dp_create(reqs->req_id, (char *)arg);
		if (err == AE_BUF) {
			break;
		}
		prop_req_client_finished(stat, 0, reqs->req_id);
		break;
	default:
def_case:
		prop_req_client_finished(stat, 0, reqs->req_id);
		break;
	}

	return err;
}

/*
 * Return a pointer to the location for the dp_put
 */
const char *prop_dp_put_get_loc(void)
{
	struct prop_req_state *reqs = &prop_req_state;

	return reqs->prop_to_send.name;
}

/*
 * Callback to close the file dp via client interface
 */
enum ada_err prop_dp_put_close(enum prop_cb_status stat, void *arg)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop *prop = &reqs->prop_to_send;
	enum ada_err err = AE_OK;

	ASSERT(prop->name);
	switch (stat) {
	case PROP_CB_BEGIN:
		err = client_close_dp_put(prop->name);
		break;
	case PROP_CB_DONE:
		prop_req_client_finished(stat, 0, reqs->req_id);
		break;
	default:
		prop_req_client_finished(PROP_CB_CONN_ERR, 0, reqs->req_id);
		break;
	}
	return err;
}

/*
 * Callback to send the dp put via client interface.
 */
enum ada_err prop_dp_put(enum prop_cb_status stat, void *arg)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop *prop = &reqs->prop_to_send;
	enum ada_err err = AE_OK;

	ASSERT(prop->name);
	switch (stat) {
	case PROP_CB_BEGIN:
		err = client_send_dp_put(prop->val, prop_dp_len, prop->name,
		    dp_recv_offset, prop_dp_tot_len, prop_dp_eof);
		break;
	case PROP_CB_DONE:
		prop_dp_tot_len -= prop_dp_len;
#ifdef WDG_SUPPORT
		wdg_reload();
#endif
		if (!prop_dp_tot_len) {
			if (prop->type != ATLV_LOC) {
				/* no close action for message datapoints */
				prop_req_client_finished(stat, 0,
				    reqs->req_id);
				break;
			}
			/* DP put is complete. close the dp on ADS */
			client_send_callback_set(prop_dp_put_close, 1);
		} else {
			prop_req_client_finished(PROP_CB_CONTINUE, 0,
			    reqs->req_id);
		}
		break;
	default:
		prop_req_client_finished(stat, 0, reqs->req_id);
		break;
	}
	return err;
}

/*
 * Callback to tell the service the dp has been fetched.
 */
static enum ada_err prop_dp_fetched(enum prop_cb_status stat, void *arg)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop *prop = &reqs->prop_to_send;
	enum ada_err err = AE_OK;

	ASSERT(prop->name);
	switch (stat) {
	case PROP_CB_BEGIN:
		err = client_send_dp_fetched(prop->name);
		break;
	default:
		prop_req_client_finished(stat, 0, reqs->req_id);
		client_notify_if_partial();
		break;
	}
	return err;
}

/*
 * Callback to send the get dp req to service via the client interface.
 */
enum ada_err prop_dp_req(enum prop_cb_status stat, void *arg)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct recv_payload *recv;
	struct prop *prop = &reqs->prop_to_send;
	enum ada_err err = AE_OK;
	int block_size;
	size_t prop_size = PROP_VAL_LEN;
	struct http_client *hc;
	u8 eof;

	ASSERT(prop->name);
	switch (stat) {
	case PROP_CB_BEGIN:
		err = client_get_dp_req(prop_dp_len,
		    prop_dp_len + LONG_GET_REQ_SIZE - 1);
		break;
	case PROP_CB_CONTINUE:
#ifdef WDG_SUPPORT
		wdg_reload();
#endif
		if (!arg) {
			stat = PROP_CB_CONN_ERR;
			goto def_case;
		}
		recv = (struct recv_payload *)arg;
		while (recv->len > 0) {
			block_size = (recv->len < prop_size) ? recv->len :
			    prop_size;

			/*
			 * For message properties, send overall length with
			 * first response.  File properties could do this, but
			 * we avoid changing the MCU protocol for compatibility.
			 */
			eof = (prop->type != ATLV_LOC &&
			    recv->tot_size == prop_dp_len + block_size);
			prop->sent_eof = eof;

			err = data_tlv_dp_stream_resp(reqs->req_id, prop->type,
			    recv->data, block_size,
			    prop_dp_len, recv->tot_size, prop->name, eof);
			if (err == AE_BUF) {
				break;
			}
			recv->len -= block_size;
			recv->data += block_size;
			recv->consumed += block_size;
			prop_dp_len += block_size;
		}
		break;
	case PROP_CB_DONE:
		if (!arg) {
			stat = PROP_CB_CONN_ERR;
			goto def_case;
		}
		hc = (struct http_client *)arg;
		if (!hc->range_given || prop_dp_len >= hc->range_bytes ||
		    hc->range_end > prop_dp_len) {
			if (prop->type == ATLV_LOC || !prop->sent_eof) {
				err = data_tlv_dp_stream_resp(reqs->req_id,
				    prop->type, NULL, 0, prop_dp_len,
				    0, prop->name, 1);
				if (err == AE_BUF) {
					break;
				}
			}
			prop_req_client_finished(stat, 0, reqs->req_id);
		} else {
			/*
			 * since we have a limit on the size of the ssl blocks
			 * we can support, GET dp_req is done through multiple
			 * requests of small chunks each.
			 */
			client_send_callback_set(prop_dp_req, 1);
			err = AE_INPROGRESS;
		}
		break;
	case PROP_CB_OVERFLOW:
		prop_req_client_finished(PROP_CB_OVERFLOW, 0, reqs->req_id);
		break;
	default:
def_case:
		prop_req_client_finished(stat, 0, reqs->req_id);
		break;
	}

	return err;
}

/*
 * Callback to send the get dp req to service via the client interface.
 */
static enum ada_err prop_dp_loc_req(enum prop_cb_status stat, void *arg)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop *prop = &reqs->prop_to_send;
	enum ada_err err = AE_OK;

	ASSERT(prop->name);
	switch (stat) {
	case PROP_CB_BEGIN:
		err = client_get_dp_loc_req(prop->name);
		break;
	case PROP_CB_DONE:
		client_send_callback_set(prop_dp_req, NODES_ADS);
		break;
	default:
		prop_req_client_finished(stat, 0, reqs->req_id);
		break;
	}

	return err;
}
#endif /* AYLA_HOST_PROP_FILE_SUPPORT */

/*
 * Returns 1 if module is busy posting a property to the service
 */
int prop_req_is_busy(const char *caller, const char *dropped)
{
	struct prop_req_state *reqs = &prop_req_state;

	if (reqs->prop_to_send.name) {
		if (caller) {
			log_put(LOG_WARN
			    "%s: %s dropped due to ADS busy with %s",
			    caller, dropped, reqs->prop_to_send.name);
		}
		return 1;
	}
	return 0;
}

static void prop_req_name_set(const char *name)
{
	struct prop_req_state *reqs = &prop_req_state;
	size_t len;

	len = strlen(name);
	ASSERT(len < sizeof(reqs->send_name.name));
	memcpy(reqs->send_name.name, name, len + 1);
	reqs->prop_to_send.name = reqs->send_name.name;
}

static void prop_req_send_copy(struct prop *prop)
{
	struct prop_req_state *reqs = &prop_req_state;

	memcpy(&reqs->prop_to_send, prop, sizeof(reqs->prop_to_send));
	prop_req_name_set(prop->name);
}

/*
 * Setup the property response from MCU to the requestor
 */
int prop_req_setup_response(struct prop *prop)
{
	struct prop_req_state *reqs = &prop_req_state;

	if (prop_req_is_busy(__func__, prop->name)) {
		return AERR_ADS_BUSY;
	}
	reqs->req_id++;
	prop_req_send_copy(prop);
	return 0;
}

/*
 * If we're not busy, this sets up a callback for handling the next request.
 * Otherwise set a flag so we send it once we're not busy.
 */
static void prop_req_setup_cb(void)
{
	struct prop_req_state *reqs = &prop_req_state;

	if (!prop_req_is_busy(NULL, NULL)) {
		hp_buf_callback_pend(prop_req_cb);
	} else {
		reqs->cb_trylater = 1;
	}
}

static void *prop_req_enq_int(void *arg)
{
	struct prop_req *preq = (struct prop_req *)arg;
	struct prop_req_state *reqs = &prop_req_state;
	struct prop_req *prev = reqs->req_list;

	if (prev == NULL) {
		reqs->req_list = preq;
		prop_req_setup_cb();
	} else {
		while (prev->next != NULL) {
			prev = prev->next;
		}
		prev->next = preq;
	}
	return NULL;
}

/*
 * Add a property request to the end of the req_list.
 * Starts the request if the list was previously empty.
 */
static void prop_req_enq(struct prop_req *preq)
{
	host_proto_call_blocking(prop_req_enq_int, preq);
}

/*
 * Remove a property request from the head of the list.
 * The entry must be on the list.
 * Entry is usually first on the list (will assert otherwise).
 */
static void prop_req_deq(struct prop_req *preq)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop_req **prev = &reqs->req_list;

	while (*prev && *prev != preq) {
		prev = &(*prev)->next;
	}
	ASSERT(*prev);
	*prev = preq->next;
}

static void prop_req_done(struct prop_req *preq, struct prop *prop, int error)
{
	struct prop_req_state *reqs = &prop_req_state;

	prop_req_timeout_end(preq);
	prop_req_deq(preq);
	if (preq->callback) {
		if (!preq->prop.name) {
			prop = NULL;	/* prop not involved in request */
		}
		preq->callback(prop, preq->arg, preq->continuation, error);
	}

	/*
	 * If the prop_req_list isn't empty AND a new entry wasn't
	 * added by the callback then send the next prop_get cb.
	 */
	if (reqs->req_list && reqs->req_list == preq->next) {
		prop_req_setup_cb();
	}
	free(preq);
}

/*
 * If a particular prop get request is taking too long,
 * drop it and move on so we can serve other requests.
 */
static void prop_req_get_timeout(struct timer *tm)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop_req *preq = CONTAINER_OF(struct prop_req, host_timer, tm);

	if (preq == NULL) {
		return;
	}
	if (preq->name[0]) {
		log_put(LOG_WARN "timed out waiting for response prop: %s",
		    preq->name);
	} else {
		log_put(LOG_WARN "features req timeout");
	}

	if (prop_req_is_busy(NULL, NULL) &&
	    preq->busy_resets < MAX_ADS_BUSY_RESETS) {
		preq->busy_resets++;
		reqs->get_rst_timer = 1;
		return;
	}
	if (reqs->req_list != preq) {
		log_put(LOG_ERR "%s: timed out request not active", __func__);
		return;
	}
	prop_req_done(preq, NULL, 0);
}

/*
 * Callback that is made when a buffer is available.
 * Send prop request command to MCU.
 */
static void prop_req_cb(struct hp_buf *bp)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop_req *preq = reqs->req_list;

	if (preq == NULL) {
		hp_buf_free(bp);
		return;
	}
	ASSERT(preq->handler);
	preq->handler(bp, preq);
}

/*
 * Set up a new property request for a name.
 */
static struct prop_req *prop_req_new(const char *name)
{
	struct prop_req *preq;
	size_t len;

	preq = calloc(1, sizeof(*preq));
	if (!preq) {
		return NULL;
	}

	/*
	 * Copy name to request.
	 */
	if (name) {
		len = strlen(name);
		if (len >= sizeof(preq->name)) {
			free(preq);
			return NULL;
		}
		memcpy(preq->name, name, len + 1);
	}
	ayla_timer_init(&preq->host_timer, prop_req_get_timeout);
	return preq;
}

/*
 * Handle sending a request property value.
 */
static void prop_req_handle_get(struct hp_buf *bp, struct prop_req *req)
{
	struct prop_req_state *reqs = &prop_req_state;
	u32 time_limit = HOST_PROP_REQ_TIMEOUT;
	const char *name = req->name;

	if (!name[0]) {
		name = NULL;
		time_limit = HOST_PROP_FEATURES_TIMEOUT;
	}
	reqs->get_req_id = data_tlv_prop_req_send(bp, name);
	prop_req_timeout_start(req, time_limit);
}

/*
 * Request property names and types from host.
 * Forms and queues a request.
 * Returns AE_IN_PROGRESS on success.
 */
enum ada_err prop_req_get(const char *name,
		void (*cb)(struct prop *, void *, u32, int), void *arg)
{
	struct prop_req *preq;

	preq = prop_req_new(name);
	if (!preq) {
		return AE_ALLOC;
	}
	preq->callback = cb;
	preq->arg = arg;
	preq->handler = prop_req_handle_get;
	preq->prop.name = name;

	log_put(LOG_DEBUG "prop_req_get: prop %s", name);
	prop_req_enq(preq);
	return AE_IN_PROGRESS;
}

/*
 * Handle sending a continuation request.
 */
static void prop_req_continuation(struct hp_buf *bp, struct prop_req *req)
{
	struct prop_req_state *reqs = &prop_req_state;

	data_tlv_req_next(bp, req->continuation, &reqs->get_req_id);

	prop_req_timeout_start(req, HOST_PROP_REQ_TIMEOUT);
}

/*
 * Request next property from MCU.
 * A continuation token is given, which if 0 indicates the first property.
 * If non-zero, it should be the value received from an earlier get_next.
 * Returns zero on success.
 */
int prop_req_get_next(u32 continuation,
		void (*cb)(struct prop *, void *arg, u32 cont, int err),
		void *arg)
{
	struct prop_req *preq;

	preq = prop_req_new(NULL);
	if (!preq) {
		return -1;
	}
	preq->callback = cb;
	preq->arg = arg;
	preq->continuation = continuation;
	preq->handler = prop_req_continuation;

	prop_req_enq(preq);
	return 0;
}

static void prop_req_timeout_start(struct prop_req *preq, u32 time)
{
	host_proto_timer_set(&preq->host_timer, time);
}

static void prop_req_timeout_end(struct prop_req *preq)
{
	host_proto_timer_cancel(&preq->host_timer);
}

void prop_req_timeout_halt(void)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop_req *preq = reqs->req_list;

	if (!preq) {
		return;
	}
	prop_req_timeout_end(preq);
}

void prop_req_timeout_restart(void)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop_req *preq = reqs->req_list;

	if (!preq) {
		return;
	}
	prop_req_timeout_start(preq, HOST_PROP_REQ_TIMEOUT);
}

/*
 * Reset the timeout. Useful when receiving a long string. Probably want to
 * reset this timer after receiving each piece.
 */
void prop_req_resp_reset_timeout(u16 req_id)
{
	struct prop_req_state *reqs = &prop_req_state;

	if (req_id == reqs->get_req_id) {
		prop_req_timeout_restart();
	}
}

/*
 * Handle reply for prop_get() request.
 * Called from data_tlv after parsing response from MCU.
 */
int prop_req_get_resp(u16 req_id, struct prop *prop,
			u32 continuation, int error)
{
	struct prop_req_state *reqs = &prop_req_state;
	struct prop_req *preq = reqs->req_list;

	if (preq == NULL || req_id != reqs->get_req_id) {
		return AERR_INTERNAL;
	}
	preq->continuation = continuation;
	prop_req_done(preq, prop, error);
	data_tlv_clear_ads(req_id, 1);
	data_tlv_reset_offset();
	return 0;
}

/*
 * Returns -1 if the property value is too long after being JSON escaped
 * Returns -2 if the property value is not properly UTF8 encoded.
 */
int prop_chk_val_after_json(char *val)
{
	ssize_t len = json_format_bytes(NULL, 0, val, strlen(val),
	    NULL, NULL, 0);

	if (len > TLV_MAX_STR_LEN) {
		return -1;
	} else if (len < 0) {
		/* bad string. can't json encode */
		return -2;
	}
	return 0;
}

/*
 * Send an echo fail TLV to the MCU for prop_to_send
 */
void prop_send_echofail(void)
{
	struct prop_req_state *reqs = &prop_req_state;

	ASSERT(reqs->prop_to_send.name);
	data_tlv_send_echofail((char *)reqs->prop_to_send.name);
}

/*
 * Handle send request.
 */
static void prop_req_handle_send(struct hp_buf *bp, struct prop_req *preq)
{
	struct prop *prop = &preq->prop;
	enum ada_err err;
	int mcu_err;
	u8 src;

	err = data_tlv_send(bp, prop->name, prop->val, prop->len,
	    prop->type, &preq->offset, prop->send_dest,
	    preq->req_id, preq->use_req_id, preq->ack_id, prop->dp_meta);
	if (err == AE_BUF) {
		log_put(LOG_DEBUG "%s: send \"%s\" off %lu AE_BUF",
		    __func__, prop->name, preq->offset);
		hp_buf_callback_pend(prop_req_cb);
		return;
	}
	if (err) {
		log_put(LOG_ERR "%s: send \"%s\" off %lu err %d",
		    __func__, prop->name, preq->offset, err);
		mcu_err = AERR_INTERNAL;
	} else {
		log_put(LOG_DEBUG "%s: send \"%s\" off %lu",
		    __func__, prop->name, preq->offset);
		mcu_err = 0;
	}
	src = prop->send_dest;
	prop_req_done(preq, NULL, mcu_err);
	ada_prop_mgr_recv_done(src);
}

/*
 * Send property change to MCU.
 */
enum ada_err prop_req_prop_send(const char *name, const void *val,
	size_t val_len, enum ayla_tlv_type type, u32 *offset,
	u8 src, u16 req_id, u8 use_req_id,
	const char *ack_id, const struct prop_dp_meta *dp_meta)
{
	struct prop_req *preq;
	struct prop *prop;

	preq = prop_req_new(name);
	if (!preq) {
		return AE_ALLOC;
	}
	preq->handler = prop_req_handle_send;
	preq->offset = *offset;
	preq->req_id = req_id;
	preq->use_req_id = use_req_id;

#ifdef AYLA_HOST_PROP_ACK_SUPPORT
	if (snprintf(preq->ack_id, sizeof(preq->ack_id), "%s",
	    ack_id) >= sizeof(preq->ack_id)) {
		preq->ack_id[0] = '\0';
	}
#endif

	prop = &preq->prop;
	prop->name = preq->name;
	prop->val = (void *)val;	/* discards const */
	prop->len = val_len;
	prop->type = type;
	prop->send_dest = src;
	prop->dp_meta = (struct prop_dp_meta *)dp_meta;	/* discards const */

	prop_req_enq(preq);
	return AE_OK;
}
