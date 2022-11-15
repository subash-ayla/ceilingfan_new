/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <driver/uart.h>

#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/tlv.h>
#include <ayla/conf.h>
#include <ayla/log.h>
#include <ayla/clock.h>
#include <ayla/base64.h>
#include <net/net_crypto.h>
#include <ada/ada_conf.h>
#include <ada/metrics.h>
#include <ada/client.h>
#include <adw/wifi.h>
#ifdef AYLA_BLUETOOTH_SUPPORT
#include <adb/adb.h>
#include <adb/al_bt.h>
#endif
#include "esp_wifi.h"
#include "esp_interface.h"
#include "esp_spi_flash.h"
#include "esp_console.h"
#include "esp_pm.h"
#include "libapp_conf_int.h"
#include "libapp_conf_wifi.h"
#include "libapp_sched.h"
#include "esp_system.h"
#include <libapp/libapp.h>

/*
 * Console UART and CLI config.
 */
#define SETUP_CLI_EN_CONF	"cli/enable"
#define SETUP_CLI_EN		'1'	/* enable CLI and cons and lock */
#define SETUP_CLI_DIS		'0'	/* disable CLI and lock it off */
#define SETUP_CLI_DIS_UART	'c'	/* disable CLI and UART and lock off */
#define SETUP_CLI_UNLOCK	'\0'	/* user can disable CLI and/or cons */

u8 libapp_cli_enable;
u8 libapp_cons_enable;
static esp_pm_lock_handle_t libapp_cli_pm_lock;

#ifndef LIBAPP_SETUP_EN_SECRET	/* usually defined in Makefile per app */
#define LIBAPP_SETUP_EN_SECRET "\xb0\xf6\xe8\xb6\xb3\xc0"
#endif

/*
 * Generate a setup_mode enable key based on the OEM-ID and OEM-model.
 * If the OEM-ID is empty, no key should be required.
 */
static int libapp_cli_setup_mode_key_gen(char *buf, size_t len)
{
	mbedtls_sha1_context ctx;
	const char secret[] = LIBAPP_SETUP_EN_SECRET;
	u8 key[SHA1_SIG_LEN];
	int err;
	size_t out_len;

	if (!oem[0]) {
		buf[0] = '\0';		/* no key needed if OEM ID not set */
		return 0;
	}
	mbedtls_sha1_init(&ctx);
	err = mbedtls_sha1_starts_ret(&ctx);
	err |= mbedtls_sha1_update_ret(&ctx,
	    (u8 *)secret, strlen(secret));
	err |= mbedtls_sha1_update_ret(&ctx,
	    (u8 *)oem, strlen(oem));
	err |= mbedtls_sha1_update_ret(&ctx,
	    (u8 *)oem_model, strlen(oem_model));
	err |= mbedtls_sha1_finish_ret(&ctx, key);
	ASSERT(!err);
	out_len = len;
	return ayla_base64_encode(key, sizeof(key), buf, &out_len);
}

/*
 * Read setup mode key into buffer.
 * Returns non-zero if old key location used.
 */
static int libapp_cli_setup_mode_key_read(char *key, size_t len)
{
	int key_len;

	key[0] = '\0';
	key_len = libapp_conf_get_string(SETUP_KEY_CONF, key, len);
	if (key_len <= 0) {
		return -1;
	}
	return 0;
}

/*
 * Check setup_mode key against the configured value.
 * Return 0 on success.
 */
int libapp_cli_setup_mode_key_check(const char *guess)
{
	char key[SETUP_KEY_LENGTH + 1];
	static int attempts;

	if (libapp_cli_setup_mode_key_read(key, sizeof(key)) &&
	    libapp_cli_setup_mode_key_gen(key, sizeof(key))) {
		return -1;
	}
	if (strcmp(guess, key)) {
		/* block to deter brute-force attack */
		vTaskDelay(++attempts * 1000 / portTICK_PERIOD_MS);
		return -2;		/* wrong key */
	}
	attempts = 0;
	return 0;
}

