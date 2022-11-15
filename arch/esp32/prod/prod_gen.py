#! /usr/bin/env python3
# coding: utf-8
#
# Copyright 2021 Ayla Networks, Inc.  All rights reserved.
#
'''
Production files generator

Does the following steps:
	Reads app configuration file
	Reads DSN archive
	Generates manufacturing archive containing NVS partitions
	and key partitions
'''

#
# Please update version before making any changes
#
__version__ = '0.1 2021-09-08'

import os
import sys
import io
import argparse
import subprocess
import logging
import shutil
import tempfile
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

#prod_tmp_dir = 'tmp'		# directory for intermediate files
prod_tmp_dir_obj = tempfile.TemporaryDirectory(dir='.')
prod_tmp_dir = prod_tmp_dir_obj.name

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

	idf_path = abspath(os.path.join(pkg, 'esp-idf'))
	os.environ['IDF_PATH'] = idf_path
	esptool = os.path.join(idf_path, 'components',
		'esptool_py', 'esptool', 'esptool.py')
	os.environ['ESPTOOL'] = esptool

# These paths are fixed by $IDF_PATH/tools/mass_mfg/mfg_gen.py in its CWD
prod_def_keys = os.path.join(prod_mfg_gen_dir, 'keys')
prod_def_nvs = os.path.join(prod_mfg_gen_dir, 'bin')
prod_def_csv = os.path.join(prod_mfg_gen_dir, 'csv')

prod_conf = None

def mkdir_try(path):
	try:
		os.mkdir(path)
	except FileExistsError:
		pass

#
# Make temporary directories we'll need
#
def prod_gen_mkdirs():
	mkdir_try(prod_tmp_dir)
	mkdir_try(prod_def_nvs)
	mkdir_try(prod_def_keys)

#
# Remove temporary NVS files
#
def prod_gen_nvs_clean():
	try:
		shutil.rmtree(prod_tmp_dir)
	except FileNotFoundError:
		pass
	prod_gen_mkdirs()

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
#               --fileid id/dev_id \
#		conf\config.csv tmp\master.csv p 0x60000
# This is run in directory prod_mfg_gen_dir (tmp) so generated files go there
#
def nvs_gen_run():
	argv = ['python', prod_mfg_gen, 'generate']
	if prod_def_keys:
		argv = argv + ['--keygen']
	argv = argv + ['--fileid', 'id/dev_id']
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
def prod_gen_setup(pcsv, limit=100):
	count = 0
	try:
		pcsv.gen_config_csv(prod_conf_in, config = prod_conf['config'])
		count = pcsv.gen(limit=limit)
	except RuntimeError as e:
		log.error('run time error from prod_csv: %s', str(e))
		sys.exit(1)
	if count:
		nvs_gen_run()
		log.debug(f'Generated {count:d} NVS partitions')
	return count

#
# Main function
#
def prod_gen(args):
	#
	# Create object used to generate NVS partitions as needed.
	#
	pcsv = prod_csv.prod_csv(args.model, xml_dir = args.dsns,
	    out_file = prod_master_csv_file,
	    omit_dsn = False,
	    start_sn = args.serial)

	count = pcsv.dsn_count()
	if count == 0:
		log.error(f'No DSNs')
		return

	#
	# Remove old temporary directory
	# TODO make this directory random and ephemeral
	#
	prod_gen_nvs_clean()

	#
	# Generate NVS flash files
	#
	prod_gen_setup(pcsv, limit = count)

	#
	# Move files to a set of nvs.bin and nvs_key.bin files per
	# DSN directory.
	#
	out_dir = args.out
	out_zip = False
	if out_dir.endswith('.zip'):
		out_dir = out_dir[:-4]
		out_zip = True
	try:
		shutil.rmtree(out_dir)
	except FileNotFoundError:
		pass
	mkdir_try(out_dir)

	for dsn in pcsv.dsns_get():
		new_dir = os.path.join(out_dir, dsn)
		os.mkdir(new_dir)
		old_nvs = os.path.join(prod_def_nvs, 'p-' + dsn + '.bin')
		old_key = os.path.join(prod_def_keys, 'keys-p-' + dsn + '.bin')
		new_nvs = os.path.join(new_dir, 'nvs.bin')
		new_key = os.path.join(new_dir, 'nvs_keys.bin')
		os.rename(old_nvs, new_nvs)
		os.rename(old_key, new_key)

	#
	# remove DSNs from input directory
	#
	if not args.keep_dsns:
		dsns = pcsv.dsn_reserve(count = count)
		for i in range(count):
			pcsv.dsn_used(i)
		pcsv.dsn_delete(count = count)

	log.info(f'success: {count:d} devices')

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
		'Ayla Production File Generator prod_gen-' + __version__)
	parser.add_argument('--conf', required=True,
		default=None,
		help='OEM config input file')
	parser.add_argument('--csv_file', required=False,
		default='master.csv',
		help='output file for comma-separated-values for NVS')
	parser.add_argument('-d', '--debug', required=False,
		default=False, action='store_true',
		help='enable debug logging')
	parser.add_argument('--dsns', required=False,
		default='dsns',
		# TODO: allow zip file to be specified for ease of use.
		help='directory containing xml files for each device')
	parser.add_argument('--keep_dsns', required=False,
		default=False, action='store_true',
		help='keep DSN files after use')
	parser.add_argument('--model', required=True,
		help='manufacturer\'s module part name')
	parser.add_argument('--out', required=True, default=None,
		help='output directory')
	parser.add_argument('--prod', required=False,
		default=None,
		help='production package directory')
	parser.add_argument('--serial', required=False,
		default=None,
		help='batch serial number or date code')
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

	if not args.conf:
		args.conf = os.path.join(args.app, 'prod_config.txt')

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
		prod_conf = dict([['config',[]],['verify_id',[]]])

	try:
		prod_gen(args)
	except FileNotFoundError as e:
		log.error(f'prod_run: file not found error: {str(e):s}')
		sys.exit(1)

if __name__ == '__main__':
	_main()

# end
