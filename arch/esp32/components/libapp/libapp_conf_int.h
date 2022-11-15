/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBAPP_CONF_H__
#define __AYLA_LIBAPP_CONF_H__

/*
 * string length limits
 */
#define CONF_PATH_STR_MAX	64	/* max len of conf variable name */
#define CONF_ADS_HOST_MAX (CONF_OEM_MAX + 1 + CONF_MODEL_MAX + 1 + 24)
					/* max ADS hostname length incl NUL */
#define ADA_PUB_KEY_LEN	400

#define SETUP_KEY_LENGTH	30	/* length of base64(sha1 hash) + 1 */
#define SETUP_KEY_CONF		"setup_mode/key"	/* in app space */
#define SETUP_MODE_CONF		"sys/setup_mode"
#define LIBAPP_DSN_CONF		"id/dev_id"
#define LIBAPP_OEM_ID_CONF	"oem/oem"
#define LIBAPP_OEM_MODEL_CONF	"oem/model"
#define LIBAPP_OEM_KEY_CONF	"oem/key"
#define LIBAPP_CONF_APP "app"		/* app config prefix */
#define LIBAPP_SETUP_KEY_CONF	(LIBAPP_CONF_APP "/" SETUP_KEY_CONF)
#define LIBAPP_HOST_PREF_CONF	"host/prefix"

extern const char mod_sw_version[];

extern u8 libapp_cli_enable;	/* non-zero if CLI can be used in user mode */
extern u8 libapp_cons_enable;	/* non-zero if UART can be used in user mode */

void libapp_conf_init(void);

void libapp_cli_init(void);
void libapp_nvs_init(void);

esp_err_t libapp_conf_cli(int argc, char **argv);
extern const char libapp_conf_cli_help[];

esp_err_t libapp_diag_cli(int argc, char **argv);
extern const char libapp_diag_cli_help[];

/*
 * Load configuration items from OEM config script.
 */
void libapp_conf_load_cli(const char *in);

/*
 * Check setup-mode key before allowing config changes in user mode.
 */
int libapp_cli_setup_mode_key_check(const char *guess);

/*
 * Set setup_mode re-enable key.
 */
int libapp_cli_setup_mode_key_set(const char *val);

/*
 * Remove startup config items from NVS (factory reset, without the reset).
 * If app is non-zero, erase app-configured items as well.
 */
int libapp_conf_startup_erase(int app);

void libapp_ota_init(void);

void libapp_net_event_init(void);

#endif /* __AYLA_LIBAPP_CONF_H__ */
