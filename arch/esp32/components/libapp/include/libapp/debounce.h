/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBAPP_DEBOUNCE_H__
#define __AYLA_LIBAPP_DEBOUNCE_H__

/*
 * Definitions for library routine to debounce a GPIO for button input.
 */

#define LIBAPP_DEBOUNCE_MS	10	/* default debounce time (ms) */

/*
 * Note, this structure is updated at interrupt time,
 * so be careful about atomicity and don't use bit fields.
 */
struct libapp_debounce {
	u8	raw;		/* last-read raw value */
	u8	val;		/* debounced value */
	u32	end_time;	/* time when bounce will be over */
	u32	debounce_ms;	/* duration in which to ignore changes (ms) */
};

/*
 * Debounce a Boolean value.
 *
 * db is the debounce state structure.
 * It should be initialized to zero before the first call only.
 *
 * The debounce_ms field can be set to the number of milliseconds during which
 * to ignore subsequent changes to the raw input.  If it is zero, a default of
 * LIBAPP_DEBOUNCE_MS is used.
 *
 * val is the new raw value of the input.
 *
 * Returns -1 if the debounced value hasn't changed.
 * Returns the value otherwise.
 */
int libapp_debounce(struct libapp_debounce *db, u8 new_val);

/*
 * Return amount of debouncing time remaining, in ms.
 * Returns -1 if not bouncing.
 * Returns 0 if time has expired.
 * If libapp_debounce() is called in an interrupt, be sure to block it.
 */
int IRAM_ATTR libapp_debounce_delay_get(struct libapp_debounce *db);

#endif /* __AYLA_LIBAPP_DEBOUNCE_H__ */
