/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#include <sys/types.h>
#include <string.h>
#include <stdio.h>

#include <lwip/sys.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"

#include <ayla/utypes.h>
#include <ada/libada.h>
#include <ayla/assert.h>
#include <ada/err.h>
#include <ayla/log.h>
#include <ada/dnss.h>
#include <net/net.h>
#include <ada/server_req.h>
#include <adw/wifi.h>
#include <adw/wifi_conf.h>
#include <libapp/libapp.h>

#include "libapp_conf_int.h"
#include "libapp_conf_wifi.h"
#include <esp_wifi.h>

/*
 * Event handler.
 * This is called by the Wi-Fi subsystem on connects and disconnects
 * and similar events.
 * This allows the application to start or stop services on those events,
 * and to implement status LEDs, for example.
 */

static void libapp_wifi_event_handler(enum adw_wifi_event_id id, void *arg)
{
	switch (id) {
	case ADW_EVID_AP_START:
		break;

	case ADW_EVID_AP_UP:
		server_enable_redir();
		server_up();
		dnss_up();
		break;

	case ADW_EVID_AP_DOWN:
		dnss_down();
		break;

	case ADW_EVID_STA_UP:
		break;

	case ADW_EVID_STA_DHCP_UP:
#ifndef AYLA_LOCAL_CONTROL_SUPPORT
		ada_client_up();
#else
		ada_client_ip_up();
#endif
		ada_client_health_check_en();
		server_up();
		libapp_cloud_up();
		break;

	case ADW_EVID_STA_DOWN:
#ifndef AYLA_LOCAL_CONTROL_SUPPORT
		ada_client_down();
#else
		ada_client_ip_down();
#endif
		break;

	case ADW_EVID_ENABLE:
		break;

	case ADW_EVID_SETUP_START:
	case ADW_EVID_SETUP_STOP:
	case ADW_EVID_DISABLE:
	case ADW_EVID_SCAN_DONE:
	case ADW_EVID_RESTART_FAILED:
		break;

	default:
		break;
	}
}

int libapp_wifi_cmd(int argc, char **argv)
{
	adw_wifi_cli(argc, argv);
	return 0;
}

/*
 * Configure compiled-in AP mode SSID with MAC address.
 */
void libapp_wifi_ap_ssid_set(const char *prefix)
{
	struct ada_conf *cf = &ada_conf;
	char ssid[32];
	const u8 *pssid;

	pssid = adw_wifi_ap_ssid_get();
	if (!pssid[0]) {
		snprintf(ssid, sizeof(ssid),
		    "%s%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x", prefix,
		    cf->mac_addr[0], cf->mac_addr[1], cf->mac_addr[2],
		    cf->mac_addr[3], cf->mac_addr[4], cf->mac_addr[5]);
		adw_wifi_ap_ssid_set(ssid);
	}
}

/*
 * Configure Wi-Fi interface hostnames for DHCP.
 * Use 6-hex-digit STA MAC-address suffix.
 */
void libapp_wifi_hostname_prefix_set(const char *prefix)
{
	char hostname[32];
	u8 mac[6];

	esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
	snprintf(hostname, sizeof(hostname),
	    "%s%2.2x%2.2x%2.2x", prefix,
	    mac[3], mac[4], mac[5]);
	adw_wifi_hostname_set(hostname);
}

static void libapp_wifi_init_hostname(void)
{
	char buf[20];
	int len;

	len = libapp_conf_get_string(LIBAPP_HOST_PREF_CONF, buf, sizeof(buf));
	if (len <= 0) {
		return;
	}
	libapp_wifi_hostname_prefix_set(buf);
}

void libapp_wifi_init(void)
{
	int enable_redirect = 1;

	libapp_wifi_init_hostname();
#if defined(AYLA_BLUETOOTH_SUPPORT) && !defined(LIBAPP_AP_MODE_SUPPORT)
	adw_wifi_ap_mode_not_supported();
#endif
	adw_wifi_event_register(libapp_wifi_event_handler, NULL);
	adw_wifi_init();
	adw_wifi_page_init(enable_redirect);
	adw_wifi_powersave(ADW_WIFI_PS_ON);
	adw_check_wifi_enable_conf();
#ifndef AYLA_DEMO_TEST
	adw_wifi_enable();	/* TODO overrides config */
#endif
	if (adw_wifi_configured()) {
		ada_client_health_check_en();
	}
}
