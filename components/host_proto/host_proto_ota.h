/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_HOST_PROTO_OTA_H__
#define __AYLA_HOST_PROTO_OTA_H__

void host_proto_ota_init(void);
int host_proto_ota_rx(const void *buf, int len);

#endif /* __AYLA_HOST_PROTO_OTA_H__ */
