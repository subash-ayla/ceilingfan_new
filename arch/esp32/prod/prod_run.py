#! /usr/bin/env python3
# coding: utf-8
#
# Copyright 2020-2021 Ayla Networks, Inc.  All rights reserved.
#
'''
Production package main entry

Does the following steps:
	Checks whether new DSN files are present
	- Takes DSN files and config text file as input.
	- Makes csv files for each DSN using prod_csv.py
	- Uses mass_mfg/mass_gen.py from ESP-IDF to make NVS images and keys

	- Checks whether any NVS images and keys are available
	- The group size is specified by the number of serial ports.
	- For each group, launch a task to download images and wait
		- accumulate output in separate files.
	- after all have completed - give status
		- show output for any that failed
		- remove NVS files for the ones that worked
	- wait for user to start next batch - could be rerun of the command
'''

#
# Please update version before making any changes
#
__version__ = '1.3.1 2022-05-19'

import os
import sys
import io
import argparse
import subprocess
import logging
import shutil
from datetime import datetime,timezone
#
# Ayla python modules
#
from pyayla import prod_csv
from pyayla import prod_batch
from pyayla import oem_config

log = logging

nvs_prefix = 'p'
key_prefix = 'keys'
nvs_size = '0x60000'		# size of NVS partition

prod_tmp_dir = 'tmp'		# directory for intermediate files
prod_conf_dir = 'conf'		# directory for configuration files
prod_esp_idf = 'ext'		# location of ESP-IDF scripts

prod_mfg_gen_dir = prod_tmp_dir # directory where mfg_gen will run

# File containing serial port paths.  can override with --ports arg
prod_com_ports_file = os.path.join(prod_conf_dir, 'com_ports.txt')

# File where CSVs for DSNs is put by prod_csv for mfg_gen
prod_mfg_gen_master_in = 'master.csv'
prod_master_csv_file = os.path.join(prod_tmp_dir, 'master.csv')

# Config File describing CSV values to write to NVS
prod_mfg_gen_conf_in = 'config.csv'
prod_conf_in = os.path.join(prod_tmp_dir, 'config.csv')

#
# Init the paths for various pieces of the production package.
#
def prod_paths_init(pkg):
	global prod_mfg_gen

	# Path to mfg_gen.py script from tmp dir
	prod_mfg_gen = os.path.join('..', pkg, prod_esp_idf, 'mfg_gen.py')
	idf_path = os.path.abspath(os.path.join(pkg, 'esp-idf'))
	os.environ['IDF_PATH'] = idf_path
	esptool = os.path.join(idf_path, 'components',
		'esptool_py', 'esptool', 'esptool.py')
	os.environ['ESPTOOL'] = esptool

# These paths are fixed by $IDF_PATH/tools/mass_mfg/mfg_gen.py in its CWD
prod_def_keys = os.path.join(prod_mfg_gen_dir, 'keys')
prod_def_nvs = os.path.join(prod_mfg_gen_dir, 'bin')
prod_def_csv = os.path.join(prod_mfg_gen_dir, 'csv')

prod_dsns_warn = 1000		# warn if less than this many DSNs remain

prod_conf = None

def mkdir_try(path):
	try:
		os.mkdir(path)
	except FileExistsError:
		pass

#
# Make temporary directories we'll need
#
def prod_run_mkdirs():
	mkdir_try(prod_tmp_dir)
	mkdir_try(prod_def_nvs)
	mkdir_try(prod_def_keys)

#
# Remove temporary NVS files
#
def prod_run_nvs_clean():
	try:
		shutil.rmtree(prod_tmp_dir)
	except FileNotFoundError:
		pass
	prod_run_mkdirs()

#
# Pause between batches
#
def flash_pause(group_size):
	print('')
	if group_size == 1:
		print('Connect next device to be initialized')
	else:
		print('Please connect the next set of',
		    'devices to be initialized.')
	while True:
		resp = input('Enter "c" to continue, "q" to quit: ')
		if resp.lower() == 'c':
			print('')
			return True
		if resp.lower() == 'q' or resp.lower() == 'quit':
			print('')
			return False
	return False

#
# Remove uninteresing lines from the output of mfg_gen.py
#
def nvs_gen_out_edit(lines):
	#
	# patterns to delete from output if they match the start of lines
	#
	skip_list = [
		'Created CSV file: ',
		'Creating NVS binary with version: V2 ',
		'Created NVS binary: ===> ',
		'Files generated in ',
		'Created encryption keys: ',
		'Generating encrypted NVS binary images...',
	]
	out = ''
	for line in lines.splitlines():
		line = str(line)
		if len(line) == 0:
			continue
		skip = False
		for match in skip_list:
			if line.startswith(match):
				skip = True
				break
		if not skip:
			out = out + line + '\n'
	return out

