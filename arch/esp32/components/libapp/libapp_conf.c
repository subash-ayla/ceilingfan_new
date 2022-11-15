/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "nvs.h"
#include "soc/rtc.h"
#include "time.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_console.h"
#include "esp_spi_flash.h"
#include "esp_efuse.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <ayla/utypes.h>
#include <ayla/clock.h>
#include <ayla/parse.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/tlv.h>
#include <ayla/conf.h>

#include <ada/libada.h>
#include <net/base64.h>
#include <net/net.h>
#include <libapp/libapp.h>
#include "libapp_conf_int.h"
#include "libapp_conf_wifi.h"
#include "libapp_nvs_int.h"

#include "libapp_command_table.h"	/* initializes libapp_tokens[] table */

#define LIBAPP_CONF_TOK_LEN	15	/* max name length imposed by NVS */
#define LIBAPP_CONF_STARTUP_PREF '_'	/* prefix for startup config */
#define LIBAPP_CONF_APP_F_PREF '.'	/* pfx for app-private factory config */
#define LIBAPP_CONF_APP_S_PREF ':'	/* pfx for app-private startup config */
#define LIBAPP_OEM_ENCR_STACK	5500	/* stack for encrypting OEM key */
#define LIBAPP_CONF_RESET_CBS	4	/* number of reset callbacks allowed */
#define LIBAPP_CONF_HASH "conf/hash"	/* hash of factory config in "app/" */

/*
 * The mfg_model and mfg_serial are read from the configuration.
 * These are the defaults.
 */
char conf_sys_mfg_model[CONF_MODEL_MAX] =
#if CONFIG_IDF_TARGET_ESP32C3
	"esp32c3";
#elif CONFIG_IDF_TARGET_ESP32
	"esp32";
#else
#error target not supported
#endif
char conf_sys_mfg_serial[CONF_MFG_SN_MAX] = "p1";

/*
 * OEM and OEM model should be configured with the CLI in setup mode.
 */
char oem[CONF_OEM_MAX + 1];
char oem_model[CONF_OEM_MAX + 1];
static char conf_hw_id[32];
static void (*libapp_conf_factory_reset_callback[LIBAPP_CONF_RESET_CBS])(void);

/*
 * List of strings that indicate secret configuration items.
 * If any part of a config item name matches one of these strings,
 * that item should not be shown on the console.
 */
static const char *libapp_conf_secret[] = {
	"/key",
	"/private_key"
};

static int libapp_conf_is_secret(const char *name)
{
	const char **sp;

	for (sp = libapp_conf_secret;
	    sp < &libapp_conf_secret[ARRAY_LEN(libapp_conf_secret)]; sp++) {
		if (strstr(name, *sp)) {
			return 1;
		}
	}
	return 0;
}

/*
 * Lookup token in table by long name.
 */
static const char *libapp_conf_short_token(const char *token)
{
	const struct libapp_token *tok;

	for (tok = libapp_tokens;
	    tok < &libapp_tokens[ARRAY_LEN(libapp_tokens)]; tok++) {
		if (!strcmp(token, tok->name)) {
			return tok->short_name;
		}
	}
	return NULL;
}

/*
 * Lookup full token by short name.
 */
static const char *libapp_conf_full_token(const char *short_token)
{
	const struct libapp_token *tok;

	for (tok = libapp_tokens;
	     tok < &libapp_tokens[ARRAY_LEN(libapp_tokens)]; tok++) {
		if (!strcmp(short_token, tok->short_name)) {
			return tok->name;
		}
	}
	return NULL;
}

/*
 * Check NVS short names vs. ADA tokens.
 */
static void libapp_conf_token_check(void)
{
	enum conf_token tk;
	const char *name;
	const char *short_name;
	const struct libapp_token *tok;
	const char *prev = NULL;

	for (tk = 0; tk < CT_TOTAL; tk++) {
		name = conf_string(tk);
		if (!name) {
			continue;
		}

		/*
		 * Every ADA token should have a short token mapping.
		 * Even "n".
		 */
		short_name = libapp_conf_short_token(name);
		if (!short_name) {
			log_put(LOG_ERR "%s: missing token 0x%x \"%s\"",
			    __func__, tk, name);
		}
	}
	for (tok = libapp_tokens;
	     tok < &libapp_tokens[ARRAY_LEN(libapp_tokens)]; tok++) {
		tk = conf_token_parse(tok->name);

		/*
		 * An extra token could be OK for backwards-compatibility.
		 */
		if (tk == CT_INVALID_TOKEN) {
			log_put(LOG_WARN "%s: extra token \"%s\"",
			    __func__, tok->name);
		}

		/*
		 * Don't allow tokens to be out of order or
		 * duplicates could creep in.
		 */
		if (prev && strcmp(prev, tok->short_name) >= 0) {
			log_put(LOG_ERR "%s: tokens out of order \"%s\" \"%s\"",
			    __func__, prev, tok->short_name);
		}
		prev = tok->short_name;
	}
}

/*
 * Replace name with a shorter version, with 1 byte of prefix added.
 *
 * Caller must supply buffer with length of at least 2 bytes, preferably 16.
 * Return is pointer to buffer with NUL termination, or NULL on failure.
 *
 * The NVS names are formed as follows:
 * 1. For startup (non-factory) config names, prepend "_".
 * 2. If name with prefix (used or not) would be 15 bytes or less, and
 *    doesn't begin with an upper-case letter, use it directly.
 * 3. Split names into tokens separated by / and replace those tokens with
 *    a two-character code starting with an upper-case letter.  If the
 *    token doesn't match a name in the table, and doesn't contain an
 *    upper-case letter, use it directly as a literal.
 * 4. Slashes are not included except between literals.
 */
