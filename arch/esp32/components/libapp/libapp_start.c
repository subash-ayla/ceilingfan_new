/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */

/*
 * Ayla device agent demo.
 */

#include <stddef.h>
#include <string.h>
#include <sys/time.h>

#include "soc/rtc.h"
#include "time.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_console.h"
#include "esp_newlib.h"

#include <ada/libada.h>
#include <ada/client.h>
#include <ayla/nameval.h>
#include <ayla/log.h>
#ifdef AYLA_WIFI_SUPPORT
#include <adw/wifi.h>
#endif
#include <libapp/libapp.h>
#include "libapp_conf_int.h"
#include "libapp_conf_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vfs.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "soc/uart_channel.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_interface.h"

#include <lwip/ip4_addr.h>

#define LIBAPP_DEV_NULL		"/app/null"	/* name for null device */
#if CONFIG_IDF_TARGET_ESP32C3
#define LIBAPP_CLI_STACK_SIZE	(5 * 1024)
#else
#define LIBAPP_CLI_STACK_SIZE	(5 * 1024)
#endif
#define LIBAPP_CLI_TASK_PRIO	1
#define LIBAPP_CLI_CMD_LEN	1000
#define LIBAPP_CLI_CMD_ARGS	16

#if CONFIG_IDF_TARGET_ESP32C3
#define LIBAPP_CONS_TX_IO	21
#define LIBAPP_CONS_RX_IO	20
#else
#if CONFIG_ESP_CONSOLE_UART_NUM == UART_NUM_0
#define LIBAPP_CONS_TX_IO	UART_NUM_0_TXD_DIRECT_GPIO_NUM
#define LIBAPP_CONS_RX_IO	UART_NUM_0_RXD_DIRECT_GPIO_NUM
#elif CONFIG_ESP_CONSOLE_UART_NUM == UART_NUM_1
#define LIBAPP_CONS_TX_IO	UART_NUM_1_TXD_DIRECT_GPIO_NUM
#define LIBAPP_CONS_RX_IO	UART_NUM_1_RXD_DIRECT_GPIO_NUM
#elif CONFIG_ESP_CONSOLE_UART_NUM == UART_NUM_2
#define LIBAPP_CONS_TX_IO	UART_NUM_2_TXD_DIRECT_GPIO_NUM
#define LIBAPP_CONS_RX_IO	UART_NUM_2_RXD_DIRECT_GPIO_NUM
#else
#error console as configured is not supported
#endif
#endif

#define FIX_EPOCH				   1535032179

#define SETUP_PROMPT "setup-> "
#define USER_PROMPT "--> "

#define DEL	0x7f
#define CTL_C	('C' & 0x1f)	/* control-C */
#define CTL_U	('U' & 0x1f)	/* control-U (rub-out) */

static void (*libapp_app_first_connect)(void);
static void (*libapp_identify_cb)(void);

void libapp_identify(void)
{
	log_put(LOG_DEBUG "%s called\n", __func__);

	if (libapp_identify_cb) {
		libapp_identify_cb();
	}
}

void libapp_identify_callback_set(void (*callback)(void))
{
	libapp_identify_cb = callback;
}

static void libapp_client_start(void)
{
	int rc;

	/*
	 * Read configuration.
	 */
	libapp_conf_init();

	/*
	 * Init libada.
	 */
	rc = ada_init();
	if (rc) {
		log_put(LOG_ERR "ADA init failed");
		return;
	}

#ifdef AYLA_LOCAL_CONTROL_SUPPORT
	/*
	 * Enable local control access.
	 */
	rc = ada_client_lc_up();
	if (rc) {
		log_put(LOG_ERR "ADA local control up failed");
		return;
	}
#endif

	libapp_ota_init();
	libapp_wifi_init();
}

void libapp_cloud_up(void)
{
	static u8 is_cloud_started;

	if (is_cloud_started) {
		return;
	}
	is_cloud_started = 1;
	if (libapp_app_first_connect) {
		libapp_app_first_connect();
	}

}

