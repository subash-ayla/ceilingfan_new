/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <esp_err.h>
#include <esp_console.h>
#include <libpha/secure.h>
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>

static void pha_secure_cli_lock(void)
{
	int rc;
	char buf[200];

	buf[0] = '\0';
	rc = pha_secure_lock(buf, sizeof(buf));
	if (rc < 0) {
		printcli("error:  %s", buf);
	} else if (rc > 0 || buf[0]) {
		printcli("%s", buf);
	}
}

static const char pha_secure_cli_help[] = "secure [show|lock] - "
    "device security commands";

static esp_err_t pha_secure_cli(int argc, char **argv)
{
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "show"))) {
		pha_secure_efuses_show();
		return ESP_OK;
	}
	if (argc == 2 && !strcmp(argv[1], "lock")) {
		pha_secure_cli_lock();
		return ESP_OK;
	}
	printcli("usage: %s", pha_secure_cli_help);
	return ESP_OK;
}

static const esp_console_cmd_t pha_secure_cli_cmd = {
	.command = "secure",
	.help = pha_secure_cli_help,
	.func = pha_secure_cli,
};

void pha_secure_cli_register(void)
{
	esp_console_cmd_register(&pha_secure_cli_cmd);
}
