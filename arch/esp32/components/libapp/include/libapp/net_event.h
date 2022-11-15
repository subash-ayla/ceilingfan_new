/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBAPP_NET_EVENT_H__
#define __AYLA_LIBAPP_NET_EVENT_H__

/*
 * Event/status codes for network changes.
 */
enum libapp_net_status {
	NS_NONE = 0,		/* reserved, do not use */
	NS_IDLE,		/* Doing nothing, not connected to Wi-Fi */
	NS_PROVISIONING,	/* open for provisioning via BT or AP-mode */
	NS_PROVISIONING_STOPPED, /* stopped provisioning via BT or AP-mode */
	NS_ASSOCIATING,		/* joining Wi-Fi or other network */
	NS_ASSOCIATED,		/* join succeeded */
	NS_CONNECTED,		/* connected to cloud */
	NS_DISCONNECTED,	/* disconnected from cloud */
};

/*
 * Array initializer for names indexed by status enum.
 */
#define LIBAPP_NET_STATUS_NAMES { \
	[NS_NONE] = "none", \
	[NS_IDLE] = "idle", \
	[NS_ASSOCIATING] = "associating", \
	[NS_PROVISIONING] = "provisioning", \
	[NS_PROVISIONING_STOPPED] = "provisioning stopped", \
	[NS_ASSOCIATED] = "associated", \
	[NS_CONNECTED] = "connected", \
	[NS_DISCONNECTED] = "disconnected", \
}

/*
 * Get current network status.
 * This may be called at any time to get the status.
 * It is quick, just reading a variable..
 */
enum libapp_net_status libapp_net_status_get(void);

/*
 * Register an event handler to get network status updates.
 *
 * Note that the handler will usually be called from some other thread,
 * so take precautions there, such as queuing events to your own thread.
 */
void libapp_net_event_register(void (*handler)(enum libapp_net_status));

/*
 * Unregister an event handler registered by libapp_net_event_register().
 *
 * Return zero on success, -1 if the handler was not found.
 */
int libapp_net_event_unregister(void (*handler)(enum libapp_net_status));

/*
 * Return string for network status.
 */
const char *libapp_net_status_name_get(enum libapp_net_status);

/*
 * Send event to registered handlers.
 */
void libapp_net_event_send(enum libapp_net_status);

#endif /* __AYLA_LIBAPP_NET_EVENT_H__ */
