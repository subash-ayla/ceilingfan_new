#! /usr/bin/env python3
# coding: utf-8
#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#
'''
Class prod_tool:  Uses esptool for install operations
'''

__version__ = '1.2 2022-01-14'

import os
import sys
import logging
import argparse

_esptool_path = os.path.join('prod', 'esp-idf', 'components',
	'esptool_py', 'esptool')
sys.path.insert(1, _esptool_path)
import esptool

log = logging

_efuse_crypt_cnt_word = 0
_efuse_crypt_cnt_bit = 20
_efuse_crypt_cnt_mask = 0x7f

def _set_chip_info(chip):
	global _efuse_crypt_cnt_word
	global _efuse_crypt_cnt_bit
	global _efuse_crypt_cnt_mask

	if chip == 'esp32':
		_efuse_crypt_cnt_word = 0
		_efuse_crypt_cnt_bit = 20
		_efuse_crypt_cnt_mask = 0x7f	# 7 bits
	elif chip == 'esp32c3':
		#
		# SPI_BOOT_CRYPT_CNT starts at bit 82 (word 2 bit 18) in eFuses,
		# but it is read via EFUSE_RD_REPEAT_DATA1_REG bit 18.
		#
		_efuse_crypt_cnt_word = 1
		_efuse_crypt_cnt_bit = 18
		_efuse_crypt_cnt_mask = 0x7	# 3 bits
	else:
		log.error(f'unsupported chip {chip:s}')
		sys.exit(1)

#
# Parse command-line arguments
#
def _parse_args():
	parser = argparse.ArgumentParser(description =
		'Ayla esptool wrapper' + __version__)
	parser.add_argument('--port', required=True)
	parser.add_argument('--chip', required=False, default='esp32')
	parser.add_argument('-d', '--debug', required=False,
		default=False, action='store_true',
		help='enable debug logging')
	subparsers = parser.add_subparsers(help='sub-command help', dest='cmd')
	subparsers.add_parser('verify_encrypt_off',
		help='exit with error if encryption is set')
	args = parser.parse_args()
	return args

def _init_log(args):
	name = os.path.basename(sys.argv[0])
	if name.endswith('.py'):
		name = name[:-3]

	#
	# Init logging
	#
	level = logging.INFO
	if args.debug:
		level = logging.DEBUG
	log.basicConfig(level=level,
	    format=name + ': %(levelname)s: %(message)s')

#
# Subcommand 'verify_encrypt_off'
# Verify that encryption is off, and exit on error otherwise.
#
def _verify_encrypt_off(esp):
	ef = esp.read_efuse(_efuse_crypt_cnt_word)
	flash_crypt_cnt = (ef >> _efuse_crypt_cnt_bit) & _efuse_crypt_cnt_mask

	#
	# Count the number of 1's set in flash_crypt_cnt.
	# If it is odd, encryption is enabled.
	#
	ones = bin(flash_crypt_cnt).count('1')
	if ones & 1:
		log.error('flash encryption is enabled')
		sys.exit(1)

def _main():
	args = _parse_args()
	_init_log(args)
	log.info('version ' + __version__)

	_set_chip_info(args.chip)

	#
	# connect to the module
	loader = esptool._chip_to_rom_loader(args.chip)
	esp = loader(args.port)
	esp.connect()

	#
	# run sub-command
	#
	if args.cmd == 'verify_encrypt_off':
		_verify_encrypt_off(esp)

if __name__ == '__main__':
	_main()