static char *libapp_get_line(void)
{
	char *line;
	size_t len;
	char *bp;
	int overrun = 0;
	int c;

	/*
	 * Allocate command buffer.  On failure, try smaller buffer.
	 */
	len = LIBAPP_CLI_CMD_LEN;
	line = malloc(len);
	if (!line) {
		printcli("cmd buf alloc failed");
		return NULL;
	}
	bp = line;
	*bp = '\0';

	for (;;) {
		c = fgetc(stdin);
		if (c < 0 || c > 0x7f) {
			break;
		}
		switch (c) {
		case DEL:
		case '\b':
			if (bp == line) {
				break;
			}
			bp--;
			*bp = '\0';
			fputs("\b \b", stdout);	/* space over previous char */
			break;
		case '\r':
		case '\n':
			fputs("\r\n", stdout);
			if (overrun) {
				printcli("command line too long");
				goto out;
			}
			return line;
		case CTL_U:
		case CTL_C:
			fputs("\r", stdout);
			goto out;
		default:
			/* Always keep room for terminating NUL */
			/* drop characters past end of buffer */
			if (bp >= line + len - 2) {
				overrun = 1;
				break;
			}
			*bp++ = (char)c;
			*bp = '\0';
			fputc(c, stdout);
			break;
		}
	}
out:
	free(line);
	return NULL;
}

static void libapp_console_task(void *pvParameters)
{
	char *line;
	int ret;
	esp_err_t err;

	/*
	 * Print line expected by OEM script to indicate CLI is ready.
	 */
	printcli("CLI ready.");		/* expected */

	/* Main loop */
	while (1) {
		fputs(conf_setup_mode ? SETUP_PROMPT : USER_PROMPT, stdout);

		line = libapp_get_line();
		if (line == NULL) { /* Ignore empty lines */
			continue;
		}
		if (!conf_setup_mode && !libapp_cli_enable) {
			printf("CLI disabled\n");
			free(line);
			continue;
		}

		/* Try to run the command */
		err = esp_console_run(line, &ret);

		if (err == ESP_ERR_NOT_FOUND) {
			printf("Unrecognized command\n");
		} else if (err == ESP_ERR_INVALID_ARG) {
			/* command was empty */
		} else if (err == ESP_OK && ret != ESP_OK) {
			printf("Command returned non-zero error code: "
			    "0x%x (%s)\n", ret, esp_err_to_name(err));
		} else if (err != ESP_OK) {
			printf("Internal error: %s\n", esp_err_to_name(err));
		} else {
			/* command was successful */
		}
		free(line);
	}
}

/*
 * Interpose on ESP-IDF log output vprintf function.
 *
 * This is so these messages can be sent to the service with other logs,
 * or saved to flash in the unlikely event of a crash.
 *
 * fmt is currently always like "S (<time>) tag: <message>", where
 * S is severity, and tag is the module logging the message.
 * If it is like that, convert the severity to our severity level and skip
 * the space.  Leave the timestamp.
 *
 * We should have a more general system of module names.
 * For now, use the default "mod" module.
 */
static int libapp_console_log(const char *fmt, va_list args)
{
	enum log_sev sev;

	switch (*fmt) {
	case 'E':
		sev = LOG_SEV_ERR;
		break;
	case 'W':
		sev = LOG_SEV_WARN;
		break;
	case 'I':
		sev = LOG_SEV_INFO;
		break;
	case 'D':
		sev = LOG_SEV_DEBUG;
		break;
	case 'V':
		sev = LOG_SEV_DEBUG2;
		break;
	default:
		sev = LOG_SEV_NONE;
		break;
	}
	if (*fmt && sev != LOG_SEV_NONE) {
		fmt++;
		if (*fmt == ' ') {
			fmt++;
		}
	} else {
		sev = LOG_SEV_ERR;
	}
	return log_put_va_sev(LOG_MOD_DEFAULT, sev, fmt, args);
}

/*
 * Low-level output function.
 *
 * Using newlib's printf is not a good option since it tries to allocate
 * and initialize a lock, which may fail in low-memory situations.
 *
 * The caller supplies '\n', which should be converted to '\r\n' (CR/LF).
 */
static void libapp_console_output(const char *str)
{
	size_t len;
	char *cp;
	int uart = CONFIG_ESP_CONSOLE_UART_NUM;

	while (*str) {
		cp = strchr(str, '\n');
		if (!cp) {
			len = strlen(str);
		} else {
			len = cp - str;
		}
		uart_write_bytes(uart, str, len);
		if (!cp) {
			break;
		}
		uart_write_bytes(uart, "\r\n", 2);
		str = cp + 1;
	}
}

/*
 * Interpose on ADA's log output.
 */
static void libapp_console_print(const char *str)
{
	if (conf_setup_mode || libapp_cons_enable) {
		libapp_console_output(str);
	}
}

static void libapp_console_drop(const char *str)
{
}

static int libapp_null_open(const char *path, int flags, int mode)
{
	return 0;
}

static ssize_t libapp_null_read(int fd, void *dest, size_t len)
{
	return 0;
}

static ssize_t libapp_null_write(int fd, const void *data, size_t len)
{
	return 0;
}

/*
 * For cases where the console is disabled, connect stdin, stdout,
 * and stderr to a null file, and disable the associated I/O pins.
 */
