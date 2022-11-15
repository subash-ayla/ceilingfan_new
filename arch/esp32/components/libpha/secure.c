/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <libapp/libapp.h>
#include "ftm_int.h"
#include "secure_int.h"
#include "app_build.h"

#define PHA_SECURE_ESP32_REV	3	/* minimum chip revision */

#define PHA_SECURE_DIS	"secure/dis"

struct pha_secure_efuse {
	const esp_efuse_desc_t **efuse;
	const char *name;
};

#if ESP_IDF_VERSION <= ESP_IDF_VERSION_VAL(4, 3, 0)
#error ESP-IDF version not supported
#endif

/*
 * List of single-bit eFuses to set, in order.
 */
static struct pha_secure_efuse pha_secure_efuses[] = {
#if CONFIG_IDF_TARGET_ESP32
	{ ESP_EFUSE_DISABLE_DL_CACHE, "DISABLE_DL_CACHE" },
	{ ESP_EFUSE_DISABLE_DL_DECRYPT, "DISABLE_DL_DECRYPT" },
	{ ESP_EFUSE_DISABLE_DL_ENCRYPT, "DISABLE_DL_ENCRYPT" },
	{ ESP_EFUSE_UART_DOWNLOAD_DIS, "UART_DOWNLOAD_DIS" },
	{ ESP_EFUSE_DISABLE_JTAG, "DISABLE_JTAG" },
	{ ESP_EFUSE_CONSOLE_DEBUG_DISABLE, "CONSOLE_DEBUG_DISABLE" },

	/*
	 * Write-disabling FLASH_CRYPT_CNT controls multiple fields including
	 * UART_DOWNLOAD_DIS.
	 */
	{ ESP_EFUSE_WR_DIS_FLASH_CRYPT_CNT, "WR_DIS_FLASH_CRYPT_CNT" },
#elif CONFIG_IDF_TARGET_ESP32C3
	{ ESP_EFUSE_DIS_DOWNLOAD_MANUAL_ENCRYPT,
	    "DISABLE_DL_MANUAL_ENCRYPT" },
	{ ESP_EFUSE_DIS_DOWNLOAD_ICACHE, "DISABLE_DL_ICACHE" },
	{ ESP_EFUSE_DIS_DOWNLOAD_MODE, "DISABLE_DL_MODE" },
	{ ESP_EFUSE_DIS_PAD_JTAG, "DISABLE_PAD_JTAG" },
	{ ESP_EFUSE_DIS_USB_JTAG, "DISABLE_USB_JTAG" },
	{ ESP_EFUSE_SOFT_DIS_JTAG, "SOFT_DISABLE_JTAG" },
	{ ESP_EFUSE_WR_DIS_SPI_BOOT_CRYPT_CNT, "WR_DIS_FLASH_CRYPT_CNT" },
#else
#error target not supported
#endif
};

/*
 * List of eFuse fields to show only.
 */
static struct pha_secure_efuse pha_secure_efuses_read[] = {
	/*
	 * Show encryption count and status.
	 * Flash encryption is enabled if this has an odd number of bits set.
	 */
#if CONFIG_IDF_TARGET_ESP32
	{ ESP_EFUSE_FLASH_CRYPT_CNT, "FLASH_CRYPT_CNT" },
#elif CONFIG_IDF_TARGET_ESP32C3
	{ ESP_EFUSE_SPI_BOOT_CRYPT_CNT, "SPI_BOOT_CRYPT_CNT" },
#else
#error target not supported
#endif
	{ ESP_EFUSE_SECURE_VERSION, "SECURE_VERSION (anti-rollback)" },
};

void pha_secure_efuses_show(void)
{
	struct pha_secure_efuse *ef;
	esp_flash_enc_mode_t mode;
	const char *msg;
	uint32_t val = 0;
	const uint32_t val_bits = sizeof(val) * 8;
	uint8_t bit = 0;
	esp_err_t err;

	for (ef = pha_secure_efuses; ef < ARRAY_END(pha_secure_efuses); ef++) {
		err = esp_efuse_read_field_blob(ef->efuse, &bit, 1);
		if (err) {
			printcli("secure: %s error %#x", ef->name, err);
			continue;
		}
		printcli("secure: %s = %u", ef->name, bit);
	}

	for (ef = pha_secure_efuses_read;
	    ef < ARRAY_END(pha_secure_efuses_read); ef++) {
		err = esp_efuse_read_field_blob(ef->efuse, &val, val_bits);
		if (err) {
			printcli("secure: %s error %#x", ef->name, err);
			continue;
		}
		printcli("secure: %s = %#x", ef->name, val);
	}

	mode = esp_get_flash_encryption_mode();
	switch (mode) {
	case ESP_FLASH_ENC_MODE_DISABLED:
		msg = "not enabled";
		break;
	case ESP_FLASH_ENC_MODE_DEVELOPMENT:
		msg = "enabled for development";
		break;
	case ESP_FLASH_ENC_MODE_RELEASE:
		msg = "enabled for release";
		break;
	default:
		msg = "unknown";
		break;
	}
	printcli("secure: Flash encryption mode is %s.", msg);
}