int libapp_cli_setup_mode_key_set(const char *in_key)
{
	char key[SETUP_KEY_LENGTH + 1];
	size_t key_len;

	key_len = strlen(in_key);
	if (key_len >= sizeof(key)) {
		printcli("re-enable key too long");
		return -1;
	}

	/* save the same length every time to give no hints */
	/* If no key, this will delete it from config */
	if (key_len) {
		memset(key, 0, sizeof(key));
		memcpy(key, in_key, key_len);
		key_len = sizeof(key) - 1;
	}
	return libapp_conf_set_blob(SETUP_KEY_CONF, key, key_len, 1);
}

/*
 * setup_mode command.
 * setup_mode enable|disable|show [<key>]
 * Require key if disabling and key not set.
 */
static int libapp_cli_setup_mode(int argc, char **argv)
{
	char key[SETUP_KEY_LENGTH + 1];
	int rc;

	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "show"))) {
		printcli("setup_mode %sabled",
		    conf_setup_mode ? "en" : "dis");
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "enable")) {
		rc = libapp_cli_setup_mode_key_check(argv[2]);
		if (rc == -1) {
			printcli("setup_mode enable not allowed");
		} else if (rc) {
			printcli("wrong key");
		} else {
			/* saves if setting setup mode */
			ada_conf_setup_mode(1);
		}
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "disable")) {
		if (!mfg_or_setup_mode_ok()) {
			return 0;
		}
		libapp_cli_setup_mode_key_read(key, sizeof(key));
		if (argc == 3 && strcmp(argv[2], key)) {
			if (libapp_cli_setup_mode_key_set(argv[2])) {
				return 0;
			}
		}
		ada_conf_setup_mode(0); /* saves if clearing setup mode */
		return 0;
	}
	printcli("usage error");
	return 0;
}

static int libapp_cli_save(int argc, char **argv)
{
	char *args[] = { "conf", "save", NULL};
	if (argc != 1) {
		printcli("save: unused args - nothing saved");
		return 0;
	}
	esp_pm_lock_acquire(libapp_cli_pm_lock);
	conf_cli(2, args);
	esp_pm_lock_release(libapp_cli_pm_lock);
	return 0;
}

static int libapp_cli_version(int argc, char **argv)
{
	if (argc != 1) {
		printcli("usage: version");
		return 0;
	}
	printcli("%s", mod_sw_version);
	return 0;
}

static int libapp_cli_oem(int argc, char **argv)
{
	ada_conf_oem_cli(argc, argv);
	return 0;
}

static const char libapp_cli_client_help[] =
    "client <options> - show client state";

static esp_err_t libapp_cli_client(int argc, char **argv)
{
	ada_client_cli(argc, argv);
	return ESP_OK;
}

static esp_err_t libapp_cli_id(int argc, char **argv)
{
	ada_conf_id_cli(argc, argv);
	return ESP_OK;
}

static int libapp_cli_reset(int argc, char **argv)
{
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "factory"))) {
		if (argc == 2) {
			libapp_conf_factory_reset();
		}
		esp_restart();
		ASSERT_NOTREACHED();
	}
	printcli("usage: reset [factory]\n");
	return 0;
}

static esp_err_t libapp_cli_log(int argc, char **argv)
{
	ada_log_cli(argc, argv);
	return ESP_OK;
}

#ifdef AYLA_LOG_CORE
static esp_err_t libapp_cli_log_core(int argc, char **argv)
{
	ada_log_core_cli(argc, argv);
	return ESP_OK;
}
#endif

#ifdef AYLA_LOG_SNAPSHOTS
static esp_err_t libapp_cli_log_snap(int argc, char **argv)
{
	ada_log_snap_cli(argc, argv);
	return ESP_OK;
}
#endif

#ifdef AYLA_METRICS_SUPPORT
static int libapp_metrics_cli(int argc, char **argv)
{
	metrics_cli(argc, argv);
	return ESP_OK;
}
#endif

