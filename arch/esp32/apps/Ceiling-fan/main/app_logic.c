/*
 * Copyright 2022 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef AYLA_ESP32_SUPPORT
#define AYLA_ESP32_SUPPORT		/* for timer.h */
#endif

#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "freertos/semphr.h"
#include <ayla/assert.h>
#include <sys/types.h>
#include <sys/time.h>
#include "driver/periph_ctrl.h"
#include "driver/timer.h"
#include <ada/libada.h>
#include <ada/sprop.h>
#include <adw/wifi.h>
#include <libapp/libapp.h>
#include <libapp/debounce.h>
#include <ayla/log.h>
#include <ayla/timer.h>
#include "app_int.h"
#include "math.h"
#include "esp_types.h"
#include "app_logic.h"
#include <libapp/net_event.h>
#include <libpha/ftm.h>
#include "app_debug.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "ada_lock.h"
#include <libapp/libapp.h>
#include <adb/adb.h>
#include <adb/al_bt.h>
#include <declarations.h>

/*
 * Using for debug.
 */
#define DEBUG_TEST_MODE

#define FTM_CHECK_PERIOD	500
#define FTM_BT_PERIOD		15000
#define FTM_STEP_PERIOD		3000
#define FTM_STOP_PERIOD		500

/* 1s = 1000ms */
#define WIFI_LED_1HZ_PERIOD	1000

/* 200ms */
#define WIFI_LED_5HZ_PERIOD	200

/* 1s = 5 * 200ms */
#define WIFI_BLINK_5HZ_COUNT	5

/* 12 months, 30 days/month, 24 hours/day */
#define MAX_LIFE_HOURS		(12 * 30 * 24)

/* 11 months, 30 days/month, 24 hours/day */
#define REMIND_LIFE_HOURS	(11 * 30 * 24)

/* 6 hours, 360 times count */
#define FILTER_USED_COUNT	(6 * 60)

/* 1 minute, 60 seconds/minute, 1000 ms/s */
#define FILTER_TIMER_PERIOD	(60 * 1000)

/* 1 second, 1000 ms/s */
#define FILTER_LED_PERIOD	1000

/* 200ms */
#define FILTER_RESET_PERIOD	200

/* 5s = 25 * 200ms */
#define FILTER_BLINK_COUNT	25

/* 5 seconds, 1000 ms/s */
#define LONG_PRESS_PERIOD	5000

/* 1.5 second, 1000 ms/s */
#define AIRP_INIT_PERIOD	1500

/* 1.5 second, 1000 ms/s */
#define FIRST_POWER_PERIOD	1500

#define CHANGE_DATE_LEN_MAX		11
#define SERIAL_NUMBER_LEN_MAX	32
#define LOCATION_STR_LEN_MAX	64
#define GENERAL_NOTES_LEN_MAX	64
#define MARKETING_TEXT_LEN_MAX	64
#define STRING_PROP_LEN_MAX	1024

#define CFG_FILTER_USED_LEN	10

#define CFG_FILTER_USED		"airp/used"
#define CFG_FILTER_INSTALLED	"airp/installed"
#define CFG_FILTER_SERIALNO	"airp/serialno"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof(a[0]))
#define PRGNAME "Ceiling_Fan_app"

#define APP_VERS "1.0"

#define BUILD_STRING APP_VERS BUILD_SUFFIX " " \
	BUILD_DATE " " BUILD_TIME " " BUILD_VERSION

enum fan_speed_val {
	SPEED_OFF = 0x00,
	SPEED_LOW = 0x01,
	SPEED_MED = 0x02,
	SPEED_HIGH = 0x03,
};

enum led_type {
	LED_TYPE_WIFI = 0,
	LED_TYPE_FILTER = 1,
	LED_TYPE_LOW = 2,
	LED_TYPE_MED = 3,
	LED_TYPE_HIGH = 4,
};

enum led_onoff {
	LED_OFF = 0,
	LED_ON = 1,
};

enum tag_ftm_step {
	FTM_STEP_0 = 0,
	FTM_STEP_1,
	FTM_STEP_2,
	FTM_STEP_3,
	FTM_STEP_4,
	FTM_STEP_5,
	FTM_STEP_6,
	FTM_STEP_FINISH
};


/* local copies of properties' value */
static u8 filter_status;
static int filter_hours_used;
static int filter_max_life = MAX_LIFE_HOURS;
static char filter_installed_date[CHANGE_DATE_LEN_MAX];
static char filter_serial_number[SERIAL_NUMBER_LEN_MAX];
static u8 power;
static int mode = SPEED_LOW;
static int wifi_rssi;
static char app_options[STRING_PROP_LEN_MAX];

char sw_ver[] = APP_NAME " " BUILD_STRING;

struct timer_head app_ac_timers;
struct ada_lock *prop_lock;

static xQueueHandle app_logic_evt_queue;
enum app_queue_event {
	SEND_PROPS,
	RX_FROM_MCU,
};
#ifdef USE_ESP_WROOM_32E

#define GPIO_OUT_LED_WIFI      GPIO_NUM_12
#define GPIO_OUT_LED_FILTER    GPIO_NUM_14

#define GPIO_OUT_LED_LOW       GPIO_NUM_25
#define GPIO_OUT_LED_MED       GPIO_NUM_26
#define GPIO_OUT_LED_HIGH      GPIO_NUM_27

