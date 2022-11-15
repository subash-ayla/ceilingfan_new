/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_PHA_SECURE_H__
#define __AYLA_PHA_SECURE_H__

/*
 * Show security eFuses via printcli.
 */
void pha_secure_efuses_show(void);

/*
 * Set security settings for CLI.
 * Error message is written to buffer on error.
 * Return value is 0 on success, -1 on error, +1 if fuses are already set.
 */
int pha_secure_lock(char *msg, size_t len);

/*
 * Register command for showing and setting security.
 */
void pha_secure_cli_register(void);

#endif /* __AYLA_PHA_SECURE_H__ */