static void libapp_cli_time_show(void)
{
	char buf[24];
	u32 utc_time = clock_utc();
	s32 abs_mins = timezone_info.mins;

	clock_fmt(buf, sizeof(buf), utc_time);
	printcli("UTC Time:   %s, Time Since Boot: %llu ms", buf,
	    clock_total_ms());
	clock_fmt(buf, sizeof(buf), clock_local(&utc_time));

	if (timezone_info.valid) {
		/*
		 * Note: Device definition for offset is minutes *west* of UTC.
		 * So the sign is backwards from normal usage.
		 * This means a PST (UTC-8) uses mins = +480.
		 */
		if (timezone_info.mins < 0) {
			abs_mins *= -1;
		}
		printcli("Local Time: %s, Timezone: UTC%c%2.2ld:%2.2ld",
		    buf, timezone_info.mins <= 0 ? '+' : '-',
		    abs_mins / 60, abs_mins % 60);
		if (daylight_info.valid) {
			clock_fmt(buf, sizeof(buf), daylight_info.change);
			printcli("DST Active: %u until %s UTC",
			    daylight_info.active, buf);
		}
	}
}

static void libapp_cli_time_set(const char *val)
{
	char buf[24];
	u32 old_time;
	u32 new_time;

	old_time = clock_utc();
	new_time = clock_parse(val);
	if (!new_time) {
		printcli("time set: invalid time");
		return;
	}

	/*
	 * Set the clock with the current clock source.
	 */
	if (clock_set(new_time, clock_source())) {
		printcli("time set: clock not set");
	}
	log_put(LOG_INFO "clock set by CLI");
	clock_fmt(buf, sizeof(buf), old_time);
	log_put(LOG_INFO "clock was %s UTC", buf);
	clock_fmt(buf, sizeof(buf), new_time);
	log_put(LOG_INFO "clock now %s UTC", buf);
}

static const char libapp_cli_time_help[] = "time [set YYYY-MM-DDThh:mm:ss]";

static esp_err_t libapp_cli_time(int argc, char **argv)
{
	if (argc == 1) {
		libapp_cli_time_show();
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "set")) {
		libapp_cli_time_set(argv[2]);
		return 0;
	}
	printcli("usage: %s", libapp_cli_time_help);
	return 0;
}

static void libapp_cli_crash_overflow(char *arg)
{
	char buf[2000];

	if (arg) {
		libapp_cli_crash_overflow(buf);
		(void)arg;
		(void)buf[0];
		printcli("crash_overflow arg %p, buf %u", arg, buf[0]);
	}
}

static void libapp_cli_crash_hang(void)
{
	u32 time;

	if (esp_task_wdt_add(NULL)) {
		printcli("enabling watchdog failed");
		return;
	}
	time = clock_ms() + 60 * 1000;
	while (clock_gt(time, clock_ms())) {
		vTaskDelay(1000);
	}
	if (esp_task_wdt_delete(NULL)) {
		printcli("disabling watchdog failed");
	}
}

#ifdef CONFIG_IDF_TARGET_ARCH_RISCV
/*
 * Single core implementation
 */
static void libapp_cli_crash_hang_intr(void)
{
	u32 time;

	vPortEnterCritical();
	time = clock_ms() + 5 * 1000;
	while (clock_gt(time, clock_ms())) {
		;
	}
	vPortExitCritical();
}
#else
/*
 * Dual core implementation
 */
static void libapp_cli_crash_hang_intr(void)
{
	portMUX_TYPE spin_lock;
	u32 time;

	vPortCPUInitializeMutex(&spin_lock);
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(4, 3, 0)
	vTaskEnterCritical(&spin_lock);
#else
	vPortEnterCritical(&spin_lock);
#endif
	time = clock_ms() + 5 * 1000;
	while (clock_gt(time, clock_ms())) {
		;
	}
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(4, 3, 0)
	vTaskExitCritical(&spin_lock);
#else
	vPortExitCritical(&spin_lock);
#endif
}
#endif

/*
 * crash command.
 */
