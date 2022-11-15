/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_LIBPHA_SECURE_INT_H__
#define __AYLA_LIBPHA_SECURE_INT_H__

/*
 * Set flash encryption in Release mode by setting eFuses that prevent
 * flash downloading and re-enabling.
 * Returns -1 on error.
 * Returns 1 if already locked or locking disabled.
 * Returns 0 on success.
 */
int pha_ftm_secure_lock(void);

#endif /* __AYLA_LIBPHA_SECURE_INT_H__ */
