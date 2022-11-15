#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#

#
# Configuration for CDTech CDA01 module should work for all ESP32-C3 modules
#
CHIP := esp32c3
FLASH_FREQ := 80m
FLASH_MODE := qio
FLASH_SIZE := 4M

SDKCONFIG := conf/sdkconfig-esp32-c3-4M
