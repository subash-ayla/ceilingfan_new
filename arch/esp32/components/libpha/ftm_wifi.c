/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */

/*
 * Factory test mode (FTM) Wi-Fi tests.
 */
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_netif.h>
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/clock.h>
#include <ayla/log.h>
#include <ayla/wifi_error.h>
#include <ayla/conf.h>
#include <ayla/conf_token.h>
#include <ada/err.h>
#include <ada/libada.h>
#include <ada/ada_conf.h>
#include <ada/client.h>
#include <ada/sprop.h>
#include <adw/wifi.h>
#include <adw/wifi_conf.h>
#include <libapp/libapp.h>
#include <libapp/libapp_ota.h>
#include <libpha/ftm.h>
#include "ftm_int.h"

struct pha_wifi_net {
	u8 ssid[33];
	u8 ssid_len;
	char key[33];
};

static struct pha_wifi_net pha_ftm_wifi_nets[2];
static u8 pha_ftm_wifi_net_indx;

enum pha_ftm_wifi_state {
	FTWS_INIT = 0,		/* not started yet */
	FTWS_SCANNING,		/* scanning for nets */
	FTWS_CONNECTING,	/* connecting to the selected AP and service */
	FTWS_OTA_WAIT,		/* waiting to see if OTA is available */
	FTWS_SHUTDOWN,		/* stop Wi-Fi */
	FTWS_DONE,
};
static enum pha_ftm_wifi_state pha_ftm_wifi_state;
static long pha_ftm_wifi_min_rssi;
static u8 pha_ftm_wifi_min_rssi_set;
static u8 pha_ftm_wifi_scan_done;
static u8 pha_ftm_wifi_connected;
static u8 pha_ftm_wifi_ads_connected;
static unsigned long pha_ftm_wifi_time_limit;
static unsigned long pha_ftm_wifi_scan_time;	/* configured scan limit */
static unsigned long pha_ftm_wifi_ads_time;	/* ADS connection time limit */

static void pha_ftm_wifi_event(enum adw_wifi_event_id event, void *arg);

/*
 * Get FTM config for a Wi-Fi net.
 */
static int pha_ftm_wifi_ssid_read(struct pha_wifi_net *net,
		const char *ssid_conf, const char *key_conf)
{
	int len;

	memset(net, 0, sizeof(*net));
	len = libapp_conf_get_string(ssid_conf, (char *)net->ssid,
	    sizeof(net->ssid));
	if (len <= 0) {
		return -1;
	}
	net->ssid_len = len;
	libapp_conf_get_string(key_conf, net->key, sizeof(net->key));
	return 0;
}

static int pha_ftm_wifi_init(void)
{
	char buf[16];
	char *errptr;
	long rssi;
	unsigned long val;
	int rc[2];

	rc[0] = pha_ftm_wifi_ssid_read(&pha_ftm_wifi_nets[0],
	    PHA_FTM_CONF_N0_SSID, PHA_FTM_CONF_N0_KEY);
	rc[1] = pha_ftm_wifi_ssid_read(&pha_ftm_wifi_nets[1],
	    PHA_FTM_CONF_N1_SSID, PHA_FTM_CONF_N1_KEY);
	if (rc[0] && rc[1]) {
		return -1;	/* neither net configured */
	}
	if (libapp_conf_get_string(PHA_FTM_CONF_RSSI_MIN,
	    buf, sizeof(buf)) > 0) {
		rssi = strtol(buf, &errptr, 10);
		if (*errptr) {
			pha_ftm_log(LOG_WARN "%s: invalid min RSSI: %s",
			    __func__, buf);
		} else {
			pha_ftm_wifi_min_rssi = rssi;
			pha_ftm_wifi_min_rssi_set = 1;
		}
	}
	pha_ftm_wifi_scan_time = PHA_FTM_CONF_DEF_SCAN_TIME;
	if (libapp_conf_get_string(PHA_FTM_CONF_SCAN_TIME,
	    buf, sizeof(buf)) > 0) {
		val = strtoul(buf, &errptr, 10);
		if (*errptr || val > PHA_FTM_CONF_MAX_SCAN_TIME / 1000) {
			pha_ftm_log(LOG_WARN "%s: invalid scan time limit: %s",
			    __func__, buf);
		} else {
			pha_ftm_wifi_scan_time = val * 1000;
		}
	}
	if (libapp_conf_get_string(PHA_FTM_CONF_ADS_TIME,
	    buf, sizeof(buf)) > 0) {
		val = strtoul(buf, &errptr, 10);
		if (*errptr || val > PHA_FTM_CONF_MAX_ADS_TIME / 1000) {
			pha_ftm_log(LOG_WARN "%s: invalid ADS time limit: %s",
			    __func__, buf);
		} else {
			pha_ftm_wifi_ads_time = val * 1000;
		}
	}
	adw_wifi_event_register(pha_ftm_wifi_event, NULL);
	return 0;
}

