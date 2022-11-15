#! /bin/bash

#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#

# Show file and line for a backtrace printed by esp32 crash dump
# Runs addr2line for each address on the command line.

# Run like:
#
# addr build/starter_app.elf Backtrace: 0x40134206:0x3ffd3f80 0x40133d55:0x3ffd4030
#
# The Backtrace: token is ignored but handier to cut-and-paste the whole line.


ADDR2LINE=~/.espressif/tools/xtensa-esp32-elf/esp-2019r2-8.2.0/xtensa-esp32-elf/bin/xtensa-esp32-elf-addr2line
ADDR2LINE=xtensa-esp32-elf-addr2line

if [ $# -lt 1 ]
then
	echo "usage: $0 [<elf>] <addr>" >&2
	exit 1
fi

elf=$1

#
# If the ELF file argument is not valid, assume the last component of the
# directory is the app-name and try build/app-name.elf as the file.
#
if [ $# -lt 2 -o \( ! -f $elf -a ${elf##*.} != "elf" \) ]
then
	elf=build/${PWD##*/}.elf
else
	shift
fi

if [ ! -f $elf ]
then
	echo "ELF file \"$elf\" not found" >&2
	exit 1
fi

for addr
do
	if [ $addr == "Backtrace:" ]
	then
		continue
	fi
	# remove suffix including :
	addr=${addr%%:*}
	echo addr $addr
	$ADDR2LINE -f -e $elf $addr
done
