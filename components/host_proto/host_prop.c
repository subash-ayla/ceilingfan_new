/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <sys/types.h>
#include <string.h>
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/json.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/conf.h>
#include <ada/err.h>
#include <ada/prop.h>
#include <ada/prop_mgr.h>
#include <ayla/ayla_proto_mcu.h>
#include "host_prop.h"
#include "prop_req.h"
#include "data_tlv.h"

#define HOST_PROP_TEMPLATE_VER_PROP	"oem_host_version"

struct host_prop_state {
	u8	has_connected;	/* has done any connection */
	u8	busy;		/* is busy sending a prop request */
	u8	conn_mask;	/* connectivity mask */
	u16	get_req_id;	/* request ID for GET */
};
static struct host_prop_state host_prop_state;

struct host_prop_get_arg {
	enum ada_err (*callback)(struct prop *, void *arg, enum ada_err);
	void *arg;
};

char template_version[CONF_OEM_VER_MAX + 1];

static void host_prop_template_cb(struct prop *prop, void *arg,
		u32 cont, int err)
{
	if (err) {
		/* prop may be NULL */
		log_put(LOG_ERR "%s: err %d", __func__, err);
		return;
	}
	if (!prop) {
		log_put(LOG_ERR "%s: timeout", __func__);
		prop_req_get(HOST_PROP_TEMPLATE_VER_PROP,
		    host_prop_template_cb, NULL);
		return;
	}
	if (prop->type != ATLV_UTF8) {
		log_put(LOG_ERR "%s: prop %s type 0x%x",
		    __func__, prop->name, prop->type);
		return;
	}
	snprintf(template_version, sizeof(template_version), prop->val);
}

int host_prop_is_busy(const char *caller, const char *prop_name)
{
	struct host_prop_state *hp_state = &host_prop_state;

	return (int)hp_state->busy;
}

u32 host_prop_backoff_time_remaining(void)
{
	return 1;		/* TODO */
}

/*
 * Receive a property datapoint from the agent and forward to the host MCU.
 */
static enum ada_err host_prop_recv(const char *name, enum ayla_tlv_type type,
		const void *val, size_t len,
		size_t *offset, u8 src, void *cb_arg,
		const char *ack_id, struct prop_dp_meta *dp_meta)
{
	struct host_prop_state *hp_state = &host_prop_state;
	enum ada_err err;
	u32 off = (u32)*offset;
	u16 req_id = 0;
	u8 use_req_id = 0;

	/*
	 * Undocumented feature.  If cb_arg is 1, the property is being
	 * received as part of the requested GET.
	 */
	if (cb_arg == (void *)1) {
		use_req_id = 1;
		req_id = hp_state->get_req_id;
	}

	err = prop_req_prop_send(name, val, len, type, &off, src,
	    req_id, use_req_id, ack_id, dp_meta);
	if (err) {
		return err;
	}

	/*
	 * Returning AE_BUF will cause client to pause sending
	 * additional requests until ada_prop_mgr_recv_done() is called
	 * in prop_req.c.
	 */
	return AE_BUF;
}

/*
 * Callback to report success/failure of property post to ADS/apps.
 * status will be PROP_CB_DONE on success.
 * fail_mask contains a bitmask of failed destinations.
 */
static void host_prop_send_done(enum prop_cb_status status, u8 fail_mask,
		void *arg)
{
	struct host_prop_state *hp_state = &host_prop_state;
	u32 req_id = (unsigned long)arg;

	hp_state->busy = 0;
	prop_req_client_finished(status, fail_mask, (u16)req_id);
}

/*
 * Callback on completion of GET request.
 */
static void host_prop_get_val_cb(struct prop *prop, void *arg,
	 u32 continuation, int err)
{
	struct host_prop_get_arg *get_arg = arg;

	ASSERT(get_arg);
	ASSERT(get_arg->callback);
	get_arg->callback(prop, get_arg->arg, err);
	free(get_arg);
}

/*
 * ADA wants to fetch a value of a property.
 * Function must arrange to eventually call (*get_cb)
 * with the value of the property.
 */
static enum ada_err host_prop_get_val(const char *name,
		enum ada_err (*get_cb)(struct prop *, void *arg, enum ada_err),
		void *arg)
{
	struct host_prop_get_arg *get_arg;
	enum ada_err err;

	get_arg = calloc(1, sizeof(*get_arg));
	if (!get_arg) {
		return AE_ALLOC;
	}
	get_arg->callback = get_cb;
	get_arg->arg = arg;

	err = prop_req_get(name, host_prop_get_val_cb, get_arg);
	if (err != AE_OK && err != AE_IN_PROGRESS) {
		free(get_arg);
	}
	return err;
}

