/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */

/*
 * Ayla schedule configuration.
 */
#include <stdio.h>
#include <string.h>

#include <ada/libada.h>
#include <ada/sched.h>
#include <ayla/nameval.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/tlv_access.h>
#include <esp_err.h>
#include <esp_attr.h>
#include "libapp_conf_int.h"

/*
 * Save the last schedule runtime in RAM that is not cleared on a reboot,
 * but would be lost on a power cycle.
 */
static u32 RTC_NOINIT_ATTR libapp_sched_saved_run_time;
static u16 libapp_sched_count;
static u32 sched_test_time;
static u8 libapp_sched_dynamic;

/*
 * Form the ADA config table name for persisting the schedule value.
 */
static int libapp_sched_conf_value_name(char *name, size_t len, unsigned index)
{
	return snprintf(name, len, "sched/n/%u/value", index);
}

/*
 * Form the ADA config table name for persisting the schedule name.
 * This is compatible with legacy production agents.
 */
static int libapp_sched_conf_name_name(char *name, size_t len, unsigned index)
{
	return snprintf(name, len, "sched/n/%u/prop", index);
}

/*
 * Initialize and load schedules from config
 *
 * If the format is NULL, schedule names will be determined by the template
 * and dynamically added to the table as received by the cloud.
 *
 * Load names and values from the configuration.
 */
enum ada_err libapp_sched_init(const char *format, u16 count)
{
	char conf_name[40];
	u8 tlvs[SCHED_TLV_LEN];
	enum ada_err err;
	char name[PROP_NAME_LEN];
	unsigned int i;
	int rv;
	int len;

	if (format) {
		err = ada_sched_init(count);
	} else {
		err = ada_sched_dynamic_init(count);
	}
	if (err) {
		log_put(LOG_ERR "%s: failed %u", __func__, err);
		return err;
	}
	for (i = 0; i < count; i++) {
		if (format) {
			len = snprintf(name, sizeof(name), format, i + 1);
			if (len >= sizeof(name)) {
				log_put(LOG_ERR
				    "%s: failed - name too long", __func__);
				return AE_LEN;
			}
		} else {
			libapp_sched_conf_name_name(conf_name,
			    sizeof(conf_name), i);
			memset(name, 0, sizeof(name));
			rv = adap_conf_get(conf_name, name, sizeof(name));
			if (rv < 0 || rv >= sizeof(name)) {
				continue;
			}
		}
		err = ada_sched_set_name(i, name);
		if (err) {
			log_put(LOG_ERR "%s: ada_sched_set_name err %d",
			     __func__, err);
			return err;
		}

		libapp_sched_conf_value_name(conf_name, sizeof(conf_name), i);

		/*
		 * Look for the schedule in the configuration.
		 */
		rv = adap_conf_get(conf_name, tlvs, sizeof(tlvs));
		if (rv < 0) {
			continue;
		}

		err = ada_sched_set_index(i, tlvs, (size_t)rv);
		AYLA_ASSERT(!err);
	}
	libapp_sched_count = count;
	libapp_sched_dynamic = !format;
	ada_sched_enable();
	return AE_OK;
}

/*
 * Save schedules as needed.
 */
void adap_sched_conf_persist(void)
{
	char conf_name[40];
	u8 tlvs[SCHED_TLV_LEN];
	size_t new_len;
	char *name;
	enum ada_err err;
	int i;
	int rv;

	for (i = 0; i < libapp_sched_count; i++) {
		libapp_sched_conf_value_name(conf_name, sizeof(conf_name), i);

		new_len = sizeof(tlvs);
		err = ada_sched_get_index(i, &name, tlvs, &new_len);
		if (err) {
			new_len = 0;
		}

		rv = adap_conf_set(conf_name, tlvs, new_len);
		if (rv) {
			log_put(LOG_ERR "%s: conf_set \"%s\" failed rv %d",
			    __func__, conf_name, rv);
		}

		if (libapp_sched_dynamic) {
			libapp_sched_conf_name_name(conf_name,
			    sizeof(conf_name), i);
			if (new_len) {
				new_len = strlen(name);
			}
			rv = adap_conf_set(conf_name, name, new_len);
			if (rv) {
				log_put(LOG_ERR
				    "%s: conf_set \"%s\" failed rv %d",
				    __func__, conf_name, rv);
			}
		}
	}
}

void adap_sched_run_time_write(u32 run_time)
{
	libapp_sched_saved_run_time = run_time;
}

