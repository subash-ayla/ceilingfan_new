/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBAPP_H__
#define __AYLA_LIBAPP_H__

/*
 * Initialize libapp.
 * This is optional if libapp_start() is used, but can be used to initialize
 * libapp_conf_*() APIs and others before calling libapp_start().
 */
void libapp_init(void);

/*
 * Start app library common app features.
 */
void libapp_start(void (*init)(void), void (*first_connect)(void));

/*
 * Connected to cloud.
 */
void libapp_cloud_up(void);

/**
 * Get application config item from NVS.
 *
 * \param name short nvs name in "app/" space.
 * \param buf buffer to receive response.
 * \param buf_len length of buffer.
 * \returns length of buffer used or -1 on error.
 */
int libapp_conf_get(const char *name, void *buf, size_t buf_len);

/**
 * Get application config string item from NVS.
 *
 * Like libapp_conf_get(), but guarantee's string NUL termination.
 * Returns -1 if the string value with termination doesn't fit.
 *
 * \param name short nvs name in "app/" space.
 * \param buf buffer to receive response.
 * \param buf_len length of buffer.
 * \returns length of value in buffer or -1 on error.
 */
int libapp_conf_get_string(const char *name, char *buf, size_t buf_len);

/**
 * Get application config long integer item from NVS.
 *
 * Like libapp_conf_get_string(), but interprets the string as an long integer.
 * Returns -1 if the string is over 19 digits or not parsable as a long integer.
 *
 * \param name short nvs name in "app/" space.
 * \param result pointer to resulting long integer.
 * \returns 0 on success or -1 on error.
 */
int libapp_conf_long_get(const char *name, long *result);

/**
 * Set application config blob in NVS.
 *
 * \param name short nvs name in "app/" space.
 * \param val binary item to be written.
 * \param len length of value to be written.
 * \param factory startup config item if 0, otherwise factory config item.
 * \returns length of buffer used or -1 on error.
 */
int libapp_conf_set_blob(const char *name, const void *buf, size_t len,
    u8 factory);

/**
 * Set application config string in NVS.
 *
 * \param name short nvs name in "app/" space.
 * \param val string to be written.
 * \param factory startup config item if 0, otherwise factory config item.
 * \returns length of buffer used or -1 on error.
 */
int libapp_conf_set(const char *name, const char *val, u8 factory);

/**
 *  Get application state from NVS (not secure).
 *
 * \param name short nvs name in "app/" space.
 * \param buf buffer to receive response.
 * \param buf_len length of buffer.
 * \returns length of buffer used or -1 on error.
 */
int libapp_conf_state_get(const char *name, void *buf, size_t buf_len);

/**
 * Set application state blob in NVS (not secure).
 *
 * \param name short nvs name in "app/" space.
 * \param val binary item to be written.
 * \param len length of value to be written.
 * \returns length of buffer used or -1 on error.
 */
int libapp_conf_state_set(const char *name, const void *val, size_t len);

/**
 * Open partition for unencrypted state saving.
 * Used only when speed is of the essence and security is not a concern.
 * Required before use of libapp_conf_state_get() and libapp_conf_state_set().
 */
void libapp_nvs_state_init(void);

/*
 * Initialize to handle schedule properties.
 *
 * This handles N schedules, with names formed by using the format with
 * unsigned int 1 through count.
 *
 * Loads schedule values from the configuration
 */
enum ada_err libapp_sched_init(const char *format, u16 count);

#ifdef AYLA_BLUETOOTH_SUPPORT
/*
 * Bluetooth demo initialization
 */
void libapp_bt_init(void);

/*
 * Set callback to be called when passkey needs to be displayed
 */
void libapp_bt_passkey_callback_set(void (*callback)(u32 passkey));

/*
 * Set factory test mode for BLE which disables Wi-Fi setup over BLE.
 */
void libapp_bt_ftm_enable(void);
#endif

/*
 * Function called by other library components when device should identify
 * itself to the end user.
 */
void libapp_identify(void);

/*
 * Set callback to be called when device should identify itself to the end
 * user, for example, briefly blinking an LED.
 */
void libapp_identify_callback_set(void (*callback)(void));

/*
 * Set callback to be called during a factory reset.
 * To be used only by factory test mode code, for now.
 * Only one callback allowed.
 */
void libapp_conf_factory_reset_callback_set(void (*handler)(void));

/*
 * Perform a factory reset.
 */
void libapp_conf_factory_reset(void);

/*
 * Log information about configuration validity.
 */
void libapp_conf_check(void);

#endif /* __AYLA_LIBAPP_H__ */