static char *libapp_conf_nvs_name(const char *name, char *buf, size_t len)
{
	char tok[CONF_PATH_STR_MAX];
	const char *short_token;
	const char *in = name;
	char *out = buf;
	size_t tlen;
	unsigned int after_literal = 0;
	const char *cp;

	/*
	 * Prepend the startup prefix.
	 */
	*out++ = LIBAPP_CONF_STARTUP_PREF;
	len--;
	*out = '\0';

	/*
	 * If name is short enough, just use it.
	 * Don't allow a short name starting with an upper-case letter, though.
	 */
	tlen = strlen(name);
	if (tlen < len && tlen < LIBAPP_CONF_TOK_LEN && !isupper(name[0])) {
		memcpy(out, name, tlen + 1);
		return buf;
	}

	/*
	 * Tokenize name.
	 */
	while (*in) {
		/*
		 * Copy up to the next / into tok[].
		 */
		cp = strchr(in, '/');
		if (cp) {
			tlen = cp - in;
		} else {
			tlen = strlen(in);
			cp = in + tlen;
		}
		if (tlen >= sizeof(tok) - 1) {
			return NULL;
		}
		memcpy(tok, in, tlen);
		tok[tlen] = '\0';
		in = cp;
		if (in[0] == '/') {	/* if not at end, skip slash */
			in++;
		}

		/*
		 * Lookup the token.
		 * If not found, use literally (it may be a number).
		 * If it contains uppercase, don't allow it to be literal.
		 */
		short_token = libapp_conf_short_token(tok);
		if (short_token) {
			after_literal = 0;
		} else {
			for (cp = tok + 1; *cp != '\0'; cp++) {
				if (isupper(*cp)) {
					return NULL;
				}
			}
			short_token = tok;
			if (after_literal) {
				/*
				 * Put slash between literals.
				 */
				if (len < 2) {
					return NULL;
				}
				len--;
				*out++ = '/';
				*out = '\0';
			}
			after_literal = 1;
		}

		/*
		 * Append short name to output.
		 */
		tlen = strlen(short_token);
		if (tlen >= len) {
			return NULL;
		}
		memcpy(out, short_token, tlen + 1);	/* include NUL */
		len -= tlen;
		out += tlen;
	}
	return buf;
}

/*
 * Read blob from NVS.
 */
static int libapp_conf_nvs_get(nvs_handle_t nvs, const char *nvs_name,
				void *buf, size_t buf_len)
{
	esp_err_t rc;
	size_t len = buf_len;
	int rlen;

	ASSERT(nvs);
	rc = nvs_get_blob(nvs, nvs_name, NULL, &len);
	if (rc == ESP_ERR_NVS_NOT_FOUND) {
		rlen = AE_NOT_FOUND;
	} else if (rc != ESP_OK) {
		log_put(LOG_ERR "%s: \"%s\" failed rc %d",
		    __func__, nvs_name, rc);
		rlen = AE_ERR;
	} else {
		rlen = len;
		if (len) {
			rc = nvs_get_blob(nvs, nvs_name, buf, &len);
			rlen = (int)len;
			if (rc != ESP_OK) {
				log_put(LOG_ERR "%s: \"%s\" failed rc %d",
				    __func__, nvs_name, rc);
				rlen = AE_ERR;
			}
		}
	}
	return rlen;
}

/*
 * Get ADA string config item.
 * Returns length of value or -1 on failure.
 */
static int libapp_ada_conf_get(nvs_handle_t nvs, const char *name,
				void *buf, size_t buf_len, u8 factory)
{
	char name_buf[LIBAPP_CONF_TOK_LEN + 1];
	char *nvs_name;
	int len = -1;

	nvs_name = libapp_conf_nvs_name(name, name_buf, sizeof(name_buf));
	if (!nvs_name) {
		log_put(LOG_ERR "%s: name invalid \"%s\"", __func__, name);
		return -1;
	}

	/*
	 * Try startup config first if factory flag isn't set.
	 * A zero-length result should be an empty name, not a failure.
	 */
	if (!factory) {
		len = libapp_conf_nvs_get(nvs, nvs_name, buf, buf_len);
	}
	if (len < 0) {
		/*
		 * Try factory config
		 */
		len = libapp_conf_nvs_get(nvs, nvs_name + 1, buf, buf_len);
	}
	return len;
}

/*
 * Get string config item.
 * Returns length of value or -1 on failure.
 */
int adap_conf_get(const char *name, void *buf, size_t buf_len)
{
	return libapp_ada_conf_get(libapp_nvs, name, buf, buf_len, 0);
}

/*
 * Set value in NVS.
 * Delete the item if the length is zero.
 */
