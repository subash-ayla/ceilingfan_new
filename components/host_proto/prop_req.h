/*
 * Copyright 2011 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __HOST_PROTO_PROP_REQ_H__
#define __HOST_PROTO_PROP_REQ_H__

#define HOST_PROP_REQ_TIMEOUT 500	/* (ms) timeout for prop request */
#define HOST_PROP_FEATURES_TIMEOUT 9000 /* (ms) time for feature request */
#define HOST_PROP_ACK_TIMEOUT 500	/* (ms) timeout for acks from host */
#define HOST_PROP_SEND_TIMEOUT 25000	/* (ms) timeout for prop sends */

/*
 * Issue request to the host for a property.
 */
enum ada_err prop_req_get(const char *name,
		void (*cb)(struct prop *, void *, u32 cont, int err),
		void *arg);

/*
 * Issue request to the host to get next property.
 * A callback with a zero continuation will indicate the end.
 */
int prop_req_get_next(u32 continuation,
		void (*cb)(struct prop *, void *, u32, int), void *arg);

/*
 * Handle response to prop_get().
 */
int prop_req_get_resp(u16 req_id, struct prop *, u32 continuation, int error);

/*
 * Complete response to prop_get().
 * - only used if property is not handled by the client (eg. CLI, HTTP server)
 */
void prop_req_get_resp_finished(void);

/*
 * Format a typed value into a buffer.
 */
size_t prop_req_fmt(char *buf, size_t len, enum ayla_tlv_type type,
		void *val, size_t val_len, char **out_val);

#ifdef AYLA_HOST_PROP_FILE_SUPPORT
/*
 * Create a data point.
 */
void prop_req_dp_create(const char *name, u16 req_id);
#endif /* AYLA_HOST_PROP_FILE_SUPPORT */

/*
 * Returns 1 if module is busy posting a property to the service
 */
int prop_req_is_busy(const char *caller, const char *dropped);

/*
 * Callback to send the current property via client interface.
 */
enum ada_err prop_req_send(enum prop_cb_status stat, void *arg);

/*
 * Send an echo failure TLV to the host.
 */
void prop_req_send_echofail(void);

/*
 * Setup the property response from the host to the requestor
 */
int prop_req_setup_response(struct prop *prop);

/*
 * Halt and restart the prop get timeout
 * Used to hold the timeout during ADS_BUSY so that we don't
 * penalize the host for that time
 */
void prop_req_timeout_halt(void);
void prop_req_timeout_restart(void);

/*
 * Reset the timeout. Useful when receiving a long string. Probably want to
 * reset this timer after receiving each piece.
 */
void prop_req_resp_reset_timeout(u16 req_id);

/*
 * Send property change to MCU.
 */
enum ada_err prop_req_prop_send(const char *name, const void *val,
	size_t val_len, enum ayla_tlv_type type, u32 *offset,
	u8 src, u16 req_id, u8 use_req_id,
	const char *ack_id, const struct prop_dp_meta *dp_meta);

/*
 * Client is finished with request.
 */
void prop_req_client_finished(enum prop_cb_status status, u8 fail_mask,
		u16 req_id);

#endif /* __HOST_PROTO_PROP_REQ_H__ */