#
# Run mfg_gen to make NVS images and keys
# Trying to be equivalent to
# ..\<path-to-esp-idf>\tools\mass_mfg\mfg_gen.py generate --keygen \
#		conf\config.csv tmp\master.csv p 0x60000
# This is run in directory prod_mfg_gen_dir (tmp) so generated files go there
#
def nvs_gen_run():
	argv = ['python', prod_mfg_gen, 'generate']
	if prod_def_keys:
		argv = argv + ['--keygen']
	argv = argv + [prod_mfg_gen_conf_in,
	    prod_mfg_gen_master_in, nvs_prefix, nvs_size]

	try:
		p = subprocess.Popen(argv,
		    cwd=prod_mfg_gen_dir,
		    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
	except FileNotFoundError as e:
		log.error(f'nvs_gen_run: file "{str(e.filename):s}" not found')
		log.debug(f'args {str(argv):s}')
		raise

	stdout, stderr = p.communicate()

	#
	# Note mfg_gen.py exits status 0 even on failure!
	#
	status = p.poll()
	if status:
		log.error(f'script {prod_mfg_gen:s} exited status {status:d}')

	# Note stderr should be empty since it was redirected to stdout
	if stderr != None and len(stderr) != 0:
		log.error('errors occurred')
		log.info(f'cmd {str(argv):s}')
		for line in stderr.decode(encoding='utf-8').splitlines():
			log.info('output: ' + line)

	if stdout != None and len(stdout) != 0:
		#
		# Shorten the output by removing uninteresting lines
		#
		short_out = nvs_gen_out_edit(stdout.decode(encoding='utf-8'))
		if len(short_out):
			log.error('unexpected output:')
			log.info('-' * 8)
			log.info(f'cmd {str(argv):s}')
			log.info(f'edited output from {prod_mfg_gen:s}:')
			for line in short_out.splitlines():
				log.info('edited output: ' + line)
			log.info('-' * 8)
			log.info(f'stdout from {prod_mfg_gen:s}:')
			stdout = stdout.decode(encoding='utf-8')
			for line in stdout.splitlines():
				log.info('full output: ' + line)
			log.info('-' * 8)

#
# Get more NVS partitions
#
def prod_run_setup(pcsv):
	count = 0
	try:
		pcsv.gen_config_csv(prod_conf_in, config = prod_conf['config'])
		count = pcsv.gen(limit=100)
	except RuntimeError as e:
		log.error('run time error from prod_csv: %s', str(e))
		sys.exit(1)
	if count:
		nvs_gen_run()
		log.debug(f'Generated {count:d} NVS partitions')
	return count

#
# Get complete list of NVS prefixes to flash.
#
def prod_run_devs_get():
	devs = []
	with os.scandir(prod_def_nvs) as it:
		for entry in it:
			path = entry.name
			if path.startswith(nvs_prefix + '-'):
				devs.append(path)

	#
	# sort entries by their number
	# This should match the order of DSNs used
	#
	prefix_len = len(nvs_prefix) + 1
	suffix_len = len('.bin')
	devs.sort(key=lambda path: int(path[prefix_len : -suffix_len]))
	return devs

def log_dsns_left(count, path):
	if count == 0:
		log.error(f'No more DSN device credentials.')
	elif count < prod_dsns_warn:
		log.warning(f'Only {count:d} DSNs remain.')
	else:
		log.info(f'DSN device credentials remaining: {count:d}.')
		return
	log.info(f'Reserve more DSN device credentials and add them ' + \
	    f'to directory {path:s}.')
	log.info(f'Do not reuse DSNs.')

#
# Main function
#
def prod_run(args):
	#
	# Create object used to generate NVS partitions as needed.
	#
	pcsv = prod_csv.prod_csv(args.model, xml_dir = args.dsns,
	    out_file = prod_master_csv_file,
	    omit_dsn = args.reflash,
	    start_sn = args.serial)

	if not args.reflash:
		dsns_left = pcsv.dsn_count()
		log_dsns_left(dsns_left, args.dsns)
		dsns_warned = (dsns_left < prod_dsns_warn)
		if dsns_left == 0:
			return

	#
	# Flash devices in groups
	#
	count = 0
	good_count = 0
	group_size = len(args.ports)
	devs = []
	group_start = 0
	while True:
		if len(devs) < group_size:
			prod_run_nvs_clean()
			prod_run_setup(pcsv)
			devs = prod_run_devs_get()

			if not args.reflash and len(devs) == 0:
				log_dsns_left(0, args.dsns)
				break

			if len(devs) < group_size:
				log.error(f'Too few device credentials. ' + \
				    f'{len(devs):d} left.')
				log_dsns_left(len(dev), args.dsns)
				break

		#
		# Pause here for a new set of devices to be placed in fixture.
		#
		if not flash_pause(group_size):
			break

		group = devs[:group_size]
		dsns = pcsv.dsn_reserve(count = group_size)

		#
		# Flash a group of devices.
		#
		status = prod_batch.prod_batch(args, group, dsns,
		    prod_def_nvs, prod_def_keys, prod_conf, log=log,
		    encrypt = (args.reflash or args.reflash_dsn))

		#
		# summarize status and remove used DSN files
		#
		for i in range(len(status)):
			if status[i] == 0:
				pcsv.dsn_used(i)
				good_count = good_count + 1
			count = count + 1

		#
		# Delete the DSN entries and device entries used
		#
		pcsv.dsn_delete(count = group_size)
		devs = devs[group_size:]

		if not args.reflash:
			dsns_left = dsns_left - group_size
			if not dsns_warned and dsns_left < prod_dsns_warn:
				log_dsns_left(dsns_left, args.dsns)
				dsns_warned = True

	if good_count == count:
		log.info(f'success: {count:d} devices')
	else:
		errs = count - good_count
		log.warning(f'errors: {errs:d}, good {good_count:d}')

#
# Get list of com ports from file
#
def get_ports(path):
	ports = []
	i = 0
	with open(path, 'r', encoding='utf-8') as fp:
		line_no = 0
		for line in fp:
			line_no = line_no + 1
			line = line.strip()
			# TODO test access to port
			ports.append(line)
			log.info(f'device {i:d} port {line:s}')
			i = i + 1
	return ports

#
# Read config items from text file
# The file may be overridden by supplying an empty file or 'none'.
#
def read_config(path):
	global prod_conf

	try:
		prod_conf = oem_config.parse_config(path)
	except FileNotFoundError:
		log.error(f'file {path:s} not found')
		sys.exit(1)
	except prod_csv.CONF_NAME_NOT_MAPPED:
		sys.exit(1)
	except prod_csv.CONF_NAME_TOO_LONG:
		sys.exit(1)

#
# Parse command-line arguments
#
def parse_args():
	parser = argparse.ArgumentParser(description =
		'Ayla Module Batch Flasher ' + __version__)
	parser.add_argument('--app', required=True,
		help='application package directory')
	parser.add_argument('--conf', required=False,
		default=None,
		help='alternative application or OEM config input file')
	parser.add_argument('--csv_file', required=False,
		default='master.csv',
		help='output file for comma-separated-values for NVS')
	parser.add_argument('-d', '--debug', required=False,
		default=False, action='store_true',
		help='enable debug logging')
	parser.add_argument('--dsns', required=False,
		default='dsns',
		help='directory containing xml files for each device')
	parser.add_argument('--images', required=False,
		default=None, help='directory containing images')
	parser.add_argument('--model', required=True,
		help='manufacturer\'s module part name')
	parser.add_argument('--no-encrypt-nvs', required=False,
		default=False, action='store_true',
		help='disable encryption of NVS')
	parser.add_argument('--no_app', required=False,
		default=False, action='store_true',
		help='disable download of application')
	parser.add_argument('--no_boot', required=False,
		default=False, action='store_true',
		help='disable download of bootloader')
	parser.add_argument('--no_erase', required=False,
		default=False, action='store_true',
		help='disable erase of entire flash')
	parser.add_argument('--no_nvs', required=False,
		default=False, action='store_true',
		help='disable download of NVS')
	parser.add_argument('--no_stub', '--no-stub', required=False,
		default=False, action='store_true',
		help='pass --no-stub to esptool')
	parser.add_argument('--port', required=False,
		default='@' + prod_com_ports_file,
		help='COM ports or @ followed by file name ' + \
		   'containing COM port list ' + \
		   '- defaults to @' + prod_com_ports_file)
	parser.add_argument('--prod', required=False,
		default=None,
		help='production package directory')
	parser.add_argument('--reflash', required=False,
		default=False, action='store_true',
		help='reflash module with encryption enabled - maintains DSN')
	parser.add_argument('--reflash_dsn', required=False,
		default=False, action='store_true',
		help='reflash module with encryption enabled - writes DSN')
	parser.add_argument('--serial', required=False,
		default=None,
		help='batch serial number or date code')
	parser.add_argument('--trace', required=False,
		default=False, action='store_true',
		help='enable esptool tracing')
	args = parser.parse_args()

	name = os.path.basename(sys.argv[0])
	if name.endswith('.py'):
		name = name[:-3]
	level = logging.INFO
	if args.debug:
		level = logging.DEBUG
	log.basicConfig(level=level,
	    format=name + ': %(levelname)s: %(message)s')
	log.info('version ' + __version__)

	# Handle case where COM ports are specified in a file
	args.ports = [ args.port ]
	if args.port[0] == '@':
		args.ports = get_ports(args.port[1:])

	# Clear encryption keys directory path if it should not be used
	if args.no_encrypt_nvs:
		global prod_def_keys
		prod_def_keys = None

	if not args.conf:
		args.conf = os.path.join(args.app, 'prod_config.txt')
	if not args.images:
		args.images = os.path.join(args.app, 'images')

	#
	# From the command name path, determine the path to the
	# production scripts package.
	#
	if not args.prod:
		args.prod = os.path.dirname(sys.argv[0])
	prod_paths_init(args.prod)

	return args

def _main():
	args = parse_args()
	if args.conf.lower() != 'none':
		prod_conf = read_config(args.conf)
	else:
		prod_conf = dict([['config',[]],['verify_id',[]],
		    ['prod',dict()]])

	try:
		prod_run(args)
	except FileNotFoundError as e:
		log.error(f'prod_run: file not found error: {str(e):s}')
		sys.exit(1)

if __name__ == '__main__':
	_main()

# end
