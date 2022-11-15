#ifndef DECLARATIONS_H
#define DECLARATIONS_H
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <freertos/timers.h>
#include "app_build.h"
#include "app_int.h"

extern u8 mcu_feature_mask;	/* features of MCU */
extern u8 mcu_feature_mask_min;	/* minimum features for transport */

#define APP_FAN_SPEED "fan_speed"
#define APP_FAN_PWR "fan_power"
#define APP_FAN_DIR "fan_direction"
#define APP_LIGHT_PWR "light_control"
#define APP_LIGHT_RATING "light_rating"
#define APP_LIGHT_COLOR "light_color_index"
#define APP_FW_VERSION "fw_version"
static enum ada_err app_fanspeed_control(struct ada_sprop *sprop,
	const void *buf, size_t len);
static enum ada_err app_fanpower_control(struct ada_sprop *sprop,
	const void *buf, size_t len);
static enum ada_err app_fandirection_control(struct ada_sprop *sprop,
	const void *buf, size_t len);
static enum ada_err app_lightpower_control(struct ada_sprop *sprop,
	const void *buf, size_t len);
static enum ada_err app_lightrating_control(struct ada_sprop *sprop,
	const void *buf, size_t len);
static enum ada_err app_lightcolor_control(struct ada_sprop *sprop,
	const void *buf, size_t len);
void send_all_prop();
#endif
