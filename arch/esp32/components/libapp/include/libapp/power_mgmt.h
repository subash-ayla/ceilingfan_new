/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#ifndef __AYLA_POWER_MGMT_H__
#define __AYLA_POWER_MGMT_H__

/*
 * Initialize power management.
 */
void libapp_power_mgmt_init(void);

/*
 * Get lock to stay at high-performance power level.
 *
 * Applications can use this but in general it might be better if they allocated
 * their own locks uisng esp_pm_lock_create().
 *
 * Note: this is not thread-safe.
 */
void libapp_power_mgmt_acquire(void);

/*
 * Release lock, allowing lowering performance and power.
 */
void libapp_power_mgmt_release(void);

#endif /* __AYLA_POWER_MGMT_H__ */
