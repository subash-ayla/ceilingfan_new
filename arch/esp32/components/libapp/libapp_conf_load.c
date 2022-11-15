/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifdef LIBAPP_CONF_LOAD		/* define to include "conf load" subcommand */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <ayla/utypes.h>
#include <ayla/clock.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ayla/tlv.h>
#include <ayla/conf.h>
#include <jsmn.h>		/* using component/ayla version of JSMN */
#include <ayla/jsmn_get.h>

#include <ada/libada.h>
#include <net/base64.h>
#include <net/net_crypto.h>
#include <libapp/libapp.h>
#include <nvs.h>
#include "libapp_conf_int.h"

/*
 * Functions to handle 'conf load xxx' command, which loads OEM config items
 * from the oem_setup.py script.
 */
#define LIBAPP_CONF_TOK		200	/* max JSON tokens allowed in input */
#define LIBAPP_CONF_OUTER_OBJ	"config"

/*
 * Common format for conf load messages.
 * These messages are expected and parsed by the oem_config.py script.
 * This is for a CLI command so it doesn't use the libayla log system.
 */
#define CONF_LOAD_LOG(_level, _format, ...) \
	printcli("conf load: " _level  ": " _format, ##__VA_ARGS__)
#define CONF_LOAD_ERR(_fmt, ...) CONF_LOAD_LOG("error", _fmt, ##__VA_ARGS__)
#define CONF_LOAD_WARN(_fmt, ...) CONF_LOAD_LOG("warning", _fmt, ##__VA_ARGS__)
#define CONF_LOAD_INFO(_fmt, ...) CONF_LOAD_LOG("info", _fmt, ##__VA_ARGS__)
#define CONF_LOAD_DEBUG(_fmt, ...) CONF_LOAD_LOG("debug", _fmt, ##__VA_ARGS__)

/*
 * Error codes reported to script.
 * Do not change these numbers, so they will match any documentation or
 * customer-reported issues.
 */
enum libapp_conf_error_code {
	CE_ALLOC = 1,
	CE_DECODE = 2,
	CE_DECODE_LEN = 3,
	CE_PARSE_VER = 4,
	CE_VERSION = 5,
	CE_PARSE_LEN = 6,
	CE_BODY_LEN = 7,
	CE_HASH = 8,
	CE_JSON_TOK = 9,
	CE_JSON_PARSE = 10,
	CE_JSON_PART = 11,
	CE_JSON_OUTER1 = 12,
	CE_JSON_OUTER2 = 13,
	CE_JSON_OUTER3 = 14,
	CE_JSON_ARRAY1 = 15,
	CE_JSON_ARRAY2 = 16,
	CE_JSON_ARRAY3 = 17,
	CE_JSON_OBJ1 = 18,
	CE_JSON_OBJ2 = 19,
	CE_JSON_OBJ3 = 20,
	CE_JSON_OBJ4 = 21,
	CE_JSON_OBJ5 = 22,
	CE_CONF_PERM = 23,
	CE_CONF_SETUP_REQ = 24,
	CE_CONF_OEM_ID = 25,
	CE_CONF_OEM_MODEL = 26,
	CE_CONF_OEM_KEY = 27,
	CE_CONF_OEM_ORDER = 28,
	CE_CONF_OEM_LEN = 29,
	CE_CONF_SET = 30,
	CE_CONF_SETUP = 31,
	CE_CONF_ERASE_STARTUP = 32,
};

struct conf_load_state {
	u8 dryrun:1;		/* if non-zero, don't apply changes */
	u8 key_match:1;		/* first setup key entered matched */
	u8 setup_mode:1;	/* setup mode state to apply afterwards */
	u8 oem_id_set:1;	/* non-zero if OEM ID set */
	u8 oem_model_set:1;	/* non-zero if OEM model is set */
	u8 oem_key_set:1;	/* non-zero if OEM key/secret is set */
	struct ada_conf_ctx *ctx; /* context for changing ADA config values */
	u16 index;		/* index into JSON array */
};
static struct conf_load_state libapp_conf_load_state;

/*
 * OEM-setup allowed configuration paths.
 * Asterisk at end indicates sub-tokens are allowed.
 */
static const char *libapp_conf_allowed[] = {
	"app/*",
	"bt/hostname",
	"bt/key",
	"oem/*",
	"sys/setup_mode",
	"wifi/*"
};

/*
 * Return non-zero if the configuration path name is in the above allowed list.
 */
static int libapp_conf_is_allowed(const char *name)
{
	const char *wild;
	const char **allowed;
	size_t len;
	size_t tlen;

	len = strlen(name);
	for (allowed = libapp_conf_allowed;
	    allowed < libapp_conf_allowed + ARRAY_LEN(libapp_conf_allowed);
	    allowed++) {
		wild = strchr(*allowed, '*');
		if (wild) {
			tlen = wild - *allowed;
			if (len > tlen && !memcmp(name, *allowed, tlen)) {
				return 1;
			}
		} else if (!strcmp(name, *allowed)) {
			return 1;
		} else {
			continue;
		}
	}
	return 0;
}

/*
 * Print an error message in the format expected by the oem_config script.
 */
void libapp_conf_err(enum libapp_conf_error_code err, char *str)
{
	struct conf_load_state *state = &libapp_conf_load_state;

	CONF_LOAD_ERR("failed code %u index %u: %s", err, state->index, str);
}

/*
 * Copy token into character buffer and NUL-terminate it.
 */
static char *libapp_conf_json_str(jsmn_parser *parser, jsmntok_t *obj,
    char *buf, size_t len)
{
	size_t olen;

	olen = obj->end - obj->start;
	if (olen >= len) {
		return NULL;
	}
	memcpy(buf, parser->js + obj->start, olen);
	buf[olen] = '\0';
	return buf;
}

/*
 * Load one config item from the JSON object.
 * Some items, such as oem/key, app/setup_mode/key, and sys/setup_mode
 * need special handling.
 */
static int libapp_conf_load_item(const char *name, const char *val)
{
	struct conf_load_state *state = &libapp_conf_load_state;
	enum conf_error conf_err;
	char conf_app_pref[] = LIBAPP_CONF_APP "/";
	size_t len;
	int rc;

	state->index++;
	if (!libapp_conf_is_allowed(name)) {
		CONF_LOAD_ERR("'%s' not allowed", name);
		return CE_CONF_PERM;
	}
	len = strlen(val);
	if (!conf_setup_mode && !state->key_match) {
		if (!strcmp(name, LIBAPP_SETUP_KEY_CONF)) {
			rc = libapp_cli_setup_mode_key_check(val);
			switch (rc) {
			case 0:
				state->key_match = 1;
				break;
			case -1:
				CONF_LOAD_ERR("setup_enable not allowed");
				return CE_CONF_PERM;
			case -2:
			default:
				CONF_LOAD_ERR("wrong setup key");
				return CE_CONF_PERM;
			}
		} else {
			CONF_LOAD_ERR("not in setup mode");
			CONF_LOAD_ERR("cannot set \"%s\"", name);
			return CE_CONF_SETUP_REQ; /* not in setup mode */
		}
	}

	/*
	 * Passed permission checks.
	 * Handle OEM items.  ID and model must be set before key.
	 */
	if (!strcmp(name, LIBAPP_OEM_ID_CONF)) {
		state->oem_id_set = 1;
		if (!state->dryrun) {
			rc = snprintf(oem, sizeof(oem), val);
			if (rc >= sizeof(oem)) {
				return CE_CONF_OEM_LEN;
			}
		}
	} else if (!strcmp(name, LIBAPP_OEM_MODEL_CONF)) {
		state->oem_model_set = 1;
		if (!state->dryrun) {
			rc = snprintf(oem_model, sizeof(oem_model), val);
			if (rc >= sizeof(oem)) {
				return CE_CONF_OEM_LEN;
			}
		}
	} else if (!strcmp(name, LIBAPP_OEM_KEY_CONF)) {
		state->oem_key_set = 1;
		if (!state->oem_id_set || !state->oem_model_set) {
			return CE_CONF_OEM_ORDER;
		}
		if (!state->dryrun) {
			conf_err = oem_set_key(val, len, oem_model);
			if (conf_err) {
				CONF_LOAD_ERR("oem_set_key err %u",
				    conf_err);
				return CE_CONF_OEM_KEY;
			}
			return 0;
		}
	} else if (!strcmp(name, SETUP_MODE_CONF)) {
		if (val[1] || (val[0] != '0' && val[0] != '1')) {
			return CE_CONF_SETUP;
		}
		state->setup_mode = (val[0] == '1');
		return 0;	/* skip setting ADA config setup_mode */
	} else {
		/* not a special case item */
	}

	/*
	 * Store config items in NVS if not in dryrun state.
	 * The ADA config items are tested by ada_conf_set() in dryrun state.
	 */
	if (!strcmp(name, LIBAPP_SETUP_KEY_CONF)) {
		if (state->dryrun) {
			return 0;
		}
		if (libapp_cli_setup_mode_key_set(val)) {
			return CE_CONF_SET;
		}
	} else if (strlen(name) > sizeof(conf_app_pref) &&
	    !strncmp(name, conf_app_pref, sizeof(conf_app_pref) - 1)) {
		if (state->dryrun) {
			return 0;
		}
		if (libapp_conf_set_blob(name + sizeof(conf_app_pref) - 1,
		    val, len, 1)) {
			CONF_LOAD_ERR("cannot set \"%s\"", name);
			return CE_CONF_SET;
		}
	} else {
		if (state->dryrun) {
			if (ada_conf_set(state->ctx, name, val)) {
				CONF_LOAD_ERR("cannot set \"%s\"", name);
				return CE_CONF_SET;
			}
		} else if (adap_conf_set(name, val, len)) {
			CONF_LOAD_ERR("cannot set \"%s\"", name);
			return CE_CONF_SET;
		} else {
			return 0;
		}
	}
	return 0;
}

/*
 * Handle one name/value pair from JSON array iterator.
 */
static int libapp_conf_load_obj(jsmn_parser *parser, jsmntok_t *obj, void *arg)
{
	char name[CONF_VAL_MAX];
	char val[CONF_VAL_MAX];
	jsmntok_t *tok;
	char *sp;
	int rc;

	if (obj->type != JSMN_OBJECT) {
		return CE_JSON_OBJ1;
	}

	name[0] = '\0';
	val[0] = '\0';
	tok = obj;
	for (tok = obj + 1; tok < parser->tokens + parser->num_tokens &&
	    tok->start > obj->start && tok->end < obj->end; tok++) {
		switch (tok->type) {
		case JSMN_STRING:
			if (!name[0]) {
				sp = libapp_conf_json_str(parser, tok,
				    name, sizeof(name));
				if (!sp) {
					return CE_JSON_OBJ2;
				}
			} else if (!val[0]) {
				sp = libapp_conf_json_str(parser, tok,
				    val, sizeof(val));
				if (!sp) {
					return CE_JSON_OBJ3;
				}
				rc = libapp_conf_load_item(name, val);
				if (rc) {
					return rc;
				}
			} else {
				return CE_JSON_OBJ4;
			}
			break;
		default:
			return CE_JSON_OBJ5;
		}
	}
	return 0;
}

/*
 * Load JSON from CLI for OEM config.
 */
static void libapp_conf_load_json(char *in, size_t in_len)
{
	struct conf_load_state *state = &libapp_conf_load_state;
	jsmntok_t tok[LIBAPP_CONF_TOK];
	jsmntok_t *obj;
	jsmn_parser parser;
	char name[CONF_VAL_MAX];
	char *sp;
	int err;
	jsmnerr_t jsmn_err;
	u8 saved_setup_mode;

	/*
	 * Parse JSON into tokens.
	 */
	memset(tok, 0, sizeof(tok));
	jsmn_init_parser(&parser, in, tok, ARRAY_LEN(tok));
	jsmn_err = jsmn_parse(&parser);
	switch (jsmn_err) {
	case JSMN_SUCCESS:
		break;
	case JSMN_ERROR_NOMEM:
		libapp_conf_err(CE_JSON_TOK, "too many JSON tokens");
		return;
	case JSMN_ERROR_PART:
		libapp_conf_err(CE_JSON_PART, "JSON incomplete");
		return;
	default:
		CONF_LOAD_ERR("unexpected json rc %d", jsmn_err);
		/* fall through */
	case JSMN_ERROR_INVAL:
		libapp_conf_err(CE_JSON_PARSE, "JSON parse failed");
		return;
	}

	/*
	 * Look for outer "config" object.
	 */
	obj = tok;
	if (obj->type != JSMN_OBJECT) {
		libapp_conf_err(CE_JSON_OUTER1,
		    "outer 'config' object missing");
		return;
	}
	obj++;
	if (obj->type != JSMN_STRING) {
		libapp_conf_err(CE_JSON_OUTER2,
		    "outer 'config' tag missing");
		return;
	}
	sp = libapp_conf_json_str(&parser, obj, name, sizeof(name));
	if (!sp || strcmp(sp, LIBAPP_CONF_OUTER_OBJ)) {
		libapp_conf_err(CE_JSON_OUTER3,
		    "outer 'config' tag mismatch");
		return;
	}
	obj++;
	if (obj->type != JSMN_ARRAY) {
		libapp_conf_err(CE_JSON_ARRAY1, "config array not found");
		return;
	}

	/*
	 * Set up a context for dryrunning config changes in ADA.
	 * This must always succeed.
	 * Lock out changes by ADA while we do this.
	 */
	conf_lock();
	state->ctx = ada_conf_dryrun_new();
	ASSERT(state->ctx);

	/*
	 * Dryrun iteration through name/values
	 */
	err = jsmn_array_iterate(&parser, obj, libapp_conf_load_obj, NULL);
	ada_conf_abort(state->ctx);	/* free up context */
	state->ctx = NULL;
	if (err) {
		if (err < 0) {
			err = CE_JSON_ARRAY1;
		}
		libapp_conf_err(err, "array iter failed");
		goto unlock;
	}

	/*
	 * Erase all startup and app config items.
	 * Enter setup mode.
	 */
	state->dryrun = 0;
	if (libapp_conf_startup_erase(1)) {
		libapp_conf_err(CE_CONF_ERASE_STARTUP,
		    "failed to restore factory");
		goto unlock;
	}

	saved_setup_mode = conf_setup_mode;
	state->setup_mode = conf_setup_mode;
	conf_setup_mode = 1;

	/*
	 * Again iterate through name/values, this time changing config.
	 */
	state->index = 0;
	err = jsmn_array_iterate(&parser, obj, libapp_conf_load_obj, NULL);
	if (err) {
		if (err < 0) {
			err = CE_JSON_ARRAY1;
		}
		libapp_conf_err(err, "array iter failed after dryrun");
		conf_setup_mode = saved_setup_mode;
		goto unlock;
	}
	conf_unlock();

	/*
	 * Clear setup mode if specified in the JSON.
	 */
	conf_setup_mode = state->setup_mode;
	CONF_LOAD_INFO("Success");		/* expected by script */
	CONF_LOAD_INFO("Please reset to use the new configuration.");
	return;
unlock:
	conf_unlock();
}

/*
 * Load configuration items from a base64-encoded JSON input file.
 * The file format after base64-decode is JSON-formatted content with
 * a binary SHA-256 hash appended.
 */
void libapp_conf_load_cli(const char *in)
{
	struct conf_load_state *state = &libapp_conf_load_state;
	size_t len;
	size_t blen;
	char *buf;
	char *sha_ptr;
	char hash[SHA256_SIG_LEN];
	struct adc_sha256 ctx;

	memset(state, 0, sizeof(*state));
	state->dryrun = 1;

	/*
	 * Allocate buffer to hold base64 decode.
	 */
	len = strlen(in);
	blen = len + 1;
	buf = malloc(blen);
	if (!buf) {
		libapp_conf_err(CE_ALLOC, "malloc error");
		return;
	}

	/*
	 * Do base64 decode.
	 */
	if (ayla_base64_decode(in, len, buf, &blen)) {
		libapp_conf_err(CE_DECODE, "base64 decode error");
		goto free;
	}
	if (blen >= len) {
		libapp_conf_err(CE_DECODE_LEN, "base64 length error");
		goto free;
	}
	buf[blen] = '\0';

	if (blen <= sizeof(hash)) {
		libapp_conf_err(CE_BODY_LEN, "json length error");
		goto free;
	}
	blen -= sizeof(hash);
	sha_ptr = buf + blen;

	/*
	 * Compute hash using Ayla net_crypto layer on top of mbedtls.
	 */
	adc_sha256_init(&ctx);
	adc_sha256_update(&ctx, buf, blen, NULL, 0);
	adc_sha256_final(&ctx, hash);

	/*
	 * Compare hashes.
	 */
	if (memcmp(hash, sha_ptr, sizeof(hash))) {
		libapp_conf_err(CE_HASH, "SHA-256 hash mismatch");
		goto free;
	}

	/*
	 * NUL-terminate JSON, overwriting hash, then parse it.
	 */
	*sha_ptr = '\0';
	libapp_conf_load_json(buf, blen);
free:
	free(buf);
}
#endif /* LIBAPP_CONF_LOAD */