/*
 * Look for nets in the scan.
 */
static enum ftm_err pha_ftm_wifi_check_nets(void)
{
	int i;
	int rssi;
	s32 best_rssi = MIN_S32;
	int best_indx = -1;
	struct pha_wifi_net *net;
	enum conf_token sec;

	/*
	 * Search nets in opposite order for strongest RSSI.
	 * Net 2 is preferred if there are two equally strong nets.
	 */
	for (i = ARRAY_LEN(pha_ftm_wifi_nets) - 1; i >= 0; i--) {
		net = &pha_ftm_wifi_nets[i];
		sec = net->key[0] ? CT_WPA2_Personal : CT_none;
		if (net->ssid_len &&
		    !adw_wifi_scan_find(net->ssid, net->ssid_len, sec, &rssi)) {
			if (rssi > best_rssi) {
				best_indx = i;
				best_rssi = rssi;
			}
			continue;
		}
	}
	if (best_indx < 0) {
		return FTM_ERR_WIFI_NET_NOT_FOUND;
	}
	if (best_rssi < pha_ftm_wifi_min_rssi && pha_ftm_wifi_min_rssi_set) {
		return FTM_ERR_WIFI_RSSI_LOW;
	}
	if (best_indx) {
		pha_ftm_wifi_net_indx = best_indx;
		pha_ftm_log(LOG_INFO "%s found net %s rssi %ld",
		    __func__,
		    (char *)pha_ftm_wifi_nets[best_indx].ssid, best_rssi);
		pha_ftm_leave_enabled_set();
	}
	return FTM_ERR_NONE;
}

/*
 * Event handler for Wi-Fi events for FTM.
 * Set flags for polling loop to handle the event.
 */
static void pha_ftm_wifi_event(enum adw_wifi_event_id event, void *arg)
{
	static enum adw_wifi_event_id last_event;

	if (event != last_event) {
		last_event = event;
		pha_ftm_log(LOG_DEBUG "ftm_wifi: event %u", event);
	}
	switch (event) {
	case ADW_EVID_SCAN_DONE:
		pha_ftm_wifi_scan_done = 1;
		break;
	case ADW_EVID_STA_DHCP_UP:
		/*
		 * If testing connection to ADS, start it.
		 */
		if (pha_ftm_wifi_ads_time) {
#ifndef AYLA_LOCAL_CONTROL_SUPPORT
			ada_client_up();
#else
			ada_client_ip_up();
#endif
		}
		pha_ftm_wifi_connected = 1;
		break;
	default:
		break;
	}
}

/*
 * Client event.
 */
static void pha_ftm_wifi_client_event(void *arg, enum ada_err status)
{
	enum ada_err err;

	if (status) {
		pha_ftm_wifi_ads_connected = 0;
		return;
	}

	/*
	 * Send template version in case app is blocked in ftm_run().
	 */
	err = ada_sprop_send_by_name("oem_host_version");
	if (err) {
		pha_ftm_log(LOG_ERR "send of template ver rc %d", err);
	}
	pha_ftm_wifi_ads_connected = 1;
}

/*
 * Start a join to the service.
 */
static enum ftm_err pha_ftm_wifi_join(void)
{
	struct pha_wifi_net *net;
	enum wifi_error wifi_err;
	enum conf_token sec;

	net = &pha_ftm_wifi_nets[pha_ftm_wifi_net_indx];
	sec = net->key[0] ? CT_WPA2_Personal : CT_none;
	wifi_err = adw_wifi_join_net(net->ssid, net->ssid_len, sec, net->key);
	if (wifi_err && wifi_err != WIFI_ERR_IN_PROGRESS) {
		return FTM_ERR_CONN_TIMEOUT;	/* not accurately named */
	}
	return FTM_ERR_IN_PROGRESS;
}

