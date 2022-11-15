/*
 * Copyright 2011-2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <string.h>
#include <stdlib.h>

#include <ayla/utypes.h>
#include <ayla/endian.h>
#include <ayla/assert.h>
#include <ayla/tlv.h>
#include <ayla/ayla_proto_mcu.h>
#include "hp_buf.h"
#include "hp_buf_cb.h"
#include "hp_buf_tlv.h"

/*
 * Set command fields in the buffer.
 * The buffer is guaranteed to be large enough to hold a command.
 * Sets the buffer length in preparation for appending TLVs.
 */
void hp_buf_tlv_cmd_set(struct hp_buf *bp, u8 proto, u8 op, u16 req_id)
{
	struct ayla_cmd *cmd = (struct ayla_cmd *)bp->payload;

	bp->len = sizeof(*cmd);
	cmd->protocol = proto;
	cmd->opcode = op;
	put_ua_be16(&cmd->req_id, req_id);
}

/*
 * Return the largest TLV payload that could be appended.
 */
size_t hp_buf_tlv_space(struct hp_buf *bp)
{
	if (bp->len + sizeof(struct ayla_tlv) >= HP_BUF_LEN) {
		return 0;
	}
	return HP_BUF_LEN - bp->len - sizeof(struct ayla_tlv);
}

/*
 * Append a TLV to the buffer.
 * Adjust the length.
 */
void hp_buf_tlv_append(struct hp_buf *bp, enum ayla_tlv_type tlv_type,
		const void *val, size_t val_len)
{
	size_t new_len;
	struct ayla_tlv *tlv;

	new_len = bp->len + sizeof(*tlv) + val_len;
	ASSERT(new_len <= HP_BUF_LEN);
	tlv = (struct ayla_tlv *)((char *)bp->payload + bp->len);
	tlv->type = tlv_type;
	tlv->len = val_len;
	memcpy(TLV_VAL(tlv), val, val_len);
	bp->len = new_len;
}

/*
 * Append an unsigned value TLV as a big-endian TLV to the buffer.
 */
void hp_buf_tlv_append_be32(struct hp_buf *bp,
		enum ayla_tlv_type tlv_type, u32 val)
{
	be32 be_val;

	put_ua_be32(&be_val, val);
	hp_buf_tlv_append(bp, tlv_type, &be_val, sizeof(be_val));
}

/*
 * Append an u16 value TLV as a big-endian TLV to the buffer.
 */
void hp_buf_tlv_append_be16(struct hp_buf *bp, enum ayla_tlv_type tlv_type,
		u16 val)
{
	be16 be_val;

	put_ua_be16(&be_val, val);
	hp_buf_tlv_append(bp, tlv_type, &be_val, sizeof(be_val));
}

/*
 * Append an u8 value TLV as a big-endian TLV to the buffer.
 */
void hp_buf_tlv_append_u8(struct hp_buf *bp, enum ayla_tlv_type tlv_type,
		u8 val)
{
	hp_buf_tlv_append(bp, tlv_type, &val, sizeof(val));
}

void hp_buf_tlv_append_str(struct hp_buf *bp, enum ayla_tlv_type tlv_type,
	const char *str)
{
	hp_buf_tlv_append(bp, tlv_type, str, strlen(str));
}