#define GPIO_OUT_MOTOR_LOW     GPIO_NUM_21
#define GPIO_OUT_MOTOR_MED     GPIO_NUM_18
#define GPIO_OUT_MOTOR_HIGH    GPIO_NUM_19

#define GPIO_IN_BUTTON_UP      GPIO_NUM_34
#define GPIO_IN_BUTTON_DOWN    GPIO_NUM_35

#else

#define GPIO_OUT_LED_WIFI      GPIO_NUM_7
#define GPIO_OUT_LED_FILTER    GPIO_NUM_6

#define GPIO_OUT_LED_LOW       GPIO_NUM_0
#define GPIO_OUT_LED_MED       GPIO_NUM_1
#define GPIO_OUT_LED_HIGH      GPIO_NUM_3

#define GPIO_OUT_MOTOR_LOW     GPIO_NUM_18
#define GPIO_OUT_MOTOR_MED     GPIO_NUM_19
#define GPIO_OUT_MOTOR_HIGH    GPIO_NUM_10

#define GPIO_IN_BUTTON_UP      GPIO_NUM_4
#define GPIO_IN_BUTTON_DOWN    GPIO_NUM_5

#endif

#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUT_LED_WIFI) \
	| (1ULL<<GPIO_OUT_LED_FILTER) | (1ULL<<GPIO_OUT_LED_LOW) \
	| (1ULL<<GPIO_OUT_LED_MED) | (1ULL<<GPIO_OUT_LED_HIGH) \
	| (1ULL<<GPIO_OUT_MOTOR_LOW) | (1ULL<<GPIO_OUT_MOTOR_MED) \
	| (1ULL<<GPIO_OUT_MOTOR_HIGH))

#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_IN_BUTTON_UP) \
	| (1ULL<<GPIO_IN_BUTTON_DOWN))

#define ESP_INTR_FLAG_DEFAULT 0

static xQueueHandle gpio_evt_queue;

struct timer wifi_led_timer;
struct timer filter_used_timer;
struct timer filter_led_timer;
struct timer filter_reset_timer;
struct timer up_long_pressed_timer;
struct timer down_long_pressed_timer;
static bool up_button_pressed;
static bool down_button_pressed;
static bool up_button_long_pressed;
static bool down_button_long_pressed;
static int used_minute_count;

static bool ftm_check;
static bool ftm_bt_ok;
unsigned long ftm_init_ms;

static bool wifi_connected;

static struct
{
	u8 app_fan_speed_setting;	/* fan_speed property value */
	u8 app_fan_power_setting;	/* fan_powerproperty value*/
	u8 app_fan_direc_setting; /* fan_direction property value*/
	u8 app_light_power_setting;	/* light_power property value */
	u8 app_light_rate_setting;	/* light_rate property value */
	u8 app_light_color_setting;	/* light_color property value */
} t_device;

static enum tag_ftm_step ftm_step;
struct timer ftm_step_timer;
struct timer ftm_stop_timer;


static struct ada_lock *prop_lock_create(const char *name)
{
	SemaphoreHandle_t lock;	/*  is assumed to be a pointer type */

	lock = xSemaphoreCreateMutex();
	if (lock == NULL) {
		printf("Unable to create mutex\n");
		return NULL;
	}
	return (struct ada_lock *)lock;
}

static void prop_lock_init(void)
{
	if (prop_lock) {
		return;
	}
	prop_lock = prop_lock_create("prop_lock");
}

static void airp_prop_lock(struct ada_lock *lockp)
{
	SemaphoreHandle_t lock = (SemaphoreHandle_t)lockp;
	if (!lock) {
		printf("Invalid arguments\n");
		return;
	}
	if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
		printf("Unable to lock mutex\n");
		return;
	}
}

static void airp_prop_unlock(struct ada_lock *lockp)
{
	SemaphoreHandle_t lock = (SemaphoreHandle_t)lockp;
	if (!lock) {
		printf("Invalid arguments\n");
		return;
	}
	if (xSemaphoreGive(lock) != pdTRUE) {
		printf("Unable to unlock mutex\n");
		return;
	}
}

static void app_timer_set(struct timer *tm, u32 delay_ms)
{
	timer_set(&app_ac_timers, tm, delay_ms);
}

static void app_timer_cancel(struct timer *tm)
{
	timer_cancel(&app_ac_timers, tm);
}

