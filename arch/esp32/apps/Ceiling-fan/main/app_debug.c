/*
 * Copyright 2022 Ayla Networks, Inc.  All rights reserved.
 */
#include <string.h>
#include <ayla/log.h>

#include "app_debug.h"

void hac_debug_print_memory(const void *addr, uint32_t len)
{
	char buf[64];
	uint8_t *paddr = (uint8_t *)addr;
	int i, j, k;

	i = 0;
	while (i < len) {
		memset(buf, 0, sizeof(buf));
		for (j = 0; ((j < 20) && (i + j < len)); j++) {
			k = ((paddr[i + j] & 0xF0) >> 4);
			buf[j * 3 + 0] = ((k < 10)
			    ? (k + '0') : (k - 10 + 'A'));
			k = (paddr[i + j] & 0x0F);
			buf[j * 3 + 1] = ((k < 10)
			    ? (k + '0') : (k - 10 + 'A'));
			buf[j * 3 + 2] = ' ';
		}
		log_put(LOG_DEBUG "%s", buf);
		i += j;
	}
}