static int libapp_conf_nvs_set(nvs_handle_t nvs, const char *nvs_name,
    const void *val, size_t len)
{
	esp_err_t rc;
	u8 buf[20];		/* max size to check for pre-existing value */
	size_t olen = sizeof(buf);

	/*
	 * Read to see if it's already there.
	 * This helps us skip erases, which are very slow.
	 */
	rc = nvs_get_blob(nvs, nvs_name, buf, &olen);
	if (!len) {
		if (rc == ESP_ERR_NVS_NOT_FOUND) {
			return 0;
		}

		/*
		 * Delete the token
		 */
		rc = nvs_erase_key(nvs, nvs_name);
		if (rc != ESP_OK && rc != ESP_ERR_NVS_NOT_FOUND) {
			log_put(LOG_ERR "%s: nvs_erase_key \"%s\" failed rc %d",
			    __func__, nvs_name, rc);
			return -1;
		}
	} else {
		if (!rc && len == olen && !memcmp(val, buf, len)) {
			return 0;
		}
		rc = nvs_set_blob(nvs, nvs_name, val, len);
		if (rc != ESP_OK) {
			log_put(LOG_ERR "%s: nvs_set_blob \"%s\" failed rc %d",
			    __func__, nvs_name, rc);
			return -1;
		}
	}
	rc = nvs_commit(nvs);
	if (rc != ESP_OK && rc != ESP_ERR_NVS_INVALID_HANDLE) {
		log_put(LOG_ERR "%s: nvs_commit \"%s\" failed rc %d",
		    __func__, nvs_name, rc);
		return -1;
	}
	return 0;
}

/*
 * Set config item, which may be in non-string format.
 * Returns 0 on success, -1 on failure.
 */
static int libapp_ada_conf_set(nvs_handle_t nvs, const char *var,
				const void *val, size_t len, u8 factory)
{
	char name_buf[LIBAPP_CONF_TOK_LEN + 1];
	char *startup_name;
	char *nvs_name;

	startup_name = libapp_conf_nvs_name(var, name_buf, sizeof(name_buf));
	if (!startup_name) {
		log_put(LOG_ERR "%s: name invalid \"%s\"", __func__, var);
		return -1;
	}
	if (factory) {
		nvs_name = startup_name + 1;
	} else {
		nvs_name = startup_name;
	}
	if (libapp_conf_nvs_set(nvs, nvs_name, val, len)) {
		return -1;
	}

	if (factory) {
		/*
		 * Delete the startup token if it exists
		 */
		if (libapp_conf_nvs_set(nvs, startup_name, NULL, 0)) {
			return -1;
		}
	}
	return 0;
}

/*
 * Set config item, which may be in non-string format.
 * Returns 0 on success, -1 on failure.
 */
int adap_conf_set(const char *var, const void *val, size_t len)
{
	u8 factory;

	factory = conf_setup_mode || conf_setup_pending ||
	    !strcmp(var, SETUP_MODE_CONF);
	return libapp_ada_conf_set(libapp_nvs, var, val, len, factory);
}

/*
 * Get name for app config item.
 */
static char *libapp_app_nvs_name(const char *name, char *buf, size_t len,
    u8 factory)
{
	int tlen;

	tlen = snprintf(buf, len, "%c%s",
	    factory ? LIBAPP_CONF_APP_F_PREF : LIBAPP_CONF_APP_S_PREF, name);
	if (tlen >= len) {
		log_put(LOG_ERR "%s: name \"%s\" too long", __func__, name);
		return NULL;
	}
	return buf;
}

/*
 * Get config item for app as binary blob.
 */
int libapp_conf_get(const char *name, void *buf, size_t buf_len)
{
	char nvs_name[LIBAPP_CONF_TOK_LEN + 1];
	int rc;

	/*
	 * Check for item in startup config.
	 */
	if (libapp_app_nvs_name(name, nvs_name, sizeof(nvs_name), 0)) {
		rc = libapp_conf_nvs_get(libapp_nvs, nvs_name, buf, buf_len);
		if (rc > 0) {
			return rc;
		}
	}

	/*
	 * Check for item in factory config.
	 */
	if (libapp_app_nvs_name(name, nvs_name, sizeof(nvs_name), 1)) {
		rc = libapp_conf_nvs_get(libapp_nvs, nvs_name, buf, buf_len);
		if (rc > 0) {
			return rc;
		}
	}

	return -1;
}

/*
 * Like libapp_conf_get(), but guarantee's string NUL termination.
 * Returns -1 if the string value with termination doesn't fit.
 */
int libapp_conf_get_string(const char *name, char *buf, size_t buf_len)
{
	int len;

	len = libapp_conf_get(name, buf, buf_len);
	if (len < 0 || len >= buf_len) {
		return -1;
	}
	buf[len] = '\0';
	return len;
}

/*
 * Like libapp_conf_get(), but parses the result as an integer.
 * Returns -1 if the item is not present or not parsable as a long integer.
 */
int libapp_conf_long_get(const char *name, long *result)
{
	int len;
	char buf[20];
	long val;
	char *errptr;

	len = libapp_conf_get_string(name, buf, sizeof(buf));
	if (len < 0) {
		return -1;
	}
	val = strtol(buf, &errptr, 0);
	if (errptr == buf || *errptr) {
		return -1;
	}
	*result = val;
	return 0;
}

/*
 * Set config item for app as binary blob.
 * Note that a length of 0 will delete the item.
 */
int libapp_conf_set_blob(const char *name, const void *buf, size_t len,
    u8 factory)
{
	char nvs_name[LIBAPP_CONF_TOK_LEN + 1];

	if (!libapp_app_nvs_name(name, nvs_name, sizeof(nvs_name), factory)) {
		return -1;
	}
	return libapp_conf_nvs_set(libapp_nvs, nvs_name, buf, len);
}

