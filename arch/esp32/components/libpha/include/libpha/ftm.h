/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBPHA_FTM_H__
#define __AYLA_LIBPHA_FTM_H__

/*
 * These error codes are in external documentation so should not be changed.
 */
enum ftm_err {
	FTM_ERR_NONE = 0,		/* Success */
	FTM_ERR_WIFI_NET_NOT_FOUND = 1,	/* Wi-Fi network not found */
	FTM_ERR_WIFI_RSSI_LOW = 2,	/* Wi-Fi RSSI low */
	FTM_ERR_CONN_TIMEOUT = 3,	/* Cloud connection timed out */
	FTM_ERR_BLE_TIMEOUT = 4,	/* Bluetooth test timed out */
	FTM_ERR_FACT_RST = 5,		/* FTM cleared by factory reset */
	FTM_ERR_DEV = 6,		/* device-specific error */
					/* blinkable error codes above */
	FTM_ERR_IN_PROGRESS = 20	/* in progress with tests */
};

#define FTM_POLL_MS	100	/* suggested poll rate (milliseconds) */

/*
 * Determine whether FTM is enabled.
 * Should be called form the app_init() callback from libapp_start(), or
 * if that is NULL, after libapp_start() returns.
 *
 * Returns true if enabled.
 */
int pha_ftm_is_enabled(void);

/*
 * pha_ftm_run()
 *
 * Run FTM Wi-Fi, connectivity, and BLE tests.
 *
 * This blocks until tests are done or an error occurs.
 * Consider using pha_ftm_poll() instead.
 * Returns 0 on success or an error code on failure.
 */
enum ftm_err pha_ftm_run(void);

/*
 * Poll FTM for completion status.
 *
 * This does not block and should be called periodically every FTM_POLL_MS ms.
 * Returns FTM_ERR_IN_PROGRESS if not complete.
 * Returns 0 on success, and error code otherwise.
 */
enum ftm_err pha_ftm_poll(void);

enum ftm_err pha_ftm_enable_security(void);

#endif /* __AYLA_LIBPHA_FTM_H__ */
