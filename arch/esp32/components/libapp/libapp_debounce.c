/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <ada/libada.h>
#include <ayla/clock.h>
#include <libapp/debounce.h>

/*
 * See header file comments.
 * This must be in IRAM because it could be called with some interrupts blocked.
 */
int IRAM_ATTR libapp_debounce(struct libapp_debounce *db, u8 new_val)
{
	int bouncing;
	u32 now;

	now = clock_ms();
	bouncing = db->end_time && clock_gt(db->end_time, now);

	if (db->raw == new_val) {
		if (!bouncing) {
			db->end_time = 0;
		}
		return -1;
	}

	/*
	 * Raw value has changed, see if is bouncing.
	 * Set time when bouncing will be considered over.
	 */
	db->raw = new_val;
	db->end_time = now +
	    (db->debounce_ms ? db->debounce_ms : LIBAPP_DEBOUNCE_MS);
	if (bouncing) {
		return -1;
	}

	/*
	 * Accept new value.
	 */
	db->val = new_val;
	return (int)new_val;
}

int IRAM_ATTR libapp_debounce_delay_get(struct libapp_debounce *db)
{
	u32 end;
	u32 now;
	int remaining;

	end = db->end_time;
	if (!end) {
		return -1;
	}
	now = clock_ms();
	remaining = (int)(end - now);
	if (remaining < 0) {
		db->end_time = 0;
		return 0;
	}
	return remaining;
}