static bool app_is_in_ftm(void)
{
	if ((ftm_step > FTM_STEP_0) && (ftm_step < FTM_STEP_FINISH)) {
		return 1;
	} else {
		return 0;
	}
}
void send_all_prop()
{
	ada_sprop_send_by_name(APP_FAN_SPEED);
	ada_sprop_send_by_name(APP_FAN_PWR);
	ada_sprop_send_by_name(APP_FAN_DIR);
	ada_sprop_send_by_name(APP_LIGHT_PWR);
	ada_sprop_send_by_name(APP_LIGHT_RATING);
	ada_sprop_send_by_name(APP_LIGHT_COLOR);
	ada_sprop_send_by_name(APP_FW_VERSION);
}
static int set_motor_speed_and_led(enum fan_speed_val speed)
{
	if ((speed < SPEED_OFF) || (speed > SPEED_HIGH)) {
		log_put(LOG_ERR "%s: wrong fan speed value %d",
			__func__, speed);
		return -1;
	}

	log_put(LOG_DEBUG "%s: fan speed value %d",
		__func__, speed);

	/* Set motor speed and LED */
	gpio_set_level(GPIO_OUT_LED_LOW, 0);
	gpio_set_level(GPIO_OUT_LED_MED, 0);
	gpio_set_level(GPIO_OUT_LED_HIGH, 0);
	gpio_set_level(GPIO_OUT_MOTOR_LOW, 0);
	gpio_set_level(GPIO_OUT_MOTOR_MED, 0);
	gpio_set_level(GPIO_OUT_MOTOR_HIGH, 0);
	if (speed == SPEED_LOW) {
		gpio_set_level(GPIO_OUT_LED_LOW, 1);
		gpio_set_level(GPIO_OUT_MOTOR_LOW, 1);
	} else if (speed == SPEED_MED) {
		gpio_set_level(GPIO_OUT_LED_MED, 1);
		gpio_set_level(GPIO_OUT_MOTOR_MED, 1);
	} else if (speed == SPEED_HIGH) {
		gpio_set_level(GPIO_OUT_LED_HIGH, 1);
		gpio_set_level(GPIO_OUT_MOTOR_HIGH, 1);
	}

	if (app_is_in_ftm()) {
		log_put(LOG_DEBUG "%s: Skip used count in FTM",
			__func__);
		return 0;
	}

	if ((speed >= SPEED_LOW) && (speed <= SPEED_HIGH)) {
		if (!timer_active(&filter_used_timer)) {
			app_timer_set(&filter_used_timer, FILTER_TIMER_PERIOD);
		}
	} else {
		app_timer_cancel(&filter_used_timer);
	}

	return 0;
}
/* Date Format MM/DD/YYYY */
static int app_date_format_check(char *date)
{
	int len;
	int i;

	if (!date) {
		log_put(LOG_DEBUG "%s: date is NULL", __func__);
		return -1;
	}

	len = strlen(date);
	if ((len == 0) || (len >= CHANGE_DATE_LEN_MAX)) {
		log_put(LOG_DEBUG "%s: date %s long invalid", __func__, date);
		return -1;
	}

	/* MM */
	if (date[1] == '/') {
		if ((date[0] < '1') || (date[0] > '9')) {
			log_put(LOG_DEBUG "%s: month %s invalid",
				__func__, date);
			return -1;
		}
		date += 2;
	} else if (date[2] == '/') {
		if ((date[0] < '0') || (date[0] > '1')) {
			log_put(LOG_DEBUG "%s: month %s invalid",
				__func__, date);
			return -1;
		}
		if ((date[1] < '0') || (date[1] > '9')) {
			log_put(LOG_DEBUG "%s: month %s invalid",
				__func__, date);
			return -1;
		}
		date += 3;
	} else {
		log_put(LOG_DEBUG "%s: month %s invalid",
			__func__, date);
		return -1;
	}

	/* DD */
	if (date[1] == '/') {
		if ((date[0] < '1') || (date[0] > '9')) {
			log_put(LOG_DEBUG "%s: date %s invalid",
				__func__, date);
			return -1;
		}
		date += 2;
	} else if (date[2] == '/') {
		if ((date[0] < '0') || (date[0] > '2')) {
			log_put(LOG_DEBUG "%s: day %s invalid",
				__func__, date);
			return -1;
		}
		if ((date[1] < '0') || (date[1] > '9')) {
			log_put(LOG_DEBUG "%s: day %s invalid",
				__func__, date);
			return -1;
		}
		date += 3;
	} else {
		log_put(LOG_DEBUG "%s: day %s invalid",
			__func__, date);
		return -1;
	}

	/* YYYY */
	for (i = 0; i < 4; i++) {
		if ((date[i] < '0') || (date[i] > '9')) {
			log_put(LOG_DEBUG "%s: year %s invalid",
				__func__, date);
			return -1;
		}
	}

	return 0;
}

static int app_parse_date(char *date, int *pmonth, int *pday, int *pyear)
{
	char *ptr1, *ptr2;

	if (!date || !pmonth || !pday || !pyear) {
		log_put(LOG_DEBUG "%s: invalid argument", __func__);
		return -1;
	}

	ptr1 = strchr(date, '/');
	ptr2 = strchr(ptr1 + 1, '/');
	if (!ptr1 || !ptr2) {
		log_put(LOG_DEBUG "%s: date %s invalid", __func__, date);
		return -1;
	} else {
		if ((ptr1 - date) == 3) {
			*pmonth = (date[0] - '0') * 10 + (date[1] - '0');
		} else {
			*pmonth = (date[0] - '0');
		}

		if ((ptr2 - ptr1) == 3) {
			*pday = (ptr1[1] - '0') * 10 + (ptr1[2] - '0');
		} else {
			*pday = (ptr1[1] - '0');
		}

		*pyear = atoi(ptr2 + 1);
	}

	return 0;
}

