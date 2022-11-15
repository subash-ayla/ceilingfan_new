/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_HP_BUF_TLV_H__
#define __AYLA_HP_BUF_TLV_H__

/*
 * Set the command for a host_protocol buffer.
 */
void hp_buf_tlv_cmd_set(struct hp_buf *bp, u8 protocol, u8 opcode, u16 req_id);

/*
 * Append a TLV to the buffer.
 * Adjust the length.
 */
void hp_buf_tlv_append(struct hp_buf *bp, enum ayla_tlv_type tlv_type,
		const void *val, size_t val_len);

/*
 * Append an unsigned value as a big-endian TLV to the buffer.
 */
void hp_buf_tlv_append_be32(struct hp_buf *bp,
		enum ayla_tlv_type tlv_type, u32 val);

/*
 * Append an u16 value as a big-endian TLV to the buffer.
 */
void hp_buf_tlv_append_be16(struct hp_buf *bp,
		enum ayla_tlv_type tlv_type, u16 val);

/*
 * Append an u8 value TLV to the buffer.
 */
void hp_buf_tlv_append_u8(struct hp_buf *bp,
		enum ayla_tlv_type tlv_type, u8 val);

/*
 * Append a TLV with a string value.
 */
void hp_buf_tlv_append_str(struct hp_buf *bp, enum ayla_tlv_type tlv_type,
		const char *str);

/*
 * Return the largest TLV payload that could be appended.
 */
size_t hp_buf_tlv_space(struct hp_buf *bp);

#endif /* __AYLA_HP_BUF_TLV_H__ */
