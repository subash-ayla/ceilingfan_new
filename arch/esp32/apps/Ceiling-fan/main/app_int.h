/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_DEMO_H__
#define __AYLA_DEMO_H__

/*
 * Module part number for device.
 * This should be different for each Productized Host App.
 * See "Productized Host Applications Functional Spec".
 *
 * Currently allocated part numbers:
 *	LED bulb	AY028MLB1
 *	LED strip	AY028MLS1
 *	Wall dimmer	AY028MLD1
 *	Smart plugs	AY028MLO1
 *	Smart power strip AY028MLP1
 *	Hisense A/C AY028MHA1
 *	Air Purifier	AY028MCA1
 */
#define APP_MODULE_PN	"AY008ESP1"

/*
 * Count of schedules and name format.
 */
#define APP_SCHED_COUNT	28
#define APP_SCHED_FORMAT "sched%2.2u"

/*
 * Callback when the device should identify itself to the end user, for
 * example, briefly blinking an LED.
 */
void demo_identify_cb(void);

extern const char mod_sw_build[];
extern const char mod_sw_version[];

#endif /* __AYLA_DEMO_H__ */
