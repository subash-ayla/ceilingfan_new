/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */

#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#ifdef DEBUG
#define DEBUG_LOG 1
#else
#define DEBUG_LOG 0
#endif

#define ac_log_debug(fmt, ...) \
	    do { if (DEBUG_LOG) log_put(LOG_DEBUG fmt, ##__VA_ARGS__); } \
		while (0)

void hac_debug_print_memory(const void *addr, uint32_t len);
/*void hac_debug_print_pbuf(const struct pbuf *bufp);*/

#endif
