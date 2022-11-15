/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_HOST_PROP_H__
#define __AYLA_HOST_PROP_H__

struct prop;

void host_prop_init(void);
void host_prop_enable_listen(void);
int host_prop_is_busy(const char *caller, const char *prop_name);
u32 host_prop_backoff_time_remaining(void);
int host_prop_check_val_json(const char *val);

int host_prop_get_to_device(u16 req_id);
int host_prop_get(u16 req_id, const char *prop_name);

int host_prop_send(u32 req_id, struct prop *, u8 dest);

#endif /* __AYLA_HOST_PROP_H__ */
