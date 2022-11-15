/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBPHA_FTM_INT_H__
#define __AYLA_LIBPHA_FTM_INT_H__

/*
 * Configuration items.
 *
 * All of these are under "app/".
 * These are in the factory configuration.
 * Clear app/ftm/enable and app/ftm/success when disabling FTM.
 * Leave the rest in the configuration so FTM can be re-enabled.
 *
 * The maximum string length for these is 14, so that with a prefix, they
 * will be 15 or less.
 */
#define PHA_FTM_CONF_EN		"ftm/enable"	/* overall enable */
#define PHA_FTM_CONF_OK		"ftm/success"	/* FTM passed */
#define PHA_FTM_CONF_N0_SSID	"ftm/p0/ssid"
#define PHA_FTM_CONF_N0_KEY	"ftm/p0/key"
#define PHA_FTM_CONF_N1_SSID	"ftm/p1/ssid"
#define PHA_FTM_CONF_N1_KEY	"ftm/p1/key"
#define PHA_FTM_CONF_RSSI_MIN	"ftm/rssi_min" /* minimum RSSI for Wi-Fi */
#define PHA_FTM_CONF_SCAN_TIME	"ftm/scan_time" /* max time for wifi scan */
#define PHA_FTM_CONF_ADS_TIME	"ftm/ads_time" /* max time for ADS conn */
#define PHA_FTM_CONF_BLE_TIME	"ftm/ble_time" /* max time for BLE test */

#define PHA_FTM_CONF_DEF_SCAN_TIME	5000	/* scan time default (ms) */
#define PHA_FTM_CONF_MAX_SCAN_TIME	30000	/* scan time max (ms) */
#define PHA_FTM_CONF_MAX_ADS_TIME	60000	/* ADS wait time max (ms) */
#define PHA_FTM_RESTART_SCAN_LIMIT	40000	/* limit on restart scan */
#define PHA_FTM_RESTART_SUFFIX	"-restart"	/* restart SSID suffix */

/*
 * Non-configurable wait times.
 */
#define PHA_FTM_WIFI_WAIT	20000	/* Wi-Fi connect plus DHCP limit (ms) */
#define PHA_FTM_OTA_WAIT	2000	/* wait for OTA command (ms) */
#define PHA_FTM_SHUTDOWN_WAIT	500	/* wait before stopping Wi-Fi (ms) */

enum ftm_err pha_ftm_wifi_poll(void);
void pha_ftm_leave_enabled_set(void);

void pha_ftm_wifi_restart_init(void);
void pha_ftm_enable_restart(void);

void pha_ftm_log(const char *fmt, ...) ADA_ATTRIB_FORMAT(1, 2);

#endif /* __AYLA_LIBPHA_FTM_INT_H__ */