static int pha_secure_efuses_are_all_set(void)
{
	struct pha_secure_efuse *ef;
	uint8_t bit = 0;
	int all_set = 1;

	for (ef = pha_secure_efuses; ef < ARRAY_END(pha_secure_efuses); ef++) {
		esp_efuse_read_field_blob(ef->efuse, &bit, 1);
		pha_ftm_log(LOG_DEBUG "secure: efuse %s reads %u",
		    ef->name, bit);
		if (!bit) {
			all_set = 0;
		}
	}
	return all_set;
}

#ifdef BUILD_TYPE_REL
/*
 * Set single efuse field bit if not already set.
 */
static esp_err_t pha_secure_efuse_set(const esp_efuse_desc_t *field[])
{
	uint8_t bit = 0;

	esp_efuse_read_field_blob(field, &bit, 1);
	if (bit) {
		return 0;
	}
	return esp_efuse_write_field_cnt(field, 1);
}

/*
 * Set all eFuses in list.
 * Return -1 if any report failure.
 */
static int pha_secure_efuses_set(void)
{
	struct pha_secure_efuse *ef;
	esp_err_t err;
	int errs = 0;

	for (ef = pha_secure_efuses; ef < ARRAY_END(pha_secure_efuses); ef++) {
		err = pha_secure_efuse_set(ef->efuse);
		if (err) {
			pha_ftm_log(LOG_ERR "secure: setting efuse %s err %d",
			    ef->name, err);
			errs = -1;
		}
	}
	return errs;
}
#endif /* BUILD_TYPE_REL */

/*
 * Set flash encryption in Release mode by setting eFuses that prevent
 * flash downloading and re-enabling.
 * Returns -1 on error or if already locked.
 * Returns 0 on success.
 */
int pha_secure_lock(char *err_buf, size_t err_len)
{
	esp_flash_enc_mode_t mode;
	esp_chip_info_t ci;
	long val;

	esp_chip_info(&ci);
#if CONFIG_IDF_TARGET_ESP32C3
	if (ci.model != CHIP_ESP32C3) {
		snprintf(err_buf, err_len, "secure: chip model %u not ESP32C3",
		    ci.model);
		return -1;
	}
#else
	if (ci.model != CHIP_ESP32) {
		snprintf(err_buf, err_len, "secure: chip model %u not ESP32",
		    ci.model);
		return -1;
	}

	/*
	 * For rev 1 chips, we could lock up flash encryption even though
	 * boot_security v2 is not available.
	 * For now, just don't support chips before rev 3.
	 */
	if (ci.revision < PHA_SECURE_ESP32_REV) {
		snprintf(err_buf, err_len,
		    "secure: chip rev %u. Rev %u required.",
		    ci.revision, PHA_SECURE_ESP32_REV);
		return -1;
	}
#endif /* TARGET */

	/*
	 * Check flash encryption mode.  If disabled, do nothing.
	 * If already in release mode, still try to set any unset eFuses.
	 */
	mode = esp_get_flash_encryption_mode();
	if (mode == ESP_FLASH_ENC_MODE_DISABLED) {
		snprintf(err_buf, err_len,
		    "secure: flash encryption not enabled");
		return -1;	/* booter must enable encryption first */
	}

	/*
	 * See if all needed eFuses are set.
	 */
	if (pha_secure_efuses_are_all_set()) {
		snprintf(err_buf, err_len, "secure: eFuses already set");
		return -1;	/* nothing to do */
	}

	/*
	 * Check for internal module configuration item.
	 */
	if (!libapp_conf_long_get(PHA_SECURE_DIS, &val) && val == 1) {
		snprintf(err_buf, err_len, "secure: eFuses not set: "
		    "app/" PHA_SECURE_DIS " is set");
		return 1;
	}

#ifdef BUILD_TYPE_REL
	if (pha_secure_efuses_set()) {
		snprintf(err_buf, err_len, "secure: efuses_set failed");
		return -1;
	}
	snprintf(err_buf, err_len, "secure: flash encryption release mode set");
	return 0;
#else
	snprintf(err_buf, err_len, "secure: "
	    "skipping eFuse sets for non-release build");
	return 1;
#endif
}

int pha_ftm_secure_lock(void)
{
	int rc;
	char buf[200];

	buf[0] = '\0';
	rc = pha_secure_lock(buf, sizeof(buf));
	if (rc < 0) {
		pha_ftm_log(LOG_ERR "%s", buf);
	} else if (rc > 0 || buf[0]) {
		pha_ftm_log(LOG_INFO "%s", buf);
	}
	return rc;
}
