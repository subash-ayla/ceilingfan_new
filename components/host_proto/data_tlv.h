/*
 * Copyright 2011 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_DATA_TLV_H__
#define __AYLA_DATA_TLV_H__

struct hp_buf;
struct prop;
struct prop_dp_meta;

/*
 * Initialize the data_tlv module.
 */
void data_tlv_init(void);

/*
 * Set command fields in the buffer for a data_tlv request.
 * Returns the request ID used.
 */
u16 data_tlv_cmd_req_set(struct hp_buf *bp, enum ayla_data_op op);

/*
 * Set command fields int he buffer for a data_tlv response.
 */
void data_tlv_cmd_set(struct hp_buf *bp, enum ayla_data_op op, u16 req_id);

/*
 * Send a property request to the MCU.
 * Eventually frees the packet in the device layer.
 * Returns the request ID used.
 */
u16 data_tlv_prop_req_send(struct hp_buf *hp, const char *name);

enum ada_err data_tlv_send(struct hp_buf *hp, const char *, const void *,
	size_t, enum ayla_tlv_type, u32 *, u8, u16, u8,
	const char *, const struct prop_dp_meta *);

void data_tlv_req_next(struct hp_buf *bp, u32 continuation, u16 *req_id);

enum ada_err data_tlv_dp_create(u16 req_id, const char *location);
void data_tlv_reset_offset(void);
void data_tlv_clear_ads(u16 req_id, u8 send_confirmation);
void data_tlv_nak_req(u16 req_id, u8 err, const char *name, u8 failed_dests);
void data_tlv_nak(u16 req_id, u8 err, u8 clear_ads);
void data_tlv_error(u8 err);
enum ada_err data_tlv_dp_stream_resp(u16 req_id, enum ayla_tlv_type,
	const void *prop_val, size_t prop_val_len,
	u32 data_off, u32 tot_len, const char *location, u8 eof);
enum ada_err data_tlv_send_eof(u16 req_id);

/*
 * Send host MCU whenever a new destination joins or leaves
 */
void data_tlv_send_connectivity(u8 mask);

/*
 * Send Host MCU if an echo to ads fails for a property
 */
void data_tlv_send_echofail(char *prop_name);

/*
 * Process an incoming message from MCU. Route it either as data
 * or config.
 */
int data_tlv_process_mcu_pkt(u8 *data_ptr, size_t recv_len);

/*
 * Receive property update from mcu
 */
int data_tlv_recv_tlv(u16 req_id, struct prop *prop, struct ayla_tlv *off_tlv,
		struct ayla_tlv *len_tlv);

/*
 * Setup a spi callback to notify host mcu of a pending update
 */
void data_tlv_send_prop_notification(void);

/*
 * Setup a spi callback to notify host mcu of a pending client event
 */
void data_tlv_send_client_notification(void);

/*
 * Setup a spi callback to notify host mcu of a pending wifi event
 */
void data_tlv_send_wifi_notification(void);

/*
 * Send batch datapoint status.
 * The callback will be invoked later if no buffer is available.
 */
void data_tlv_batch_status(u16 id, u8 status, u8 final,
		void (*cb)(struct hp_buf *bp));

void data_tlv_wifi_init(void);

extern u8 mcu_feature_mask;	/* features of MCU */
extern u8 mcu_feature_mask_min;	/* minimum features for transport */

#endif /* __AYLA_DATA_TLV_H__ */
