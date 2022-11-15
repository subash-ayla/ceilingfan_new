/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ayla/utypes.h>
#include <ayla/clock.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/tlv.h>
#include <ayla/conf.h>

#include <ada/libada.h>
#include <ada/client_ota.h>
#include <net/base64.h>
#include <net/net.h>
#include <esp_err.h>
#include <esp_pm.h>
#include "libapp_conf_int.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_spi_flash.h"
#include "freertos/timers.h"

#include <libapp/libapp_ota.h>

#define CRC32_INIT 0xffffffffUL

/*
 * Define LIBAPP_OTA_MODULE in a makefile if module OTA is required by a
 * particular product. The default is host OTA.
 */
#ifndef LIBAPP_OTA_MODULE
#define LIBAPP_OTA_TYPE	OTA_HOST	/* use host OTAs by default */
#else
#define LIBAPP_OTA_TYPE	OTA_MODULE
#endif

static const u32 crc32_table[16] = {
	0, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
	0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
	0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
	0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

static esp_ota_handle_t update_handle;
static const esp_partition_t *update_partition;
static u8 ota_in_progress;
static esp_pm_lock_handle_t libapp_ota_pm_lock;
static const struct libapp_app_ota_ops *app_ota_ops;

/*
 * Compute CRC-8 with IEEE polynomial
 * LSB-first.  Use 4-bit table.
 */
static u32 ota_crc32(const void *buf, size_t len, u32 crc)
{
	const u8 *bp = buf;

	while (len-- > 0) {
		crc ^= *bp++;
		crc = (crc >> 4) ^ crc32_table[crc & 0xf];
		crc = (crc >> 4) ^ crc32_table[crc & 0xf];
	}
	return crc;
}

struct libapp_ota {
	u32 exp_len;
	u32 rx_len;
	u32 crc;
	u32 info_len;		/* for logs - how often to log */
	u32 info_offset;	/* for logs - last offset logged */
};
static struct libapp_ota libapp_ota;

/*
 * Do clean up resources after a OTA has begun.
 */
static esp_err_t ota_end(enum patch_state patch_err)
{
	esp_err_t err;

	ota_in_progress = 0;
	err = esp_ota_end(update_handle);
	if (err) {
		if (!patch_err) {
			log_put(LOG_ERR "esp_ota_end returned 0x%x", err);
			patch_err = PB_ERR_FATAL;
		}
	}

	if (patch_err && update_partition) {
		/*
		 * Erase the partition on error so there's no chance of
		 * executing rogue or corrupted code that didn't pass
		 * validation.
		 */
		esp_partition_erase_range(update_partition, 0,
		    update_partition->size);
		update_handle = 0;
	}

	if (app_ota_ops && app_ota_ops->ota_end) {
		app_ota_ops->ota_end(patch_err);
	}
	return err;
}

static enum patch_state libapp_ota_notify(const struct ada_ota_info *ota_info)
{
	esp_err_t err;
	enum patch_state patch_err;

	log_put(LOG_INFO
	    "OTA notification: label=\"%s\" length=%lu version=\"%s\"",
	    ota_info->label ? ota_info->label : "(none)", ota_info->length,
	    ota_info->version);
	libapp_ota.exp_len = ota_info->length;
	libapp_ota.info_len = ota_info->length / 10;
	libapp_ota.info_offset = 0;
	libapp_ota.rx_len = 0;
	libapp_ota.crc = CRC32_INIT;

	update_partition = esp_ota_get_next_update_partition(NULL);
	if (!update_partition) {
		return PB_ERR_OPEN;
	}
	log_put(LOG_INFO "OTA writing partition at 0x%x",
	    update_partition->address);

	update_handle = 0;	/* be sure it's not got a value from failure */
	err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
	if (err != ESP_OK) {
		log_put(LOG_ERR "esp_ota_begin failed (%s)",
		    esp_err_to_name(err));
		return PB_ERR_OPEN;
	}

	if (app_ota_ops && app_ota_ops->ota_start) {
		patch_err = app_ota_ops->ota_start(ota_info->length,
		    ota_info->version);
		if (patch_err) {
			log_put(LOG_ERR "app ota_start failed %d", patch_err);
			ota_end(patch_err);
			return patch_err;
		}
	}

	ota_in_progress = 1;
	ada_ota_fetch_len_set(CLIENT_OTA_FETCH_LEN_MED);
	ada_ota_start();
	return PB_DONE;
}

/*
 * Save the OTA image chunk by chunk as it is received.
 */
static enum patch_state libapp_ota_save(unsigned int offset,
		const void *buf, size_t len)
{
	struct libapp_ota *ota = &libapp_ota;
	esp_err_t err;
	enum patch_state patch_err;

	if (offset != ota->rx_len) {
		log_put(LOG_WARN "OTA save: offset skip at %u", offset);
		goto fatal_err;
	}
	ota->rx_len += len;
	if (ota->rx_len > ota->exp_len) {
		log_put(LOG_WARN "OTA save: rx at %lu past len %lu",
				ota->rx_len, ota->exp_len);
		goto fatal_err;
	}
	esp_pm_lock_acquire(libapp_ota_pm_lock);
	ota->crc = ota_crc32(buf, len, ota->crc);

	if (app_ota_ops && app_ota_ops->ota_rx_chunk) {
		patch_err = app_ota_ops->ota_rx_chunk(offset, buf, len);
		if (patch_err) {
			log_put(LOG_ERR "app ota_start failed %d", patch_err);
			goto error_exit;
		}
	}

	err = esp_ota_write(update_handle, buf, len);
	esp_pm_lock_release(libapp_ota_pm_lock);
	if (err != ESP_OK) {
		log_put(LOG_ERR "esp_ota_write failed (%s)",
		    esp_err_to_name(err));
		patch_err = PB_ERR_WRITE;
		goto error_exit;
	}

	offset += len;
	if ((offset - ota->info_offset) >= ota->info_len && ota->exp_len) {
		log_put(LOG_INFO "OTA %lu%% saved",
		     (offset * 100) / ota->exp_len);
		ota->info_offset += ota->info_len;
	}
	return PB_DONE;

fatal_err:
	patch_err = PB_ERR_FATAL;
error_exit:
	ota_end(patch_err);
	return patch_err;

}

static void ota_tmr_task(void *arg)
{
	esp_restart();
}

/*
 * Check image header.
 * If booter header SPI speed is 80 MHz, image must also be.
 * It does work to have booter in QOUT mode and image in DIO mode.
 * This may be a (temporary) bootloader limitation.
 */
static enum patch_state libapp_ota_header_check(const esp_partition_t *part)
{
	esp_image_header_t boot_head;
	esp_image_header_t img_head;
	spi_flash_mmap_handle_t mmap;
	const esp_image_header_t *head;
	const void *ptr;
	esp_err_t rc;

	/*
	 * Read bootflash header info.
	 * Use mmap API to get decrypted version.
	 */
	rc = spi_flash_mmap(0, SPI_FLASH_MMU_PAGE_SIZE, SPI_FLASH_MMAP_DATA,
	    &ptr, &mmap);
	if (rc) {
		log_put(LOG_ERR "OTA boot map err %d", rc);
		return PB_ERR_FATAL;
	}
	memcpy(&boot_head, (u8 *)ptr + ESP_BOOTLOADER_OFFSET,
	    sizeof(boot_head));
	spi_flash_munmap(mmap);

	rc = esp_partition_read(part, 0, &img_head, sizeof(img_head));
	if (rc) {
		log_put(LOG_ERR "OTA image head read err %d", rc);
		return PB_ERR_FATAL;
	}

	head = &boot_head;
	if (head->magic != ESP_IMAGE_HEADER_MAGIC) {
		log_put(LOG_ERR "boot image header bad magic %x", head->magic);
		return PB_ERR_FATAL;
	}
	head = &img_head;
	if (head->magic != ESP_IMAGE_HEADER_MAGIC) {
		log_put(LOG_ERR "OTA image header bad magic %x", head->magic);
		return PB_ERR_PHEAD;
	}

	if (boot_head.spi_speed == ESP_IMAGE_SPI_SPEED_80M &&
	    img_head.spi_speed != ESP_IMAGE_SPI_SPEED_80M)  {
		log_put(LOG_ERR "OTA image SPI speed %x incompatible",
		    img_head.spi_speed);
		return PB_ERR_PHEAD;
	}
	return 0;
}

static void libapp_ota_save_done(void)
{
	struct libapp_ota *ota = &libapp_ota;
	enum patch_state patch_err;

	if (ota->rx_len != ota->exp_len) {
		log_put(LOG_WARN "OTA save_done: rx len %lu not "
		    "expected len %lu", ota->rx_len, ota->exp_len);
		goto fatal_err;
	}
	log_put(LOG_INFO "OTA save_done len %lu crc %lx\r\n",
			ota->rx_len, ota->crc);

	patch_err = libapp_ota_header_check(update_partition);
	if (patch_err) {
		goto error_exit;
	}

	if (app_ota_ops && app_ota_ops->ota_rx_done) {
		patch_err = app_ota_ops->ota_rx_done();
		if (patch_err) {
			log_put(LOG_ERR "app ota_rx_done failed %d", patch_err);
			goto error_exit;
		}
	}
	if (app_ota_ops && app_ota_ops->ota_ready) {
		app_ota_ops->ota_ready();
		return;
	}
	libapp_ota_apply();
	return;

fatal_err:
	patch_err = PB_ERR_FATAL;
error_exit:
	ota_end(patch_err);
	ada_ota_report(patch_err);
}

void libapp_ota_apply(void)
{
	TimerHandle_t tmr;
	esp_err_t err;

	if (!ota_in_progress) {
		return;
	}
	if (ota_end(PB_DONE)) {
		log_put(LOG_ERR "esp_ota_end failed");
		goto fatal_after_ota_end;
	}
	err = esp_ota_set_boot_partition(update_partition);
	if (err != ESP_OK) {
		log_put(LOG_ERR "esp_ota_set_boot_partition failed (%s)",
		    esp_err_to_name(err));
		goto fatal_after_ota_end;
	}
	update_partition = 0;
	ada_ota_report(PB_DONE);

	tmr = xTimerCreate("otaTmr", (20000 / portTICK_RATE_MS),
	    0, NULL, ota_tmr_task);
	xTimerStart(tmr, portMAX_DELAY);
	return;

fatal_after_ota_end:
	ada_ota_report(PB_ERR_FATAL);
}

void libapp_ota_register(const struct libapp_app_ota_ops *ops)
{
	app_ota_ops = ops;
}

/*
 * Return non-zero if an OTA is in progress.
 */
int libapp_ota_is_in_progress(void)
{
	return ota_in_progress;
}

/*
 * This marks the app image as bootable and cancels any rollback.
 */
static void libapp_ota_commit(void)
{
	esp_err_t err;

	err = esp_ota_mark_app_valid_cancel_rollback();
	if (err) {
		log_put(LOG_ERR "cancel_rollback err %x", err);
		return;
	}
}

static struct ada_ota_ops libapp_ota_ops = {
	.notify = libapp_ota_notify,
	.save = libapp_ota_save,
	.save_done = libapp_ota_save_done,
};

void libapp_ota_init(void)
{
	ada_ota_register(LIBAPP_OTA_TYPE, &libapp_ota_ops);
	esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0,
	    "libapp_ota", &libapp_ota_pm_lock);

	/*
	 * Commit to this image if we get this far.
	 */
	libapp_ota_commit();
}