/*
 * Set config item for app as string.
 * Note that an empty string will delete the item.
 */
int libapp_conf_set(const char *name, const char *val, u8 factory)
{
	char nvs_name[LIBAPP_CONF_TOK_LEN + 1];

	if (!libapp_app_nvs_name(name, nvs_name, sizeof(nvs_name), factory)) {
		return -1;
	}
	return libapp_conf_nvs_set(libapp_nvs, nvs_name, val, strlen(val));
}

/*
 * Set state item, use unencrypted partition if available.
 */
int libapp_conf_state_set(const char *name, const void *val, size_t len)
{
	if (libapp_nvs_state) {
		return libapp_conf_nvs_set(libapp_nvs_state, name, val, len);
	}
	if (!libapp_nvs) {
		return -1;
	}
	return libapp_conf_set_blob(name, val, len, 1);
}

/*
 * Get state item, use unencrypted partition if available.
 */
int libapp_conf_state_get(const char *name, void *buf, size_t len)
{
	if (libapp_nvs_state) {
		return libapp_conf_nvs_get(libapp_nvs_state, name, buf, len);
	}
	if (!libapp_nvs) {
		return -1;
	}
	return libapp_conf_get(name, buf, len);
}

/*
 * Erase all startup items
 * If app is non-zero, erase app-configured factory items as well.
 */
int libapp_conf_startup_erase(int app)
{
	esp_err_t rc;
	nvs_handle_t nvs = libapp_nvs;
	nvs_iterator_t iter;
	nvs_entry_info_t info;
	unsigned int count = 0;

	libapp_conf_token_check();

	iter = nvs_entry_find("nvs", AYLA_STORAGE, NVS_TYPE_ANY);
	while (iter) {
		nvs_entry_info(iter, &info);
		iter = nvs_entry_next(iter);
		if (info.key[0] == LIBAPP_CONF_STARTUP_PREF ||
		    (info.key[0] == LIBAPP_CONF_APP_S_PREF) ||
		    (app && info.key[0] == LIBAPP_CONF_APP_F_PREF)) {
			rc = nvs_erase_key(nvs, info.key);
			if (rc) {
				log_put(LOG_ERR
				    "%s: erase key '%s' failed rc %d",
				    __func__, info.key, rc);
				return -1;
			}
			count++;
		}
	}
	nvs_commit(nvs);
	return 0;
}

/*
 * Reset config item to factory setting
 */
int adap_conf_reset_factory(const char *var)
{
	char name_buf[LIBAPP_CONF_TOK_LEN + 1];
	char *startup_name;
	nvs_handle_t nvs = libapp_nvs;
	esp_err_t rc;
	u8 buf[8];
	size_t olen = sizeof(buf);

	startup_name = libapp_conf_nvs_name(var, name_buf, sizeof(name_buf));
	if (!startup_name) {
		log_put(LOG_ERR "%s: name invalid \"%s\"", __func__, var);
		return -1;
	}

	/*
	 * Read to see if it's already there.
	 * This helps us skip erases, which are very slow.
	 */
	rc = nvs_get_blob(nvs, startup_name, buf, &olen);
	if (rc == ESP_ERR_NVS_NOT_FOUND) {
		return 0;
	}
	nvs_erase_key(nvs, startup_name);
	nvs_commit(nvs);
	return 0;
}

/*
 * Perform a factory reset.
 * Does not reboot, so caller should do that if desired.
 */
void libapp_conf_factory_reset(void)
{
	unsigned int i;

	libapp_conf_startup_erase(0);
	for (i = 0; i < ARRAY_LEN(libapp_conf_factory_reset_callback); i++) {
		if (libapp_conf_factory_reset_callback[i]) {
			libapp_conf_factory_reset_callback[i]();
		}
	}
}

/*
 * Reset the module, optionally to the factory configuration.
 * If factory is non-zero, the platform should remove any Wi-Fi configuration
 * and other customer-specific information.  The cloud configuration should
 * already have been put back to the factory settings in this case.
 */
void adap_conf_reset(int factory)
{
	conf_log(LOG_INFO "Ayla Agent requested %sreset",
	    factory ? "factory " : "");
	if (factory) {
		libapp_conf_factory_reset();
	}
	esp_restart();
}

/*
 * Register a callback which will be called when performing a factory reset.
 * Only a small number of these can be registered, and there's no unregister.
 */
void libapp_conf_factory_reset_callback_set(void (*handler)(void))
{
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(libapp_conf_factory_reset_callback); i++) {
		if (libapp_conf_factory_reset_callback[i] == NULL) {
			libapp_conf_factory_reset_callback[i] = handler;
			return;
		}
	}
	conf_log(LOG_ERR "conf: no free factory reset callbacks");
}

/*
 * If OEM key is unencrypted (starting with "encr ") in NVS,
 * encrypt it with the public key.
 * This would be done only for re-flashed devices in the module
 * factory, where the DSN is re-used from the ID partition.
 *
 * This runs in a separate thread because encryption requires a lot of stack.
 */