/*
 * Start a join to the service.
 * Disable saves to the profiles, sets timeout, disables agent if not used.
 */
static enum ftm_err pha_ftm_wifi_join_test(void)
{
	adw_wifi_save_policy_set(0, 0);		/* don't save profiles */

	if (pha_ftm_wifi_ads_time) {
		pha_ftm_wifi_time_limit = clock_ms() + pha_ftm_wifi_ads_time;
		ada_client_event_register(pha_ftm_wifi_client_event, NULL);
	} else {
		pha_ftm_wifi_time_limit = clock_ms() + PHA_FTM_WIFI_WAIT;
		ada_conf.enable = 0;		/* disable client */
	}
	return pha_ftm_wifi_join();
}

/*
 * Release DHCP lease.
 * Eventually this might be better placed in ADW.
 * There's no convenient ifkey defined for the STA, but it is "WIFI_STA_DEF".
 * Stopping DHCPC on all interfaces should be OK.
 */
static void pha_ftm_wifi_dhcp_release(void)
{
	esp_netif_t *netif;

	for (netif = NULL; (netif = esp_netif_next(netif)) != NULL; ) {
		esp_netif_dhcpc_stop(netif);
	}
}

/*
 * Run Wi-Fi test steps.
 */
enum ftm_err pha_ftm_wifi_poll(void)
{
	enum ftm_err err;
	static enum pha_ftm_wifi_state prev_state;

	if (pha_ftm_wifi_state != prev_state) {
		prev_state = pha_ftm_wifi_state;
		pha_ftm_log(LOG_DEBUG "FTM wifi poll state %u",
		    pha_ftm_wifi_state);
	}
	switch (pha_ftm_wifi_state) {
	case FTWS_INIT:
		/*
		 * Set test_connect to prevent activating device in cloud.
		 */
		ada_conf.test_connect = 1;
		if (pha_ftm_wifi_init()) {
			return FTM_ERR_NONE;	/* this test not configured */
		}
		pha_ftm_wifi_state = FTWS_SCANNING;
		adw_wifi_start_scan(0);
		pha_ftm_wifi_time_limit = clock_ms() + pha_ftm_wifi_scan_time;
		return FTM_ERR_IN_PROGRESS;

	case FTWS_SCANNING:
		err = pha_ftm_wifi_check_nets();
		if (err == FTM_ERR_WIFI_NET_NOT_FOUND &&
		    clock_gt(pha_ftm_wifi_time_limit, clock_ms())) {
			if (pha_ftm_wifi_scan_done) {
				pha_ftm_wifi_scan_done = 0;
				adw_wifi_start_scan(0);
			}
			err = FTM_ERR_IN_PROGRESS;
			break;
		}
		if (err) {
			pha_ftm_wifi_state = FTWS_DONE;
			return err;
		}

		/*
		 * start connect
		 */
		pha_ftm_wifi_state = FTWS_CONNECTING;
		err = pha_ftm_wifi_join_test();
		if (err && err != FTM_ERR_IN_PROGRESS) {
			pha_ftm_wifi_state = FTWS_DONE;
			return err;
		}
		/* fall through */
	case FTWS_CONNECTING:
		if (pha_ftm_wifi_ads_connected) {
			pha_ftm_wifi_time_limit = clock_ms() + PHA_FTM_OTA_WAIT;
			pha_ftm_wifi_state = FTWS_OTA_WAIT;
			return FTM_ERR_IN_PROGRESS;
		}
		if (!pha_ftm_wifi_ads_time && pha_ftm_wifi_connected) {

			/*
			 * DHCP successful, not testing connection to cloud.
			 * Release DHCP reservation for other modules to use.
			 */
			pha_ftm_wifi_dhcp_release();
			pha_ftm_wifi_state = FTWS_SHUTDOWN;
			pha_ftm_wifi_time_limit = clock_ms() +
			    PHA_FTM_SHUTDOWN_WAIT;
			return FTM_ERR_IN_PROGRESS;
		}
		if (clock_gt(clock_ms(), pha_ftm_wifi_time_limit)) {
			pha_ftm_wifi_state = FTWS_DONE;
			return FTM_ERR_CONN_TIMEOUT;
		}
		return FTM_ERR_IN_PROGRESS;

	case FTWS_OTA_WAIT:
		/*
		 * Wait a short time to see if there's an OTA to download.
		 */
		if (libapp_ota_is_in_progress() ||
		    !clock_gt(clock_ms(), pha_ftm_wifi_time_limit)) {
			return FTM_ERR_IN_PROGRESS;
		}
		pha_ftm_wifi_state = FTWS_DONE;
		return FTM_ERR_NONE;

	case FTWS_SHUTDOWN:
		if (!clock_gt(clock_ms(), pha_ftm_wifi_time_limit)) {
			return FTM_ERR_IN_PROGRESS;
		}
		adw_wifi_disable();
		pha_ftm_wifi_state = FTWS_DONE;
		return FTM_ERR_NONE;

	case FTWS_DONE:
	default:
		err = FTM_ERR_NONE;
		break;
	}
	return err;
}

