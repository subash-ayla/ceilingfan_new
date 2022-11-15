/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */

/*
 * Event/status codes for network changes.
 */
#include <stdlib.h>
#include <libapp/net_event.h>
#include <ayla/utypes.h>
#include <ayla/log.h>
#include <ayla/clock.h>
#include <ayla/tlv.h>
#include <ada/err.h>
#include <ada/client.h>
#include <adw/wifi.h>

static const char *libapp_net_status_names[] = LIBAPP_NET_STATUS_NAMES;
static enum libapp_net_status libapp_net_status;

struct libapp_net_handler {
	struct libapp_net_handler *next;
	void (*handler)(enum libapp_net_status);
};

static struct libapp_net_handler *libapp_net_handlers;

/*
 * Deliver event to handlers.
 */
void libapp_net_event_send(enum libapp_net_status event)
{
	struct libapp_net_handler *hp;
	struct libapp_net_handler *next;

	if (event == libapp_net_status) {
		return;
	}
	log_put(LOG_INFO "net_status %s", libapp_net_status_name_get(event));

	switch (event) {
	case NS_PROVISIONING_STOPPED:
		/*
		 * The end of provisioning must not change the state if it
		 * has moved past provisioning.
		 * Do not report a status change in that case.
		 */
		if (libapp_net_status == NS_PROVISIONING) {
			libapp_net_status = NS_IDLE;
		}
		break;
	default:
		libapp_net_status = event;
		break;
	}

	for (hp = libapp_net_handlers; hp; hp = next) {
		next = hp->next;
		if (hp->handler) {
			hp->handler(event);
		}
	}
}

static void libapp_net_wifi_event(enum adw_wifi_event_id event, void *arg)
{
	enum libapp_net_status status = libapp_net_status;

	switch (event) {
	case ADW_EVID_ASSOCIATING:
	case ADW_EVID_SETUP_START:
		status = NS_ASSOCIATING;
		break;
	case ADW_EVID_STA_DOWN:
		status = NS_DISCONNECTED;
		break;
	case ADW_EVID_STA_UP:
	case ADW_EVID_STA_DHCP_UP:
		status = NS_ASSOCIATED;
		break;
	case ADW_EVID_AP_START:
	case ADW_EVID_AP_UP:
		status = NS_PROVISIONING;
		break;

	case ADW_EVID_SETUP_STOP:
	case ADW_EVID_AP_DOWN:
		if (status == NS_ASSOCIATING) {
			status = NS_DISCONNECTED;
		}
		break;
	default:
		break;
	}
	libapp_net_event_send(status);
}

static void libapp_net_client_event(void *arg, enum ada_err err)
{
	if (!err) {
		libapp_net_event_send(NS_CONNECTED);
	} else {
		libapp_net_event_send(NS_DISCONNECTED);
	}
}

/*
 * Initialize net events.
 * Must be called after ADW initialized.
 * This may be after libapp_net_event_register() is called.
 */
void libapp_net_event_init(void)
{
	static u8 done;

	if (done) {
		return;
	}
	done = 1;
	if (adw_wifi_configured()) {
		libapp_net_event_send(NS_ASSOCIATING);
	} else {
		libapp_net_event_send(NS_IDLE);
	}

	adw_wifi_event_register(libapp_net_wifi_event, NULL);
	ada_client_event_register(libapp_net_client_event, NULL);
}

/*
 * Get current network status.
 * This may be called at any time to get the status.
 * It is quick, just reading a variable..
 */
enum libapp_net_status libapp_net_status_get(void)
{
	return libapp_net_status;
}

/*
 * Register an event handler to get network status updates.
 */
void libapp_net_event_register(void (*handler)(enum libapp_net_status))
{
	struct libapp_net_handler *hp;

	if (!handler) {
		return;
	}
	hp = calloc(1, sizeof(*hp));
	if (!hp) {
		return;
	}
	hp->handler = handler;
	hp->next = libapp_net_handlers;
	libapp_net_handlers = hp;
	handler(libapp_net_status);
}

/*
 * Unregister an event handler.
 */
int libapp_net_event_unregister(void (*handler)(enum libapp_net_status))
{
	struct libapp_net_handler **prevp;
	struct libapp_net_handler *hp;

	for (prevp = &libapp_net_handlers; *prevp != NULL; prevp = &hp->next) {
		hp = *prevp;
		if (hp->handler == handler) {
			*prevp = hp->next;	/* unlink */
			free(hp);
			return 0;
		}
	}
	return -1;
}

/*
 * Return string for network status.
 */
const char *libapp_net_status_name_get(enum libapp_net_status status)
{
	const char *name = NULL;

	if (status < ARRAY_LEN(libapp_net_status_names)) {
		name = libapp_net_status_names[status];
	}
	if (!name) {
		name = "unknown";
	}
	return name;
}
