/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __HOST_PROTO_CONF_TLV_H__
#define __HOST_PROTO_CONF_TLV_H__

void conf_tlv_msg_init(void);
void conf_tlv_msg_recv(const void *data, size_t len);

u16 conf_tlv_next_req_id(void);

#endif /* __HOST_PROTO_CONF_TLV_H__ */
