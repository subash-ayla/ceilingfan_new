/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#include <stdlib.h>
#include <esp_pm.h>
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <libapp/power_mgmt.h>

#if CONFIG_IDF_TARGET_ESP32C3
#define	LIBAPP_CPU_MHZ_MAX CONFIG_ESP32C3_DEFAULT_CPU_FREQ_MHZ

#if CONFIG_ESP32C3_DEFAULT_CPU_FREQ_MHZ == 160 || \
    CONFIG_ESP32C3_DEFAULT_CPU_FREQ_MHZ == 80
#define	LIBAPP_CPU_MHZ_MED 160
#define	LIBAPP_CPU_MHZ_MIN 80
#else
#error CPU frequency not supported
#endif

#elif CONFIG_IDF_TARGET_ESP32
#define	LIBAPP_CPU_MHZ_MAX CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ

#if CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ == 240 || \
    CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ == 160 || \
    CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ == 80
#define	LIBAPP_CPU_MHZ_MED 160
#define	LIBAPP_CPU_MHZ_MIN 80
#else
#error CPU frequency not supported
#endif

#else
#error Target not supported
#endif

static esp_pm_lock_handle_t libapp_pm_lock;

/*
 * Initialize power management.
 * This is called very early, before logs initialized, so error is not logged.
 * This will work unless power management is not configured.
 */
void libapp_power_mgmt_init(void)
{
#if CONFIG_IDF_TARGET_ESP32C3
	esp_pm_config_esp32c3_t cfg = {
#else
	esp_pm_config_esp32_t cfg = {
#endif
		.max_freq_mhz = LIBAPP_CPU_MHZ_MAX,
		.min_freq_mhz = LIBAPP_CPU_MHZ_MIN,
		.light_sleep_enable = 0,
	};

	esp_pm_configure(&cfg);
	esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0,
	    "libapp", &libapp_pm_lock);
}

/*
 * Get lock to stay at high-performance power level.
 */
void libapp_power_mgmt_acquire(void)
{
	esp_pm_lock_acquire(libapp_pm_lock);
}

/*
 * Release lock to allow lowering performance and power.
 */
void libapp_power_mgmt_release(void)
{
	esp_pm_lock_release(libapp_pm_lock);
}
