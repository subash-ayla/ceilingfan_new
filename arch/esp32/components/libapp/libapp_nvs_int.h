/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBAPP_NVS_INT_H__
#define __AYLA_LIBAPP_NVS_INT_H__

/*
 * Nvs NameSpace For application
 */
#define AYLA_STORAGE "ayla_namespace"

/*
 * Handles for use by libapp in reading and writing NVS values.
 * libapp_nvs_init() sets these.
 */
extern nvs_handle_t libapp_nvs;		/* main NVS partition */
extern nvs_handle_t libapp_nvs_state;	/* state-saving NVS */

/*
 * Initialize and return backup ID partition.
 */
nvs_handle_t libapp_nvs_id_init(void);

#endif /* __AYLA_LIBAPP_NVS_INT_H__ */