static void libapp_console_deinit(void)
{
	esp_vfs_t vfs = {
		.flags = ESP_VFS_FLAG_DEFAULT,
		.open = libapp_null_open,
		.write = libapp_null_write,
		.read = libapp_null_read,
	};

	if (esp_vfs_register(LIBAPP_DEV_NULL, &vfs, NULL)) {
		printf("failed to register " LIBAPP_DEV_NULL "\n");
		return;
	}
	fclose(stdin);
	stdin = fopen(LIBAPP_DEV_NULL, "r");
	if (!stdin) {
		printf("failed to open " LIBAPP_DEV_NULL "\n");
		return;
	}
	fclose(stdout);
	stdout = fopen(LIBAPP_DEV_NULL, "w");
	fclose(stderr);
	stderr = fopen(LIBAPP_DEV_NULL, "w");

	/*
	 * Set default stdio for new tasks.
	 */
	_GLOBAL_REENT->_stdin = stdin;
	_GLOBAL_REENT->_stdout = stdout;
	_GLOBAL_REENT->_stderr = stderr;

	gpio_reset_pin(LIBAPP_CONS_TX_IO);
	gpio_reset_pin(LIBAPP_CONS_RX_IO);
}

static void libapp_console_init(void)
{
	ada_cons_log_redir = libapp_console_print;
	esp_log_set_vprintf(libapp_console_log);

	if (!conf_setup_mode && !libapp_cons_enable) {
		libapp_console_deinit();
		ada_cons_log_redir = libapp_console_drop;
		return;
	}

	/*
	 * In case the console UART was previously disabled,
	 * and a power cycle hasn't restored the pin matrix, do that here.
	 */
	uart_set_pin(CONFIG_ESP_CONSOLE_UART_NUM, LIBAPP_CONS_TX_IO,
	    LIBAPP_CONS_RX_IO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

	/* Disable buffering on stdin and stdout */
	setvbuf(stdin, NULL, _IONBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Install UART driver for interrupt-driven reads and writes */
	uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
	    256, 0, 0, NULL, 0);

	/* Tell VFS to use UART driver */
	esp_vfs_dev_uart_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
}

static void libapp_start_console(void)
{
	if (!conf_setup_mode && !libapp_cli_enable) {
		return;
	}
	xTaskCreate(libapp_console_task, "Console Task",
	    LIBAPP_CLI_STACK_SIZE / sizeof(portSTACK_TYPE), NULL,
	    LIBAPP_CLI_TASK_PRIO, NULL);
}

static void libapp_modules_init(void)
{
	esp_console_config_t console_config = {
		.max_cmdline_args = LIBAPP_CLI_CMD_ARGS,
		.max_cmdline_length = LIBAPP_CLI_CMD_LEN,
	};

	/*
	 * Note: esp_console_init() is needed for commands from cloud even if
	 * serial console CLI is disabled.
	 */
	esp_console_init(&console_config);
	libapp_cli_init();
	libapp_console_init();
}

static void libapp_cmd_exec(const char *command)
{
	int ret;

	esp_console_run(command, &ret);
}

static void libapp_shutdown(void)
{
	/*
	 * Sync RTC to the high-resolution timer to eliminate any drift
	 * that has accumulated between the two, which would show up after
	 * reset when the clock gets restored from RTC.
	 */
	esp_sync_counters_rtc_and_frc();
}

/*
 * libapp initialization.
 *
 * This may be optionally called before libapp_start().
 * It does the minimum required for use of libapp_conf APIs.
 */
void libapp_init(void)
{
	static u8 done;

	if (done) {
		return;
	}
	done = 1;
	log_init();
	log_mask_init_min(BIT(LOG_SEV_INFO), LOG_DEFAULT);
	libapp_nvs_init();
	log_conf_load();

	libapp_modules_init();

	printf("\r\n\n%s\r\n", mod_sw_version);

	esp_register_shutdown_handler(libapp_shutdown);
	ada_client_command_func_register(libapp_cmd_exec);
	adw_wifi_init();

	libapp_start_console();
}

/*
 * Start library common features for app.
 * Called from app_main, typically.
 * Once Wi-Fi is started and agent is initialized, app_init() will be called.
 * Once the cloud is up, app_first_connect() will be called.
 */
void libapp_start(void (*app_init)(void), void (*app_first_connect)(void))
{
	libapp_app_first_connect = app_first_connect;

	libapp_init();

	libapp_client_start();
	libapp_net_event_init();

	if (app_init) {
		app_init();
	}

#ifdef AYLA_BLUETOOTH_SUPPORT
	libapp_bt_init();
#endif
}