static void libapp_conf_oem_key_crypt(void)
{
	const char key_cmd[] = "encr ";	/* start of cleartext oem secret */
	char buf[CLIENT_CONF_PUB_KEY_LEN];
	char pub_key_b64[BASE64_LEN_EXPAND(CLIENT_CONF_PUB_KEY_LEN)];
	char pub_key[CLIENT_CONF_PUB_KEY_LEN];
	char secret[CLIENT_CONF_PUB_KEY_LEN];
	int pub_key_len;
	size_t out_len;
	int len;

	len = adap_conf_get(LIBAPP_OEM_KEY_CONF, buf, sizeof(buf));
	if (len <= 0) {
		log_put(LOG_INFO "%s: no oem key", __func__);
		return;
	}

	/*
	 * Check for OEM key in cleartext prefixed by "encr ".
	 */
	if (len < sizeof(key_cmd) ||
	    memcmp(buf, key_cmd, sizeof(key_cmd) - 1)) {
		log_put(LOG_INFO "prefix mismatch");
		return;
	}
	len -= sizeof(key_cmd) - 1;
	memcpy(secret, buf + sizeof(key_cmd) - 1, len);
	secret[len] = '\0';

	/*
	 * Get public key.  It may not have been read from the config yet.
	 */
	pub_key_len = adap_conf_get(ADA_PSM_ID_PUB_KEY,
	    pub_key_b64, sizeof(pub_key_b64));
	if (pub_key_len <= 0) {
		log_put(LOG_ERR "%s: pub key not set", __func__);
		return;
	}

	/*
	 * Base64-decode the public key.
	 */
	out_len = sizeof(pub_key);
	if (net_base64_decode(pub_key_b64, pub_key_len, pub_key, &out_len)) {
		log_put(LOG_ERR "%s: pub key decode failed", __func__);
		return;
	}
	pub_key_len = out_len;

	/*
	 * The string to be encrypted is the secret followed by the oem model,
	 * from the cleartext oem/key config item.
	 */
	len = client_auth_encrypt(pub_key, pub_key_len,
	    buf, sizeof(buf), secret);
	if (len <= 0) {
		log_put(LOG_ERR "%s: encrypt failed rc %d", __func__, len);
		return;
	}
	libapp_ada_conf_set(libapp_nvs, LIBAPP_OEM_KEY_CONF, buf, len, 1);
}

static void libapp_conf_oem_key_crypt_task(void *arg)
{
	SemaphoreHandle_t sema = arg;

	libapp_conf_oem_key_crypt();
	xSemaphoreGive(sema);
	vTaskDelete(NULL);
}

/*
 * Copy ADA config item between main NVS and ID NVS.
 */
static void libapp_conf_id_copy(nvs_handle_t from, nvs_handle_t to,
		 const char *name)
{
	char buf[ADA_PUB_KEY_LEN];
	int len;

	len = libapp_ada_conf_get(from, name, buf, sizeof(buf), 1);
	if (len <= 0) {
		log_put(LOG_ERR "%s: get of \"%s\" failed", __func__, name);
		return;
	}
	if (libapp_ada_conf_set(to, name, buf, len, 1)) {
		log_put(LOG_ERR "%s: set of \"%s\" failed", __func__, name);
		return;
	}
}

/*
 * Initialize "ID" configuration.
 * Keep DSN and key, and possibly other information in the separate ID NVS
 * partition in case re-configuring the device is required.
 * If one of these partitions has no DSN, copy the DSN and key from the other.
 * If NVS has the OEM key not yet encrypted by the public key, do that now.
 */
static void libapp_conf_id_init(void)
{
	char main_buf[CONF_OEM_KEY_MAX];
	char id_buf[CONF_DEV_ID_MAX];
	int main_len;
	int id_len;
	nvs_handle_t from;
	nvs_handle_t to;
	nvs_handle_t libapp_nvs_id;
	SemaphoreHandle_t sema = NULL;

	if (!libapp_nvs) {
		return;
	}
	libapp_nvs_id = libapp_nvs_id_init();
	if (!libapp_nvs_id) {
		return;
	}
	main_len = libapp_ada_conf_get(libapp_nvs, LIBAPP_DSN_CONF,
	    main_buf, sizeof(main_buf), 1);
	id_len = libapp_ada_conf_get(libapp_nvs_id, LIBAPP_DSN_CONF,
	    id_buf, sizeof(id_buf), 1);
	if (main_len <= 0 && id_len <= 0) {
		goto out;
	}
	if (main_len > 0) {
		if (main_len == id_len && !memcmp(main_buf, id_buf, main_len)) {
			goto out;
		}
		if (id_len > 0) {
			log_put(LOG_WARN "%s: DSN mismatch with ID conf",
			    __func__);
		}
		log_put(LOG_INFO "%s: initializing ID config", __func__);
		from = libapp_nvs;
		to = libapp_nvs_id;
	} else {
		log_put(LOG_INFO "%s: restoring from ID config", __func__);
		from = libapp_nvs_id;
		to = libapp_nvs;
	}

	/*
	 * Copy items to/from ID partition.
	 */
	libapp_conf_id_copy(from, to, ADA_PSM_ID_PUB_KEY);

	/*
	 * To encrypt OEM secret, we usually won't have enough stack space.
	 * Create a separate task and wait for it to finish.
	 */
	if (to == libapp_nvs) {
		vSemaphoreCreateBinary(sema);
		if (!sema) {
			log_put(LOG_ERR "%s: sema create fails", __func__);
			goto out;
		}
		xSemaphoreTake(sema, 0);
		xTaskCreate(libapp_conf_oem_key_crypt_task, "conf_oem",
		    LIBAPP_OEM_ENCR_STACK, sema, 20, NULL);
		xSemaphoreTake(sema, portMAX_DELAY);
		vSemaphoreDelete(sema);
	}

	/*
	 * Copy DSN last in case of premature power cycle.
	 */
	libapp_conf_id_copy(from, to, LIBAPP_DSN_CONF);	/* must be last */
out:
	nvs_close(libapp_nvs_id);
}