/* Date Format MM/DD/YYYY */
static int app_cmd_date(char *date1, char *date2)
{
	int month1, month2;
	int day1, day2;
	int year1, year2;

	if (!date1 && !date2) {
		return 0;
	} else if (!date1) {
		return -1;
	} else if (!date2) {
		return 1;
	}

	if (app_parse_date(date1, &month1, &day1, &year1) < 0) {
		log_put(LOG_DEBUG "%s: date1 %s invalid", __func__, date1);
		return 0;
	}

	if (app_parse_date(date2, &month2, &day2, &year2) < 0) {
		log_put(LOG_DEBUG "%s: date2 %s invalid", __func__, date2);
		return 0;
	}

	if (year1 > year2) {
		return 1;
	} else if (year1 < year2) {
		return -1;
	}

	if (month1 > month2) {
		return 1;
	} else if (month1 < month2) {
		return -1;
	}

	if (day1 > day2) {
		return 1;
	} else if (day1 < day2) {
		return -1;
	}

	return 0;
}
/*
- set filter_serial_number
- set filter_status = 0
- set filter_hours_used = 0
- set filter_installed_date to current date
- reset filter_used timer
if (the purifier is connected to Cloud)
	update new property values to cloud
else
	store/batch the filter_hours_used values as an array in flash
fi
*/
static void app_ac_netstatus(enum libapp_net_status status)
{
	log_put(LOG_INFO "%s: status=%d", __func__, status);
	switch (status) {
	case NS_IDLE:
		wifi_connected = 0;
		wifi_rssi = 0;
		break;
	case NS_PROVISIONING:
		app_timer_set(&wifi_led_timer, 0);
		break;
	case NS_PROVISIONING_STOPPED:
		app_timer_cancel(&wifi_led_timer);
		if (wifi_connected) {
			gpio_set_level(GPIO_OUT_LED_WIFI, 1);
		} else {
			gpio_set_level(GPIO_OUT_LED_WIFI, 0);
		}
		break;
	case NS_ASSOCIATING:
		break;
	case NS_ASSOCIATED:
		wifi_connected = 1;
		app_timer_cancel(&wifi_led_timer);
		gpio_set_level(GPIO_OUT_LED_WIFI, 1);
		adap_net_get_signal(&wifi_rssi);
		break;
	case NS_CONNECTED:
		wifi_connected = 1;
		app_timer_cancel(&wifi_led_timer);
		gpio_set_level(GPIO_OUT_LED_WIFI, 1);
		adap_net_get_signal(&wifi_rssi);
		ada_sprop_send_by_name("wifi_rssi");
		break;
	case NS_DISCONNECTED:
		break;
	default:
		break;
	}
}
static void app_up_button_press(void)
{
	log_put(LOG_DEBUG "%s: UP button pressed", __func__);
	airp_prop_lock(prop_lock);
	if (power == 0) {
		power = 1;
		mode = SPEED_LOW;
		set_motor_speed_and_led(mode);
		ada_sprop_send_by_name("power");
		ada_sprop_send_by_name("mode");
	} else {
		if (mode < SPEED_HIGH) {
			mode++;
			ada_sprop_send_by_name("mode");
			set_motor_speed_and_led(mode);
		}
	}
	airp_prop_unlock(prop_lock);
	log_put(LOG_INFO "%s: power %d, mode %d", __func__, power, mode);
}
static void app_down_button_press(void)
{
	log_put(LOG_DEBUG "%s: Down button pressed", __func__);
	airp_prop_lock(prop_lock);
	if (power) {
		if (mode > SPEED_HIGH) {
			mode = SPEED_HIGH;
		}
		if (mode > SPEED_LOW) {
			mode--;
			ada_sprop_send_by_name("mode");
			set_motor_speed_and_led(mode);
		} else {
			power = 0;
			mode = SPEED_LOW;
			set_motor_speed_and_led(SPEED_OFF);
			ada_sprop_send_by_name("power");
		}
	}
	airp_prop_unlock(prop_lock);
	log_put(LOG_INFO "%s: power %d, mode %d", __func__, power, mode);
}
static void app_ftm_start(void)
{
	#ifdef AYLA_BLUETOOTH_SUPPORT
	ftm_step = FTM_STEP_1;
	if (ftm_bt_ok) {
		ftm_step = FTM_STEP_2;
		app_timer_set(&ftm_step_timer, 0);
	} else {
		app_timer_set(&ftm_step_timer, FTM_BT_PERIOD);
		log_put(LOG_INFO "%s: wait the bt ft ...", __func__);
	}
	#else
	ftm_step = FTM_STEP_2;
	app_timer_set(&ftm_step_timer, 0);
	#endif
}

static int app_start_wifi_ft(void)
{
	enum ftm_err ftm_result = FTM_ERR_NONE;

	log_put(LOG_DEBUG "%s", __func__);

	ftm_result = pha_ftm_run();
	log_put(LOG_INFO "%s:WiFi finished result %d", __func__, ftm_result);
	if (ftm_result == FTM_ERR_NONE) {
		gpio_set_level(GPIO_OUT_LED_FILTER, 0);
		ftm_step = FTM_STEP_3;
		app_timer_set(&ftm_step_timer, 0);
		return 0;
	} else {
		gpio_set_level(GPIO_OUT_LED_FILTER, 1);
		return -1;
	}
}

