/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBAPP_OTA_H__
#define __AYLA_LIBAPP_OTA_H__

#include <ada/client_ota.h>

struct libapp_app_ota_ops {
	enum patch_state (*ota_start)(u32 length, const char *version);
	enum patch_state (*ota_rx_chunk)(u32 offset, const void *chunk,
	    size_t size);
	enum patch_state (*ota_rx_done)(void);
	void (*ota_end)(enum patch_state status);

	/*
	 * Notification of available OTA.
	 * Called after download of OTA is complete and ready to apply.
	 * The OTA will not be applied until libapp_ota_apply() is called,
	 * which may be done by the handler synchronously or later.
	 */
	void (*ota_ready)(void);
};

void libapp_ota_register(const struct libapp_app_ota_ops *ops);

/*
 * Indicate that a pending OTA should go ahead.
 */
void libapp_ota_apply(void);

/*
 * Return non-zero if an OTA is in progress.
 */
int libapp_ota_is_in_progress(void);

#endif /* __AYLA_LIBAPP_OTA_H__ */