static int libapp_cli_crash(int argc, char **argv)
{
	volatile int val[3];
	volatile int *ptr;
	enum crash_method {
		CM_ASSERT,	/* assertion */
		CM_ALIGN,	/* unaligned access */
		CM_DIV0,	/* divide by 0 */
		CM_HANG,	/* CPU hard hang.  Should watchdog */
		CM_HANG_INTR,	/* CPU hard hang in blocking interrupts. */
		CM_NULL,	/* NULL pointer dereference */
		CM_STACK,	/* stack overflow */
		CM_WILD,	/* wild pointer, invalid addresss access */
		CM_NUMBER	/* number of methods, must be last  */
	} method;
	static const char *methods[CM_NUMBER] = {
		[CM_ASSERT] = "assert",
		[CM_ALIGN] = "align",
		[CM_DIV0] = "div0",
		[CM_HANG] = "hang",
		[CM_HANG_INTR] = "hang_intr",
		[CM_NULL] = "null",
		[CM_STACK] = "stack",
		[CM_WILD] = "wild",
	};

	if (argc < 2 || !strcmp(argv[1], "help")) {
		printcli("usage: crash <method>");
options:
		printcli_s("  valid methods are: ");
		for (method = 0; method < ARRAY_LEN(methods); method++) {
			printcli_s(" %s", methods[method]);
		}
		printcli(".");
		return 0;
	}

	for (method = 0; method < ARRAY_LEN(methods); method++) {
		if (argv[1] && !strcmp(argv[1], methods[method])) {
			break;
		}
	}
	if (method >= ARRAY_LEN(methods)) {
		printcli("invalid option");
		goto options;
	}

	log_put(LOG_WARN "CLI crash %s causing crash", argv[1]);
	uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM,
	    1000 / portTICK_PERIOD_MS);

	switch (method) {
	case CM_NULL:
		(*(int *)0)++;
		((void(*)(void))0)();
		break;
	case CM_WILD:
		(*(int *)0x07fffffc)++;	/* arch-dependent address */
		break;
	case CM_DIV0:
		val[0] = 1;
		val[1] = 0;
		val[2] = val[0] / val[1];
		(void)val[2];
		break;
	case CM_ASSERT:
		ASSERT(0);
		break;
	case CM_ALIGN:
		ptr = (int *)libapp_cli_crash;
		ptr = (int *)((char *)ptr + 1);
		(void)*ptr;
		printcli("test %p %x %x",
		    ptr, *(int *)ptr, *(int *)libapp_cli_crash);
		break;
	case CM_STACK:
		libapp_cli_crash_overflow("-");
		break;
	case CM_HANG:
		libapp_cli_crash_hang();
		break;
	case CM_HANG_INTR:
		libapp_cli_crash_hang_intr();
		break;
	default:
		break;
	}
	log_put(LOG_ERR "CLI crash survived");
	return 0;
}

/*
 * Read configuration for CLI disable.
 */
static void libapp_cli_cli_conf(void)
{
	struct ada_conf_item setup_mode = {
		.name = SETUP_MODE_CONF,
		.type = ATLV_UINT,
		.val = &conf_setup_mode,
		.len = sizeof(conf_setup_mode),
	};
	char en[2];

	/*
	 * Read setup_mode early to allow early console disabling.
	 */
	ada_conf_get_item(&setup_mode);

	if (libapp_conf_get_string(SETUP_CLI_EN_CONF, en, sizeof(en)) < 0) {
		en[0] = SETUP_CLI_EN;
	}
	libapp_cli_enable = en[0] == SETUP_CLI_EN;
	libapp_cons_enable = en[0] != SETUP_CLI_DIS_UART;
}

/*
 * CLI disable command.
 *
 * Disabling is only effective in user mode.  Setup mode always has both
 * the console and CLI enabled.
 *
 * The config item can be empty or "1" to enable the CLI.
 * "0" means it's disabled.
 * "c" means both CLI and console UART I/O are disabled.
 * If set non-empty, change is not allowed in user mode.
 *
 * Once the CLI or console are disabled and setup_mode is disabled,
 * the only way to re-enable the CLI is to re-enable setup mode via
 * the cloud or with an engineering version of the app.
 */
static const char libapp_cli_cli_help[] =
	"cli [console-disable | disable | enable | enable-lock | show]";