/*
 * ADA reports an event to all property managers.
 */
static void host_prop_event(enum prop_mgr_event event, const void *arg)
{
}

static void host_prop_connect_status(u8 mask)
{
	struct host_prop_state *hp_state = &host_prop_state;

	hp_state->conn_mask = mask;
	data_tlv_send_connectivity(mask);
}

static const struct prop_mgr host_prop_mgr = {
	.name = "host_prop_mgr",
	.prop_meta_recv = host_prop_recv,
	.send_done = host_prop_send_done,
	.get_val = host_prop_get_val,
	.connect_status = host_prop_connect_status,
	.event = host_prop_event,
};

/*
 * Register property manager.
 * This should be the first one registered so that it is the last one called.
 */
void host_prop_init(void)
{
	ada_prop_mgr_register(&host_prop_mgr);
	data_tlv_init();

	/*
	 * Update connectivity mask to MCU
	 */
	data_tlv_send_connectivity(0);

	/*
	 * Generate a specific error to the MCU by requesting a property
	 * with no name.  This indicates that the module has restarted and
	 * it should NAK with its feature mask.
	 */
	prop_req_get(NULL, NULL, NULL);

	/*
	 * Request the template version.
	 */
	prop_req_get(HOST_PROP_TEMPLATE_VER_PROP, host_prop_template_cb, NULL);
}

void host_prop_enable_listen(void)
{
	ada_prop_mgr_ready(&host_prop_mgr);
}

/*
 * Returns -1 if the property value is too long after being JSON escaped
 * Returns -2 if the property value is not properly UTF8 encoded.
 */
int host_prop_check_val_json(const char *val)
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

static void host_prop_get_cb(struct prop *prop)
{
	struct host_prop_state *hp_state = &host_prop_state;

	hp_state->busy = 0;
	data_tlv_clear_ads(hp_state->get_req_id, 1);
	free(prop);
}

/*
 * Request a property value from ADS for the MCU.
 */
int host_prop_get(u16 req_id, const char *name)
{
	struct host_prop_state *hp_state = &host_prop_state;
	enum ada_err err;

	if (host_prop_is_busy(__func__, name)) {
		return AERR_ADS_BUSY;
	}
	hp_state->get_req_id = req_id;
	err = ada_prop_mgr_request_get(name, host_prop_get_cb);
	if (err) {
		return err;
	}
	hp_state->busy = 1;
	return 0;
}

/*
 * Request all to-device values from ADS for the MCU.
 */
int host_prop_get_to_device(u16 req_id)
{
	return host_prop_get(req_id, NULL);
}

/*
 * Send property to ADS for host MCU.
 * Returns an MCU error code on failure.
 */
int host_prop_send(u32 req_id, struct prop *prop_in, u8 dest)
{
	struct host_prop_state *hp_state = &host_prop_state;
	struct prop *prop;
	size_t len;
	char *name;
	size_t name_len;
	size_t meta_len = sizeof(struct prop_dp_meta) * PROP_MAX_DPMETA;
	enum ada_err err;

	log_put(LOG_DEBUG "%s: prop \"%s\"", __func__, prop_in->name);

	name_len = strlen(prop_in->name) + 1;
	len = sizeof(*prop) + name_len + prop_in->len + 1;
	if (prop_in->dp_meta) {
		len += meta_len;
	}
	prop = malloc(len);
	if (!prop) {
		return AERR_INTERNAL;
	}
	memcpy(prop, prop_in, sizeof(*prop));
	if (prop_in->dp_meta) {
		prop->dp_meta = (struct prop_dp_meta *)(prop + 1);
		memcpy(prop->dp_meta, prop_in->dp_meta, meta_len);
		prop->val = (void *)((char *)prop->dp_meta + meta_len);
	} else {
		prop->val = (void *)(prop + 1);
	}
	memcpy(prop->val, prop_in->val, prop->len);
	((char *)prop->val)[prop->len] = '\0';
	name = (char *)prop->val + prop->len + 1;
	memcpy(name, prop_in->name, name_len);
	prop->name = name;

	if (!dest) {
		dest = NODES_ADS | hp_state->conn_mask;
	}
	hp_state->busy = 1;
	err = ada_prop_mgr_send(&host_prop_mgr, prop, dest, (void *)req_id);
	if (err != AE_IN_PROGRESS) {
		hp_state->busy = 0;
		log_put(LOG_ERR "%s: prop_mgr_send prop \"%s\" err %d",
		    __func__, name, err);
		return AERR_INVAL_REQ;
	}
	return 0;
}
