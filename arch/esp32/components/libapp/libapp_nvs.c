/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_partition.h>
#include <esp_flash_encrypt.h>
#include <ayla/log.h>
#include <libapp/libapp.h>
#include "libapp_conf_int.h"
#include "libapp_nvs_int.h"

/*
 * Handles used by conf.c for NVS access.
 * These are left open for read-write.
 */
nvs_handle_t libapp_nvs;
nvs_handle_t libapp_nvs_state;	/* handle for unencrypted NVS state saving */

/*
 * Initialize NVS flash partition.
 */
static int libapp_nvs_partition_init(const char *name, const char *key_name)
{
	esp_err_t err;
#ifdef CONFIG_NVS_ENCRYPTION
	const esp_partition_t *key_part;
	nvs_sec_cfg_t cfg;

	/*
	 * Find Key partition.
	 */
	key_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
	    ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, key_name);
	if (!key_part) {
		log_put(LOG_WARN "%s: no partition \"%s\" found", __func__,
		    key_name);
		return -1;
	}

	/*
	 * Load keys. If none found, generate them.
	 */
	err = nvs_flash_read_security_cfg(key_part, &cfg);
	if (err) {
		log_put(LOG_WARN "%s: NVS part %s sec cfg err %d", __func__,
		    name, err);
		log_put(LOG_WARN "%s: NVS generating keys", __func__);
		err = nvs_flash_generate_keys(key_part, &cfg);
		if (err) {
			log_put(LOG_ERR "%s: gen keys %d", __func__, err);
			return -1;
		}
	}

	err = nvs_flash_secure_init_partition(name, &cfg);
	if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
		log_put(LOG_WARN "%s: NVS sec init: part \"%s\": "
		    "no free pages - erasing",
		    __func__, name);
		err = nvs_flash_erase_partition(name);
		if (err) {
			log_put(LOG_ERR "%s: NVS erase \"%s\" failed %d",
			    __func__, name, err);
			return -1;
		}
		err = nvs_flash_secure_init_partition(name, &cfg);
	}
	if (err) {
		log_put(LOG_ERR "%s: NVS sec init: part \"%s\": "
		    "failed %d", __func__, name, err);
		return -1;
	}
#else
	log_put(LOG_WARN "%s: NVS is not encrypted", __func__);
	err = nvs_flash_init_partition(name);
	if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
		log_put(LOG_WARN "%s: NVS init: \"%s\" no free pages - erasing",
		    __func__, name);
		err = nvs_flash_erase_partition(name);
		if (err) {
			log_put(LOG_ERR "%s: NVS erase \"%s\" failed %d",
			    __func__, name, err);
			return -1;
		}
		err = nvs_flash_init_partition(name);
	}
	if (err) {
		log_put(LOG_ERR "%s: nvs flash init \"%s\" failed %d",
		    __func__, name, err);
		return -1;
	}
#endif
	return 0;
}

static nvs_handle_t libapp_nvs_open(const char *name)
{
	esp_err_t err;
	nvs_handle_t handle = 0;

	err = nvs_open_from_partition(name, AYLA_STORAGE,
	    NVS_READWRITE, &handle);
	if (err) {
		log_put(LOG_ERR "%s: nvs_open failed %d", __func__, err);
		handle = 0;
	}
	return handle;
}

nvs_handle_t libapp_nvs_id_init(void)
{
	nvs_handle_t libapp_nvs_id = 0;

	if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
	    ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs_id") &&
	    !libapp_nvs_partition_init("nvs_id", "nvs_id_keys")) {
		libapp_nvs_id = libapp_nvs_open("nvs_id");
	}
	return libapp_nvs_id;
}

void libapp_nvs_init(void)
{
	if (!libapp_nvs_partition_init("nvs", "nvs_keys")) {
		libapp_nvs = libapp_nvs_open("nvs");
	}
}

/*
 * Open partition for state saving.
 */
void libapp_nvs_state_init(void)
{
	if (!libapp_nvs_partition_init("nvs_state", "nvs_keys")) {
		libapp_nvs_state = libapp_nvs_open("nvs_state");
	}
}