void libapp_conf_check(void)
{
	char pub_key[CLIENT_CONF_PUB_KEY_LEN];
	int pub_key_len;
	const char *item_string;

	if (strlen(conf_sys_dev_id) <= 0) {
		item_string = "DSN";
		goto not_valid;
	}

	pub_key_len = adap_conf_pub_key_get(pub_key,
	    sizeof(pub_key));
	memset(pub_key, 0, sizeof(pub_key));
	if (pub_key_len <= 0) {
		item_string = "public key";
		goto not_valid;
	}

	if (strlen(conf_sys_mfg_model) <= 0) {
		item_string = "mfg model";
		goto not_valid;
	}

	if (strlen(conf_sys_mfg_serial) <= 0) {
		item_string = "mfg serial";
		goto not_valid;
	}

	if (strlen(oem) <= 0) {
		item_string = "oem id";
		goto not_valid;
	}

	if (strlen(oem_model) <= 0) {
		item_string = "oem model";
		goto not_valid;
	}

	if (oem_key_len <= 0) {
		item_string = "oem key";
		goto not_valid;
	}

	log_put(LOG_INFO "module configuration valid");
	return;

not_valid:
	log_put(LOG_WARN "%s config check failed", item_string);
	log_put(LOG_WARN "module configuration invalid");
}

/*
 * Read certain config items for "conf id" and conf_init() into memory.
 */