/*
 * Event handler for Wi-Fi events for restarting FTM.
 * This is called only while FTM is disabled.
 */
static void pha_ftm_wifi_restart_event(enum adw_wifi_event_id event, void *arg)
{
	enum ftm_err err;

	pha_ftm_log(LOG_DEBUG "ftm_wifi_restart: event %u", event);

	/*
	 * Deregister if called too long after boot.
	 */
	if (clock_gt(clock_ms(), pha_ftm_wifi_time_limit)) {
		pha_ftm_log(LOG_DEBUG "%s: deregister", __func__);
		adw_wifi_event_deregister(pha_ftm_wifi_restart_event);
		return;
	}
	switch (event) {
	case ADW_EVID_SCAN_DONE:
		/*
		 * Do nothing if Wi-Fi configured with an enabled profile.
		 */
		if (adw_wifi_configured()) {
			pha_ftm_log(LOG_DEBUG "%s: configured", __func__);
			break;
		}

		/*
		 * Check for net to cause FTM re-enable.
		 * To avoid too many scans, we'll rely on ADW to rescan.
		 */
		err = pha_ftm_wifi_check_nets();
		if (err) {
			break;
		}

		/*
		 * If AP found, start a join.
		 */
		pha_ftm_wifi_state = FTWS_CONNECTING;
		ada_conf.enable = 0;		/* disable client */
		err = pha_ftm_wifi_join();
		if (err) {
			pha_ftm_log(LOG_DEBUG "ftm_wifi_restart: join err %u",
			    err);
		}
		break;

	case ADW_EVID_STA_DHCP_UP:
		/*
		 * Succeeded in joining restart AP.  Enable FTM and restart.
		 */
		if (pha_ftm_wifi_state == FTWS_CONNECTING) {
			pha_ftm_wifi_state = FTWS_DONE;
			pha_ftm_wifi_dhcp_release();
			vTaskDelay(pdMS_TO_TICKS(PHA_FTM_SHUTDOWN_WAIT));
			pha_ftm_enable_restart();
		}
		break;
	default:
		break;
	}
}

/*
 * Enable Wi-fi to look for FTM-restart network.
 * Form restart net from main net by appending "-restart".
 */
void pha_ftm_wifi_restart_init(void)
{
	struct pha_wifi_net main_net;
	struct pha_wifi_net *net;
	int len;

	if (adw_wifi_configured()) {
		return;
	}
	net = &pha_ftm_wifi_nets[0];
	if (pha_ftm_wifi_ssid_read(&main_net,
	    PHA_FTM_CONF_N0_SSID, PHA_FTM_CONF_N0_KEY)) {
		return;
	}
	*net = main_net;		/* struct copy */
	len = snprintf((char *)net->ssid, sizeof(net->ssid),
	    "%s" PHA_FTM_RESTART_SUFFIX, main_net.ssid);
	if (len >= sizeof(net->ssid)) {
		net->ssid_len = 0;
		return;
	}
	net->ssid_len = len;
	pha_ftm_wifi_time_limit = clock_ms() + PHA_FTM_RESTART_SCAN_LIMIT;
	adw_wifi_event_register(pha_ftm_wifi_restart_event, NULL);
}