/*
	First:  run the purifier at HIGH fan speed for 3 seconds
	Second: run the purifier at MED fan speed for 3 seconds
	Third:  run the purifier at LOW fan speed for 3 seconds
	Forth:  enter Open Mode(Which enables the operator
			to test the UP and DOWN buttons
			to manually change fan speed).
*/
static void app_ftm_step_timeout(struct timer *tm)
{
	log_put(LOG_INFO "%s: step %d", __func__, ftm_step);
	switch (ftm_step) {
	case FTM_STEP_0:
		ftm_check = false;
		if (!gpio_get_level(GPIO_IN_BUTTON_UP)
			&& !gpio_get_level(GPIO_IN_BUTTON_DOWN)) {
			log_put(LOG_INFO "%s: FTM start", __func__);
			app_ftm_start();
		} else {
			log_put(LOG_INFO "%s: Cancel FTM", __func__);
		}
		break;
	case FTM_STEP_1:
		#ifdef AYLA_BLUETOOTH_SUPPORT
		app_timer_set(&filter_led_timer, 0);
		log_put(LOG_ERR "%s: bt factory test failed", __func__);
		al_bt_keep_up_clear(AL_BT_FUNC_PROVISION);
		#endif
		break;
	case FTM_STEP_2:
		#ifdef AYLA_BLUETOOTH_SUPPORT
		al_bt_keep_up_clear(AL_BT_FUNC_PROVISION);
		#endif
		app_start_wifi_ft();
		break;
	case FTM_STEP_3:
		power = 1;
		mode = SPEED_HIGH;
		set_motor_speed_and_led(mode);
		app_timer_set(&ftm_step_timer, FTM_STEP_PERIOD);
		ftm_step = FTM_STEP_4;
		break;
	case FTM_STEP_4:
		mode = SPEED_MED;
		set_motor_speed_and_led(mode);
		app_timer_set(&ftm_step_timer, FTM_STEP_PERIOD);
		ftm_step = FTM_STEP_5;
		break;
	case FTM_STEP_5:
		mode = SPEED_LOW;
		set_motor_speed_and_led(mode);
		app_timer_set(&ftm_step_timer, FTM_STEP_PERIOD);
		ftm_step = FTM_STEP_6;
		break;
	case FTM_STEP_6:
		/* fall-through */
	default:
		app_timer_cancel(&ftm_step_timer);
		break;
	}
}

/**
 * Notify factory test result.
 *
 * 1 - failure
 * 0 - success
 */
static void app_notify_bt_ft_OK(void)
{
	ftm_bt_ok = true;
	if (ftm_step == FTM_STEP_1) {
		log_put(LOG_INFO "%s: bt factory test OK", __func__);
		ftm_step = FTM_STEP_2;
		app_timer_set(&ftm_step_timer, 0);
	} else {
		log_put(LOG_DEBUG "%s: Ignore notification", __func__);
		return;
	}
}

static void app_set_init_ftm_status(void)
{
	if (adw_wifi_configured()) {
		adb_log(LOG_INFO "Wi-Fi configured, FT disabled");
		return;
	}

	if (!gpio_get_level(GPIO_IN_BUTTON_UP)
		&& !gpio_get_level(GPIO_IN_BUTTON_DOWN)) {
		/*ftm_step = FTM_STEP_1;*/
		ftm_check = true;
		ftm_init_ms = time_now();
		log_put(LOG_INFO "%s: need to check whether do FTM, time %lu",
			__func__, ftm_init_ms);
		#ifdef AYLA_BLUETOOTH_SUPPORT
		libapp_identify_callback_set(app_notify_bt_ft_OK);
		#endif
	}
}

static void app_ftm_stop_timeout(struct timer *tm)
{
	ftm_step = FTM_STEP_FINISH;
	log_put(LOG_INFO "%s: FTM stop", __func__);
	log_put(LOG_INFO "%s: enable_security", __func__);
	/* Call again to enable security */
	/*(void)pha_ftm_enable_security();*/
}

static void app_ftm_stop_check(void)
{
	if (up_button_pressed && down_button_pressed) {
		app_timer_set(&ftm_stop_timer, FTM_STOP_PERIOD);
	} else {
		app_timer_cancel(&ftm_stop_timer);
	}
}

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void app_gpio_port_init(void)
{
	gpio_config_t io_conf;

	/*disable interrupt*/
	io_conf.intr_type = GPIO_INTR_DISABLE;
	/*set as output mode*/
	io_conf.mode = GPIO_MODE_OUTPUT;
	/*bit mask of the pins that you want to set,e.g.GPIO18/19*/
	io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
	/*disable pull-down mode*/
	io_conf.pull_down_en = 0;
	/*disable pull-up mode*/
	io_conf.pull_up_en = 0;
	/*configure GPIO with the given settings*/
	gpio_config(&io_conf);

	gpio_set_level(GPIO_OUT_LED_WIFI, 0);
	gpio_set_level(GPIO_OUT_LED_FILTER, 0);
	gpio_set_level(GPIO_OUT_LED_LOW, 0);
	gpio_set_level(GPIO_OUT_LED_MED, 0);
	gpio_set_level(GPIO_OUT_LED_HIGH, 0);
	gpio_set_level(GPIO_OUT_MOTOR_LOW, 0);
	gpio_set_level(GPIO_OUT_MOTOR_MED, 0);
	gpio_set_level(GPIO_OUT_MOTOR_HIGH, 0);

	/*GPIO interrupt type : rising edge and falling edge*/
	io_conf.intr_type = GPIO_INTR_ANYEDGE;
	/*bit mask of the pins*/
	io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
	/*set as input mode*/
	io_conf.mode = GPIO_MODE_INPUT;
	#ifdef USE_ESP_WROOM_32E
	/*GPIO34/35 no pull-up and pull-down in ESP32E*/
	io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
	#else
	/*GPIO4/5 pull-up in C3 */
	io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	#endif
	gpio_config(&io_conf);

	/*create a queue to handle gpio event from isr*/
	gpio_evt_queue = xQueueCreate(200, sizeof(uint32_t));

	/*install gpio isr service*/
	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
	/*hook isr handler for specific gpio pin*/
	gpio_isr_handler_add(GPIO_IN_BUTTON_UP,
		gpio_isr_handler, (void *)GPIO_IN_BUTTON_UP);
	/*hook isr handler for specific gpio pin*/
	gpio_isr_handler_add(GPIO_IN_BUTTON_DOWN,
		gpio_isr_handler, (void *)GPIO_IN_BUTTON_DOWN);

	log_put(LOG_INFO "%s: Init completed", __func__);
}