void libapp_conf_id_read(void)
{
	struct ada_conf *cf = &ada_conf;
	static u8 sta_mac[6];
	u8 mac[6];
	static u8 done;

	if (done) {
		return;
	}
	done = 1;
	libapp_conf_id_init();

	adap_conf_get("sys/mfg_model",
	    conf_sys_mfg_model, sizeof(conf_sys_mfg_model));
	adap_conf_get("sys/mfg_serial",
	    conf_sys_mfg_serial, sizeof(conf_sys_mfg_serial));

	esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
	cf->mac_addr = sta_mac;

	/*
	 * Use the eflash mac address as hardware ID.
	 * This could be different than the one in the Wi-Fi layer.
	 */
	esp_efuse_mac_get_default(mac);
	snprintf(conf_hw_id, sizeof(conf_hw_id),
	    "mac-%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x",
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	cf->hw_id = conf_hw_id;
}

/*
 * Set platform ADA client configuration items.
 */
void libapp_conf_init(void)
{
	struct ada_conf *cf = &ada_conf;

	libapp_conf_token_check();
	libapp_conf_id_read();

	/*
	 * Load config for individual modules
	 */
	adw_conf_load();

	cf->enable = 1;
#ifndef LIBAPP_DIS_GET_ALL
	cf->get_all = 1;
#endif
	cf->poll_interval = 30;

	/*
	 * Allow the region to default (NULL) for most of the world.
	 * DNS based on the OEM-model and OEM-ID will take
	 * care of directing the device to the correct service.
	 * The only exception as of this writing is China, for which you
	 * should set cf->region = "CN".
	 */
}

#ifndef LIBAPP_DISABLE_ADAP_CONF_REG_CHANGED
/*
 * The user-registered flag changed.
 */
void adap_conf_reg_changed(void)
{
	struct ada_conf *cf = &ada_conf;

	log_put(LOG_INFO "%s: user %sregistered",
	    __func__, cf->reg_user ? "" : "un");
}
#endif /* LIBAPP_DISABLE_ADAP_CONF_REG_CHANGED */

/*
 * client_conf_sw_build() returns the string to be reported to the cloud as the
 * module image version.
 */
const char *adap_conf_sw_build(void)
{
#ifdef AYLA_PRODUCTION_AGENT
	return mod_sw_version;
#else
	return ada_version_build;
#endif
}

/*
 * client_conf_sw_version() returns the string reported to LAN clients with the
 * name, version, and more.
 */
const char *adap_conf_sw_version(void)
{
	return ada_version;
}

/*
 * Convert shortened name to full name string, including factory/startup prefix.
 */
static int libapp_conf_full_name(const char *short_name, char *buf, size_t len)
{
	char *out = buf;		/* next output byte */
	size_t olen = len;		/* remaining length for output */
	const char *in = short_name;	/* next input byte */
	size_t ilen;			/* remaining length for input */
	int tlen;
	char token[3];
	const char *sep;
	int in_literal;
	char conf_type = 'f';
	const char *full_token;

	ilen = strlen(in);

	/*
	 * If it starts with the prefix, it's a startup configuration item.
	 */
	if (in[0] == LIBAPP_CONF_STARTUP_PREF) {
		conf_type = 's';
		in++;
		ilen--;
	}
	tlen = snprintf(out, olen, "%c ", conf_type);
	if (tlen >= olen) {
		printcli("conf: \"%s\" - overrun at start", short_name);
		return -1;		/* name truncated */
	}
	out += tlen;
	olen -= tlen;

	/*
	 * If it does not start with an upper-case character, after the prefix,
	 * it's not shortened.
	 */
	if (!isupper(*in)) {
		tlen = snprintf(out, olen, "%s", in);
		if (tlen >= olen) {
			printcli("conf: \"%s\" - overrun at len %d",
			    short_name, tlen);
			return -1;
		}
		return 0;
	}

	sep = "";
	in_literal = 0;
	while (ilen > 0 && in[0] != '\0') {
		/*
		 * If input is not uppercase, it's a literal token or number.
		 */
		if (!isupper(in[0])) {
			if (in_literal) {
				tlen = snprintf(out, olen, "%c", in[0]);
			} else {
				in_literal = 1;
				tlen = snprintf(out, olen, "%s%c", sep, in[0]);
			}
			in++;
			ilen--;
		} else if (ilen >= 2) {
			/* copy 2 bytes of input to token */
			memcpy(token, in, 2);
			token[2] = '\0';
			in += 2;
			ilen -= 2;

			/* lookup token */
			full_token = libapp_conf_full_token(token);
			if (!full_token) {
				full_token = token;
			}

			/* add full name of token to output */
			tlen = snprintf(out, olen, "%s%s", sep, full_token);
			in_literal = 0;
		} else {
			printcli("conf: token \"%s\" - odd length", token);
			return -1;
		}
		if (tlen >= olen) {
			printcli("conf: \"%s\" - overrun at len %u",
			    short_name, len - olen);
			return -1;	/* name truncated */
		}
		out += tlen;
		olen -= tlen;
		sep = "/";
	}
	return 0;
}

/*
 * Show item with blob value.
 * Show as string if it appears to be one, otherwise show as hex.
 * Note that blobs are not NUL-terminated.
 */
static esp_err_t libapp_conf_show_blob(nvs_handle_t nvs, const char *short_name,
    const char *name)
{
	esp_err_t rc;
	char val[500];		/* TODO */
	char out[40];
	size_t len;
	size_t off = 0;
	int tlen;
	unsigned int i;
	int show_hex = 0;

	len = sizeof(val);
	rc = nvs_get_blob(nvs, short_name, val, &len);
	if (rc == ESP_ERR_NVS_NOT_FOUND) {
		return ESP_ERR_NVS_NOT_FOUND;
	}
	if (rc != ESP_OK) {
		printcli("conf: nvs_get_blob failed with err code %d",
		    rc);
		return rc;
	}

	if (libapp_conf_is_secret(name)) {
		printcli("  %s = (len %zu) (set)", name, len);
		return 0;
	}

	/*
	 * Show in hex if it doesn't look like good ASCII.
	 */
	for (i = 0; i < len; i++) {
		if (val[i] < 0x20 || val[i] >= 0x80) {
			show_hex = 1;
			break;
		}
	}
	if (show_hex) {
		out[0] = '\0';
		for (i = 0; i < len && off < sizeof(out) - 3; i++) {
			tlen = snprintf(out + off, sizeof(out) - off,
			    "%2.2x ", val[i]);
			if (tlen >= sizeof(out) - off) {
				break;
			}
			off += tlen;
		}
		printcli("  %s = (len %zu) %s", name, len, out);
	} else {
		tlen = len;
		if (tlen > sizeof(out) - 1) {
			tlen = sizeof(out) - 1;
		}
		memcpy(out, val, tlen);
		out[tlen] = '\0';
		if (len > tlen) {
			printcli("  %s = (len %zu) \"%s\"...", name, len, out);
		} else {
			printcli("  %s = \"%s\"", name, out);
		}
	}
	return 0;
}

static void libapp_conf_show_entry(nvs_handle_t nvs, const char *short_name,
    nvs_type_t item_type)
{
	char full_name[CONF_PATH_STR_MAX];
	const char *name;

	if (short_name[0] == LIBAPP_CONF_APP_F_PREF) {
		snprintf(full_name, sizeof(full_name),
		    "f " LIBAPP_CONF_APP "/%s", short_name + 1);
		name = full_name;
	} else if (short_name[0] == LIBAPP_CONF_APP_S_PREF) {
		snprintf(full_name, sizeof(full_name),
		    "s " LIBAPP_CONF_APP "/%s", short_name + 1);
		name = full_name;
	} else if (!libapp_conf_full_name(short_name, full_name,
	     sizeof(full_name))) {
		/* lookup succeeded */
		name = full_name;
	} else {
		printcli("conf: full name lookup failed short \"%s\"",
		    short_name);
		name = short_name;
	}

	switch (item_type) {
	case NVS_TYPE_BLOB:
		libapp_conf_show_blob(nvs, short_name, name);
		break;
	default:
		printcli(" %s type 0x%x", name, item_type);
		break;
	}
}

static void libapp_conf_show(void)
{
	nvs_handle_t nvs = libapp_nvs;
	nvs_iterator_t iter;
	nvs_entry_info_t info;
	unsigned int count = 0;

	libapp_conf_token_check();

	iter = nvs_entry_find("nvs", AYLA_STORAGE, NVS_TYPE_ANY);
	while (iter) {
		count++;
		nvs_entry_info(iter, &info);
		iter = nvs_entry_next(iter);
		libapp_conf_show_entry(nvs, info.key, info.type);
	}
	printcli("types: f = factory, s = startup");
	printcli("conf: %u entries found", count);
}

/*
 * Output field names and values consistently for manufacturing script parsing.
 */
static void libapp_conf_id_print(const char *name, const char *val)
{
	const char *tabs = "\t\t";

	if (strlen(name) > 6) {
		tabs = "\t";
	}
	printcli("%s:%s%s", name, tabs, val);
}

/*
 * For "conf id" get a string value from memory or, if that's empty, from NVS.
 */
static void libapp_conf_id_get(const char *label,
	const char *mem_val, const char *conf_name)
{
	char buf[200];
	int len;

	if (mem_val && mem_val[0]) {
		libapp_conf_id_print(label, mem_val);
	} else {
		len = adap_conf_get(conf_name, buf, sizeof(buf) - 1);
		if (len < 0) {
			len = 0;
		} else if (len > sizeof(buf) - 1) {
			len = sizeof(buf) - 1;
		}
		buf[len] = '\0';
		libapp_conf_id_print(label, buf);
	}
}

/*
 * Show information for manufacturing scripts.
 */
static void libapp_conf_id(void)
{
	u8 mac[6];
	char buf[200];
	esp_chip_info_t ci;
	char *cp;

	libapp_conf_id_read();

	/*
	 * For the host-app version, replace the first space, between the app
	 * name and version with a hyphen.
	 * This is for the module maker's production line requirements.
	 */
	snprintf(buf, sizeof(buf), "%s", mod_sw_version);
	cp = strchr(buf, ' ');
	if (cp) {
		*cp = '-';
	}
	libapp_conf_id_print("Host app", buf);
	libapp_conf_id_print("Agent", ada_version_build);
	libapp_conf_id_print("IDF", esp_get_idf_version());

	esp_chip_info(&ci);
	snprintf(buf, sizeof(buf), "model %u features 0x%x cores %u rev %u",
	    ci.model, ci.features, ci.cores, ci.revision);
	libapp_conf_id_print("Chip", buf);

	libapp_conf_id_print("HW ID", conf_hw_id);

	esp_efuse_mac_get_default(mac);
	libapp_conf_id_print("eFuse MAC", format_mac(mac, buf, sizeof(buf)));

	/*
	 * Print the Wi-Fi station MAC.
	 * Note, if there are only 2 MAC addresses programmed in,
	 * esp_read_mac() for the AP MAC would randomize it differently each
	 * time, so we don't print it.
	 */
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	libapp_conf_id_print("STA MAC", format_mac(mac, buf, sizeof(buf)));

	/*
	 * The BLE MAC will either be STA MAC + 1 or 2 depending on how
	 * many universal MAC addresses were assigned.
	 * Print this even if we're not using BLE, for manufacturing tracking.
	 */
	esp_read_mac(mac, ESP_MAC_BT);
	libapp_conf_id_print("BLE MAC", format_mac(mac, buf, sizeof(buf)));
	libapp_conf_id_print("model", conf_sys_model);
	libapp_conf_id_print("mfg_model", conf_sys_mfg_model);
	libapp_conf_id_print("mfg_serial", conf_sys_mfg_serial);
	libapp_conf_id_get("OEM ID", oem, LIBAPP_OEM_ID_CONF);
	libapp_conf_id_get("OEM model", oem_model, LIBAPP_OEM_MODEL_CONF);
	if (libapp_conf_get_string(LIBAPP_CONF_HASH, buf, sizeof(buf)) <= 0) {
		snprintf(buf, sizeof(buf), "none");
	}
	libapp_conf_id_print("Config hash", buf);
	libapp_conf_id_get("DSN", conf_sys_dev_id, LIBAPP_DSN_CONF);
}

/*
 * CLI: conf set <name> <value>
 * Set a config value.
 */
static void libapp_conf_set_cli(int argc, char **argv)
{
	char app_pref[] = LIBAPP_CONF_APP "/";
	const char *name;
	const char *val;
	int factory = 1;

	if (!mfg_or_setup_mode_ok()) {
		return;
	}
	if (!strcmp(*argv, "-s")) {
		argv++;
		argc--;
		factory = 0;
	}
	if (argc != 2) {
		printcli("usage: conf set [-s] <name> <val>");
		return;
	}
	name = argv[0];
	val = argv[1];

	/*
	 * Handle names starting with "app/".
	 */
	if (!strncmp(name, app_pref, sizeof(app_pref) - 1)) {
		if (libapp_conf_set(name + sizeof(app_pref) - 1,
		    val, factory)) {
			printcli("cannot set \"%s\"", name);
		}
		return;
	}

	/*
	 * Handle other config items in ADA space.
	 */
	if (libapp_ada_conf_set(libapp_nvs, name, val, strlen(val), factory)) {
		printcli("set of \"%s\" failed\n", name);
		return;
	}
}

const char libapp_conf_cli_help[] =
	"conf [show|id|"
#ifdef LIBAPP_CONF_LOAD
	"load <base64>|"
#endif
	"set [-s] <name> <val>]";

/*
 * 'conf' CLI
 * Show the configuration.
 */
esp_err_t libapp_conf_cli(int argc, char **argv)
{
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "show"))) {
		libapp_conf_show();
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "id")) {
		libapp_conf_id();
		return 0;
	}
#ifdef LIBAPP_CONF_LOAD
	if (argc == 3 && !strcmp(argv[1], "load")) {
		libapp_conf_load_cli(argv[2]);
		return 0;
	}
#endif
	if (argc >= 4 && !strcmp(argv[1], "set")) {
		libapp_conf_set_cli(argc - 2, argv + 2);
		return 0;
	}
	printcli("usage: %s", libapp_conf_cli_help);
	return 0;
}
