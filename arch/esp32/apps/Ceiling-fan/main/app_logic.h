/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */

#ifndef APP_LOGIC_H
#define APP_LOGIC_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <libapp/net_event.h>
#include "app_int.h"
#include "ada_lock.h"

void app_logic_init(void);
void app_logic_prop_init(void);
void app_logic_idle(void);

#endif		/* APP_LOGIC_H */