static void app_gpio_evt_handler(uint32_t io_num)
{
	static unsigned long up_press, up_release;
	static unsigned long down_press, down_release;

	log_put(LOG_DEBUG "%s", __func__);

	switch (io_num) {
	case GPIO_IN_BUTTON_UP:
		if (gpio_get_level(io_num) == 0) {
			up_press = time_now();
			up_button_pressed = 1;
			if (app_is_in_ftm()) {
				app_ftm_stop_check();
			} else {
				app_timer_set(
					&up_long_pressed_timer,
					LONG_PRESS_PERIOD);
				log_put(LOG_DEBUG "set up_long_pressed_timer");
			}
		} else {
			if (up_button_pressed) {
				up_release = time_now();
				up_button_pressed = 0;
				if (app_is_in_ftm()) {
					app_ftm_stop_check();
				} else {
					app_timer_cancel(
						&up_long_pressed_timer);
					log_put(LOG_DEBUG "cancel "
						"up_long_pressed_timer");
				}
				if (up_release >= (up_press + 5000)) {
					log_put(LOG_DEBUG "up long pressed");
				} else if (up_release >= (up_press + 20)) {
					app_up_button_press();
				} else {
					/* skip */
				}
			}
		}
		break;
	case GPIO_IN_BUTTON_DOWN:
		if (gpio_get_level(io_num) == 0) {
			down_press = time_now();
			down_button_pressed = 1;
			if (app_is_in_ftm()) {
				app_ftm_stop_check();
			} else {
				app_timer_set(
					&down_long_pressed_timer,
					LONG_PRESS_PERIOD);
				log_put(LOG_DEBUG
					"set down_long_pressed_timer");
			}
		} else {
			if (down_button_pressed) {
				down_release = time_now();
				down_button_pressed = 0;
				if (app_is_in_ftm()) {
					app_ftm_stop_check();
				} else {
					app_timer_cancel(
						&down_long_pressed_timer);
					log_put(LOG_DEBUG "cancel"
						" down_long_pressed_timer");
				}
				if (down_release >= (down_press + 5000)) {
					log_put(LOG_DEBUG "down long pressed");
				} else if (down_release >= (down_press + 20)) {
					app_down_button_press();
				} else {
					/* skip */
				}
			}
		}
		break;
	default:
		log_put(LOG_DEBUG "%s: Ignore io_num %u", __func__, io_num);
		break;
	}
}

/*
 * app_logic_idle
 *
 * The main application idle loop, processing timers and events.
 * This routine is not expected to return.
 */
void app_logic_idle(void)
{
	unsigned long leff_ms;
	uint32_t io_num;
	enum app_queue_event evt;
	log_put(LOG_INFO "%s: running", __func__);

	if (ftm_check) {
		if (!gpio_get_level(GPIO_IN_BUTTON_UP)
			&& !gpio_get_level(GPIO_IN_BUTTON_DOWN)) {
			leff_ms = (time_now() - ftm_init_ms);
			log_put(LOG_INFO "%s: leff_ms %lu", __func__, leff_ms);
			if (leff_ms < FTM_CHECK_PERIOD) {
				app_timer_set(&ftm_step_timer, leff_ms);
			} else {
				log_put(LOG_INFO "%s: FTM start", __func__);
				app_ftm_start();
			}
		} else {
			ftm_check = false;
			log_put(LOG_INFO "%s: Cancel FTM check", __func__);
		}
	} else {
	}

	while (1) {
		timer_advance(&app_ac_timers);
		if (!xQueueReceive(app_logic_evt_queue,
			&evt, portMAX_DELAY)) {
			continue;
		}
		switch (evt) {
		case SEND_PROPS:
			printf("send prop to dashboard\n");
			send_all_prop();
		break;
		case RX_FROM_MCU:
			printf("Received values from MCU\n");
			t_device.app_fan_speed_setting = 4;
			t_device.app_fan_power_setting = 1;
			t_device.app_fan_direc_setting = 0;
			t_device.app_light_power_setting = 1;
			t_device.app_light_rate_setting = 3;
			t_device.app_light_color_setting = 2;
			evt = SEND_PROPS;
			xQueueSend(app_logic_evt_queue, &evt, NULL);
		break;
		}
		if (xQueueReceive(gpio_evt_queue, &io_num, pdMS_TO_TICKS(10))) {
			if ((ftm_step > FTM_STEP_0)
				&& (ftm_step < FTM_STEP_6)) {
				continue;
			}
			app_gpio_evt_handler(io_num);
		}
	}
}