static int libapp_cli_cli(int argc, char **argv)
{
	char state = SETUP_CLI_UNLOCK;
	char new_state;
	char *lock_msg = "un";
	char en[2];

	if (libapp_conf_get_string(SETUP_CLI_EN_CONF, en, sizeof(en)) > 0) {
		state = en[0];
		lock_msg = "";
	}

	if (argc == 1 || !strcmp(argv[1], "show")) {
		printcli("console is %sabled, %slocked",
		    state == SETUP_CLI_DIS_UART ? "dis" : "en", lock_msg);
		printcli("CLI is %sabled, %slocked",
		    (state != SETUP_CLI_EN &&
		    state != SETUP_CLI_UNLOCK) ? "dis" : "en", lock_msg);
		return 0;
	}
	if (argc == 2) {
		if (!strcmp(argv[1], "disable")) {
			new_state = SETUP_CLI_DIS;
		} else if (!strcmp(argv[1], "console-disable")) {
			new_state = SETUP_CLI_DIS_UART;
		} else if (!strcmp(argv[1], "enable")) {
			new_state = SETUP_CLI_UNLOCK;
		} else if (!strcmp(argv[1], "enable-lock")) {
			new_state = SETUP_CLI_EN;
		} else {
			goto usage;
		}
		if (state == new_state) {
			return 0;
		}
		if ((state != SETUP_CLI_UNLOCK && new_state != SETUP_CLI_EN) &&
		    !mfg_or_setup_mode_ok()) {
			return 0;
		}
		en[0] = new_state;
		en[1] = '\0';
		if (libapp_conf_set(SETUP_CLI_EN_CONF, en, 1)) {
			printcli("cli: config set failed");
			return 0;
		}
		printcli("changes effective after next reset");
		return 0;
	}
usage:
	printcli("usage: %s", libapp_cli_cli_help);
	return 0;
}

#define LIBAPP_CMD_INIT(_name, _help, _func) \
	{ .command = (_name), .help = (_help), .func = (_func) }

static const esp_console_cmd_t libapp_cmds[] = {
#ifdef AYLA_BLUETOOTH_SUPPORT
	LIBAPP_CMD_INIT("bt", al_bt_cli_help, al_bt_cli),
#endif
	LIBAPP_CMD_INIT("cli", libapp_cli_cli_help, libapp_cli_cli),
	LIBAPP_CMD_INIT("client", libapp_cli_client_help, libapp_cli_client),
	LIBAPP_CMD_INIT("conf", libapp_conf_cli_help, libapp_conf_cli),
#ifdef AYLA_LOG_CORE
	LIBAPP_CMD_INIT("core", ada_log_core_help, libapp_cli_log_core),
#endif
	LIBAPP_CMD_INIT("crash", "crash <type> - crash test",
	    libapp_cli_crash),
	LIBAPP_CMD_INIT("diag", libapp_diag_cli_help, libapp_diag_cli),
	LIBAPP_CMD_INIT("id", ada_conf_id_help, libapp_cli_id),
	LIBAPP_CMD_INIT("log", ada_log_cli_help, libapp_cli_log),
#ifdef AYLA_LOG_SNAPSHOTS
	LIBAPP_CMD_INIT("log-snap", ada_log_snap_cli_help, libapp_cli_log_snap),
#endif
#ifdef AYLA_METRICS_SUPPORT
	LIBAPP_CMD_INIT("metrics", metrics_cli_help, libapp_metrics_cli),
#endif
	LIBAPP_CMD_INIT("oem", "oem key <secret> [oem-model]", libapp_cli_oem),
	LIBAPP_CMD_INIT("reset", "reset [factory] - resets device",
	     libapp_cli_reset),
	LIBAPP_CMD_INIT("save", "save - save configuration", libapp_cli_save),
	LIBAPP_CMD_INIT("sched", libapp_sched_cli_help, libapp_sched_cli),
	LIBAPP_CMD_INIT("setup_mode",
	    "setup_mode enable|disable|show [<key>] - configure setup mode",
	    libapp_cli_setup_mode),
	LIBAPP_CMD_INIT("time", libapp_cli_time_help, libapp_cli_time),
	LIBAPP_CMD_INIT("version",
	    "version - show version", libapp_cli_version),
	LIBAPP_CMD_INIT("wifi", "wifi [help|show] [<name> <value>] ...",
	    libapp_wifi_cmd),
};

/*
 * Register all standard commmands.
 */
void libapp_cli_init(void)
{
	const esp_console_cmd_t *cmd;

	esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0,
	    "libapp_cli", &libapp_cli_pm_lock);
	libapp_cli_cli_conf();
	for (cmd = libapp_cmds;
	    cmd < libapp_cmds + ARRAY_LEN(libapp_cmds); cmd++) {
		esp_console_cmd_register(cmd);
	}
	esp_console_register_help_command();
}