u32 adap_sched_run_time_read(void)
{
	return libapp_sched_saved_run_time;
}

/*
 * Display an action.
 */
static void libapp_sched_action_show(const struct ayla_tlv *atlv)
{
	char time_buf[30];
	const struct ayla_tlv *prop;
	const struct ayla_tlv *val_tlv;
	enum ayla_tlv_type type;
	char name[PROP_NAME_LEN];
	s32 val;
	size_t rlen;

	rlen = atlv->len;
	if (rlen < sizeof(*atlv) * 2) {
		printcli("sched test: malformed setprop TLV");
		return;
	}
	rlen -= sizeof(*atlv) * 2;

	prop = atlv + 1;
	if (prop->type != ATLV_NAME) {
		printcli("sched test: missing name");
		return;
	}
	if (prop->len > rlen || prop->len >= sizeof(name)) {
		printcli("sched_test: malformed setprop name len %u rlen %zu",
		    prop->len, rlen);
		return;
	}
	rlen -= prop->len;
	memcpy(name, prop + 1, prop->len);
	name[prop->len] = '\0';

	val_tlv = (struct ayla_tlv *)((u8 *)prop + prop->len +
	    sizeof(struct ayla_tlv));
	type = val_tlv->type;
	if (type != ATLV_INT && type != ATLV_BOOL) {
		printcli("sched_test: unsupported setprop type");
		return;
	}
	if (tlv_s32_get(&val, val_tlv)) {
		printcli("sched_test invalid setprop val");
		return;
	}
	clock_fmt(time_buf, sizeof(time_buf), sched_test_time);
	printcli("%s action set %s = %ld\n", time_buf, name, val);
}

static void libapp_sched_test(char *name, char *count_arg, char *time_arg)
{
	unsigned long count;
	u32 time;
	char *errptr;
	char *sched_name;
	u8 tlvs[SCHED_TLV_LEN];
	size_t len;
	enum ada_err err;
	unsigned int i;

	count = strtoul(count_arg, &errptr, 10);
	if (errptr == count_arg || *errptr || count == 0) {
		printcli("invalid count");
		return;
	}

	if (time_arg) {
		time = (u32)strtoul(time_arg, &errptr, 10);
		if (errptr == time_arg || *errptr) {
			printcli("invalid time");
			return;
		}
	} else {
		time = clock_utc();
	}

	for (i = 0; i < libapp_sched_count; i++) {
		len = sizeof(tlvs);
		err = ada_sched_get_index(i, &sched_name, tlvs, &len);
		if (err) {
			printcli("sched_get_index failed");
			return;
		}
		if (strcmp(sched_name, name)) {
			continue;
		}
		if (!len) {
			printcli("schedule not set");
			return;
		}

		/*
		 * Run schedules from the specified time.
		 */
		while (count-- > 0) {
			sched_test_time = time;
			err = ada_sched_eval((struct ayla_tlv *)tlvs,
			    len, &time, libapp_sched_action_show);
			if (err) {
				printcli("sched error %d", err);
				break;
			}
			if (!time || time == MAX_U32) {
				printcli("sched_eval: no more events");
				return;
			}
		}
		if (!err) {
			printcli("next time %lu", time);
		}
		return;
	}
	printcli("schedule not configured");
}

static void libapp_sched_show(void)
{
	u8 tlvs[SCHED_TLV_LEN];
	size_t len;
	char *name;
	enum ada_err err;
	int i;

	printcli("schedules max: %u", libapp_sched_count);
	printcli("%8s %5s  %s", "index", "len", "name");
	for (i = 0; i < libapp_sched_count; i++) {
		len = sizeof(tlvs);
		name = NULL;
		err = ada_sched_get_index(i, &name, tlvs, &len);
		if (err) {
			len = 0;
		}
		if (!name || !name[0]) {
			continue;
		}
		printcli("%8u %5zu  %s", i, len, name);
	}
}

const char libapp_sched_cli_help[] =
    "sched [show | test <sched-name> <action-count> [<time>]]";

/*
 * Test a schedule by showing the next N actions to be run and when they
 * will occur.
 */
esp_err_t libapp_sched_cli(int argc, char **argv)
{
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "show"))) {
		libapp_sched_show();
		return 0;
	}
	if ((argc == 4 || argc == 5) && !strcmp(argv[1], "test")) {
		libapp_sched_test(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
		return 0;
	}
	printcli("usage: %s", libapp_sched_cli_help);
	return 0;
}