static void app_filter_led_status_update(void)
{
	if (filter_hours_used >= filter_max_life) {
		/* Set filter_status 1 and set filter LED blink */
		if (filter_status != 2) {
			filter_status = 2;
			log_put(LOG_INFO "%s: filter_status %d",
				__func__, filter_status);
			ada_sprop_send_by_name("filter_status");
			app_timer_set(&filter_led_timer, 0);
		}
	} else if (filter_hours_used >= REMIND_LIFE_HOURS) {
		/* Set filter_status to 2 and set filter LED on */
		if (filter_status != 1) {
			filter_status = 1;
			log_put(LOG_INFO "%s: filter_status %d",
				__func__, filter_status);
			ada_sprop_send_by_name("filter_status");
			gpio_set_level(GPIO_OUT_LED_FILTER, 1);
			if ((timer_active(&filter_led_timer))) {
				app_timer_cancel(&filter_led_timer);
			}
		}
	} else {
		/* Set filter_status to 0 and set filter LED off */
		if (filter_status != 0) {
			filter_status = 0;
			log_put(LOG_INFO "%s: filter_status %d",
				__func__, filter_status);
			ada_sprop_send_by_name("filter_status");
			gpio_set_level(GPIO_OUT_LED_FILTER, 0);
			if ((timer_active(&filter_led_timer))) {
				app_timer_cancel(&filter_led_timer);
			}
		}
	}
}
static struct ada_sprop app_logic_props[] = {

	{ "fan_speed", ATLV_INT, &t_device.app_fan_speed_setting,
		sizeof(t_device.app_fan_speed_setting), ada_sprop_get_int,
			app_fanspeed_control },
	{ "fan_power", ATLV_BOOL, &t_device.app_fan_power_setting,
		sizeof(t_device.app_fan_power_setting), ada_sprop_get_bool,
			app_fanpower_control },
	{ "fan_direction", ATLV_BOOL, &t_device.app_fan_direc_setting,
		sizeof(t_device.app_fan_direc_setting), ada_sprop_get_bool,
			app_fandirection_control },
	{ "light_control", ATLV_BOOL, &t_device.app_light_power_setting,
		sizeof(t_device.app_light_power_setting), ada_sprop_get_bool,
			app_lightpower_control },
	{ "light_rating", ATLV_INT, &t_device.app_light_rate_setting,
		sizeof(t_device.app_light_rate_setting), ada_sprop_get_int,
			app_lightrating_control },
	{ "light_color_index", ATLV_INT, &t_device.app_light_color_setting,
		sizeof(t_device.app_light_color_setting), ada_sprop_get_int,
			app_lightcolor_control },
	{ "fw_version", ATLV_UTF8, &sw_ver[0],
		sizeof(sw_ver), ada_sprop_get_string, NULL}
};
static enum ada_err app_fanspeed_control(struct ada_sprop *sprop,
	const void *buf, size_t len)
{
	int rc = AE_OK;
	static u8 temp_fan_speed;
	u8 data_len = 1;
	/*
	 * Validate and Put the property in the local variable,
	 * using the agent's integer set function.
	 */
	 printf("fanspeed prop\n");
	airp_prop_lock(prop_lock);
	temp_fan_speed = t_device.app_fan_speed_setting;
	rc = ada_sprop_set_int(sprop, buf, len);
	if (rc < 0) {
		printf("rc error\n");
		airp_prop_unlock(prop_lock);
		return rc;
	}
	printf("fan speed valc=%d\n", t_device.app_fan_speed_setting);
	log_put(LOG_DEBUG "%s: Set fan speed to %d\n",
		__func__, t_device.app_fan_speed_setting);
	libapp_conf_set_blob(APP_FAN_SPEED, &t_device.app_fan_speed_setting,
		sizeof(t_device.app_fan_speed_setting), 0);
	if (t_device.app_fan_speed_setting >= 1 &&
		t_device.app_fan_speed_setting <= 6) {
		log_put(LOG_INFO "set fan property\n");
	}
	airp_prop_unlock(prop_lock);
	return rc;
}
static enum ada_err app_fanpower_control(struct ada_sprop *sprop,
	const void *buf, size_t len)
{
	int rc = AE_OK;
	u8 temp_fan_power;
	u8 data_len = 1;
	printf("fanpower control\n");
	/*
	 * Validate and Put the property in the local variable,
	 * using the agent's integer set function.
	 */
	airp_prop_lock(prop_lock);
	temp_fan_power = t_device.app_fan_power_setting;
	rc = ada_sprop_set_int(sprop, buf, len);
	if (rc < 0) {
		airp_prop_unlock(prop_lock);
		return rc;
	}
	log_put(LOG_DEBUG "%s: Set fan power to %d\n",
		__func__, t_device.app_fan_power_setting);
		airp_prop_unlock(prop_lock);
	return rc;
}
static enum ada_err app_fandirection_control(struct ada_sprop *sprop,
	const void *buf, size_t len)
{
	int rc = AE_OK;
	static u8 temp_fandirection;
	u8 data_len = 1;
	printf("fandirec control\n");
	/*
	 * Validate and Put the property in the local variable,
	 * using the agent's integer set function.
	 */
	airp_prop_lock(prop_lock);
	temp_fandirection = t_device.app_fan_direc_setting;
	rc = ada_sprop_set_int(sprop, buf, len);
	printf("fan dir valc=%d\n", t_device.app_fan_direc_setting);
	if (rc < 0) {
		airp_prop_unlock(prop_lock);
		return rc;
	}
	airp_prop_unlock(prop_lock);
	return rc;
}
static enum ada_err app_lightpower_control(struct ada_sprop *sprop,
	const void *buf, size_t len)
{
	int rc = AE_OK;
	static u8 temp_light_power;
	u8 data_len = 1;
	printf("lightpower control\n");
	/*
	 * Validate and Put the property in the local variable,
	 * using the agent's integer set function.
	 */
	airp_prop_lock(prop_lock);
	temp_light_power = t_device.app_light_power_setting;
	rc = ada_sprop_set_int(sprop, buf, len);
	if (rc < 0) {
		airp_prop_unlock(prop_lock);
		return rc;
	}
	printf("light powctr=%d\n", t_device.app_light_power_setting);
	airp_prop_unlock(prop_lock);
	return rc;
}
static enum ada_err app_lightrating_control(struct ada_sprop *sprop,
	const void *buf, size_t len)
{
	int rc = AE_OK;
	static u8 temp_light_rating_control;
	u8 data_len = 1;
	printf("lightrating control\n");
	/*
	 * Validate and Put the property in the local variable,
	 * using the agent's integer set function.
	 */
	airp_prop_lock(prop_lock);
	rc = ada_sprop_set_int(sprop, buf, len);
	temp_light_rating_control = t_device.app_light_rate_setting;
	if (rc < 0) {
		airp_prop_unlock(prop_lock);
		return rc;
	}
	printf("lightrating=%d\n", t_device.app_light_rate_setting);
	airp_prop_unlock(prop_lock);
	return rc;
}
static enum ada_err app_lightcolor_control(struct ada_sprop *sprop,
	const void *buf, size_t len)
{
	int rc = AE_OK;
	static u8 temp_light_color_control;
	u8 data_len = 1;
	printf("lightcolor control\n");
	/*
	 * Validate and Put the property in the local variable,
	 * using the agent's integer set function.
	 */
	airp_prop_lock(prop_lock);
	rc = ada_sprop_set_int(sprop, buf, len);
	temp_light_color_control = t_device.app_light_color_setting;
	if (rc < 0) {
		airp_prop_unlock(prop_lock);
		return rc;
	}
	printf("lightcolor=%d\n", t_device.app_light_color_setting);
	airp_prop_unlock(prop_lock);
	/* new send_color_to_mcu(int color);
	tx(pkt);*/
	return rc;
}

/*
 * app_logic_init
 *
 * Initialize the AC protocol and get AC type from MCU
 * This routine is called before the agent starts
 */
void app_logic_init(void)
{
	char used_str[CFG_FILTER_USED_LEN];
	int len;
	struct timeval tv;
	struct clock_info clk;

	len = libapp_conf_get_string(CFG_FILTER_USED,
		used_str, sizeof(used_str));
	if (len <= 0) {
		log_put(LOG_INFO "%s: %s Conf Read Error",
			__func__, CFG_FILTER_USED);
		filter_hours_used = 0;
	} else {
		filter_hours_used = atoi(used_str);
	}

	len = libapp_conf_get_string(CFG_FILTER_INSTALLED,
		filter_installed_date, sizeof(filter_installed_date));
	if (len <= 0) {
		log_put(LOG_INFO "%s: %s Conf Read Error",
			__func__, CFG_FILTER_INSTALLED);
		memset(filter_installed_date, 0, sizeof(filter_installed_date));
		gettimeofday(&tv, NULL);
		clock_fill_details(&clk, tv.tv_sec);
		snprintf(filter_installed_date, sizeof(filter_installed_date),
			"%2.2u/%2.2u/%4.4lu", clk.month, clk.days, clk.year);
	}

	len = libapp_conf_get_string(CFG_FILTER_SERIALNO,
		filter_serial_number, sizeof(filter_serial_number));
	if (len <= 0) {
		log_put(LOG_INFO "%s: %s Conf Read Error",
			__func__, CFG_FILTER_SERIALNO);
		memset(filter_serial_number, 0, sizeof(filter_serial_number));
	}

	ayla_timer_init(&ftm_step_timer, app_ftm_step_timeout);
	ayla_timer_init(&ftm_stop_timer, app_ftm_stop_timeout);
	/*ayla_timer_init(&wifi_led_timer, app_wifi_led_1hz_timeout);
	ayla_timer_init(&filter_used_timer, app_filter_used_timeout);
	ayla_timer_init(&filter_led_timer, app_filter_led_timeout);
	ayla_timer_init(&filter_reset_timer, app_filter_reset_timeout);
	ayla_timer_init(&up_long_pressed_timer,
		app_up_long_pressed_timeout);
	ayla_timer_init(&down_long_pressed_timer,
		app_down_long_pressed_timeout);*/
}

/*
 * app_logic_prop_init
 *
 * Initialize the application properties, and report initial device state
 * This routine is called after the agent starts.
 */
void app_logic_prop_init(void)
{
	/* Set default Wi-Fi setup timeout to 3 minutes */
	al_bt_wifi_timeout_set(3 * 60 * 1000);
	app_logic_evt_queue = xQueueCreate(10, sizeof(enum app_queue_event));
	/*
	 * Set up the event queue and register properties
	 */
	prop_lock_init();
	ada_sprop_mgr_register("airp",
	    app_logic_props, ARRAY_LEN(app_logic_props));
	libapp_net_event_register(app_ac_netstatus);

	app_gpio_port_init();
	app_set_init_ftm_status();
	/*app_filter_led_status_update();*/
}
