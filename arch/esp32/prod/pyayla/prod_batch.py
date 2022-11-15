#! /usr/bin/env python3
# coding: utf-8
#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#
import os
import io
import time
import argparse
import logging
from . import prod_install
from . import prod_expect

nvs_prefix = 'p-'
key_prefix = 'keys-'

#
# partition offsets for 4M or 8M device.
# The OTA1 partitions are not used by this script.
#
# Version 1 for esp32-based fast-track devices
#
class partition_offsets_v1:
	boot = 0x1000
	part = 0xe000
	phy_data = 0x10000
	nvs = 0x13000
	nvs_keys = 0xff000
	ota_0 = 0x100000

#
# Version 2 for esp32-c3 - compatible with ADA
# Note that esp32-c3 has a different bootloader offset from esp32.
#
class partition_offsets_v2:
	boot = 0
	part = 0xe000
	phy_data = 0xf000
	ota_0 = 0x10000
	nvs = 0x310000
	nvs_keys = 0x370000

def partition_offsets(conf):
	prod = conf['prod']
	item = 'partition_ver'
	if item in prod:
		part_ver = prod[item]
	else:
		part_ver = '1'

	if part_ver == '1':
		return partition_offsets_v1()
	if part_ver == '2':
		return partition_offsets_v2()
	log.error('invalid partition_ver')
	return None

#
# Start flash download for single part
# args are the command arguments from prod_run.py.
# path is the filename in the nvs directory.
# port is the serial port device file.
# encrypt is true if device is expected to already have encryption enabled.
# conf is the prod_conf list of dictionaries from oem_config.
#
# Returns the flash_install object, with job started.
# The caller should call the flash_install.poll() function to get status.
#
def flash_dev(args, path, port, nvs_path, nvs_key, log, encrypt, conf):
	global key_prefix

	part = partition_offsets(conf)

	image = args.images
	nvs = os.path.join(nvs_path, path)

	#
	# installer - Could use another class if needed by replacing import
	# statement above.
	#
	install = prod_install.prod_install(debug=args.debug, log=log,
	     trace=args.trace,
	     conf=conf['prod'])

	#
	# Add partitions
	#
	if not args.no_boot:
		install.at(part.boot, os.path.join(image, "bootloader.bin"),
		    True)
	install.at(part.part,
	    os.path.join(image, "partition_table.bin"), True)
	if not args.no_app:
		install.at(part.ota_0, os.path.join(image, "app.bin"), True)
	if nvs_key:
		nvs_keys = os.path.join(nvs_key, key_prefix + path)
		install.at(part.nvs_keys, nvs_keys, True)
	#
	# NVS should not be encrypted using flash encryption
	#
	install.at(part.nvs, nvs, False)

	if args.no_erase or args.no_boot or encrypt:
		install.erase_all_set(False)
	if args.no_stub:
		install.no_stub_set(True)

	#
	# Start install job
	#
	install.install(port, encrypt=encrypt)
	return install

#
# On success, remove the nvs image so it is not reused
#
def flash_remove_nvs(path_dir, path):
	nvs = os.path.join(path_dir, path)
	os.remove(nvs)

#
# Pause between parts
#
def flash_pause():
	print('Connect next device to be initialized')
	while True:
		resp = input('Enter "go" to continue, "q" to quit: ')
		if resp == 'go':
			print('')
			return True
		if resp == 'q' or resp == 'quit':
			return False
		continue
	return False

#
# Compare each item in conf to be verified with the corresponding item
# read from the module in ids.
# Return None if OK, otherwise 3-tuple of (name, value, expected value)
#
def id_compare(dsn, ids, conf):
	if dsn:
		vlist = [dict([['DSN', dsn]])] 	# list of dictionaries
	else:
		vlist = [dict()]
	if conf and 'verify_id' in conf:
		vlist = vlist + conf['verify_id']
	for node in vlist:
		for name in list(node):
			print(f'verifying name {name:s} val {node[name]:s}')
			if name not in ids:
				return (name, None, None)
			if node[name] != ids[name]:
				return (name, ids[name], node[name])
	return None

#
# Return status of comparisons from verify job
#
def verify_ids(dsn, ids, conf):
	mismatch = id_compare(dsn, ids, conf)
	if mismatch == None:
		return 0
	name = mismatch[0]
	read = mismatch[1]
	exp = mismatch[2]
	if exp == None:
		return 'ID "' + name + '" not read from module'
	return f'mismatch: name "{name:s}" read "{read:s}" expected "{exp:s}"'

#
# Structure used to track a batch of jobs
#
class batch_info(object):
	def __init__(self):
		self.index = 0
		self.status = None
		self.flash_status = None
		self.flash_job = None
		self.verify_job = None
		self.duration = 0
		self.ids = None
		self.port = None

def _log_init():
	_log = logging
	name = os.path.basename(sys.argv[0])
	if name.endswith('.py'):
		name = name[:-3]
	_log.basicConfig(level=logging.DEBUG,
	    format=name + ': %(levelname)s: %(message)s')
	return _log

#
# Flash a group of devices in parallel
# Returns a list containing status for each device in the group, 0 for success.
#
def prod_batch(args, paths, dsns, nvs_dir, nvs_keys_dir, conf, log=None,
    encrypt=False):
	if not log:
		log = _log_init()
	group_size = len(paths)
	if group_size == 0:
		return [-1]
	if group_size > 1:
		log.info(f'flashing group of {group_size:d} devices. ' + \
		    'Please wait.')
	else:
		log.info('flashing 1 device.  Please wait.')
	print('')

	batch = [None] * group_size

	#
	# start a flash job on each port in the group.
	#
	for i in range(group_size):
		bi = batch_info()
		batch[i] = bi
		bi.index = i
		bi.port = args.ports[i]
		bi.flash_job = flash_dev(args, paths[i], args.ports[i],
		    nvs_dir, nvs_keys_dir, log, encrypt, conf)

	#
	# As each flash job finishes, if successful, start to verify the id
	#
	running = True
	while running:
		running = False
		for bi in batch:
			if bi.flash_status != None:
				continue
			status = bi.flash_job.poll()
			bi.flash_status = status
			if status == None:
				running = True
			elif status == 0:
				bi.flash_job.close()
				bi.duration = bi.flash_job.duration
				#
				# start verify job
				#
				com = prod_expect.prod_expect()
				bi.verify_job = com.id_get_job(bi.port)
			else:
				bi.status = status
		if running:
			time.sleep(.2)

	#
	# Wait until all verify jobs are finished or time out
	#
	running = True
	while running:
		running = False
		for i in range(len(batch)):
			bi = batch[i]
			job = bi.verify_job
			if bi.status != None or job == None:
				continue
			status = job.poll()
			bi.status = status
			if status == None:
				running = True
				continue
			bi.verify_job = None
			if status == 0:
				bi.ids = job.id_get_job_ids()
				if bi.ids == None:
					bi.status = 'get ID failed'
				elif dsns == None:
					bi.status = verify_ids(None,
					    bi.ids, conf)
				else:
					bi.status = verify_ids(dsns[i],
					    bi.ids, conf)
		if running:
			time.sleep(.2)

	#
	# Summarize status
	#
	good_count = 0
	failed_count = 0
	statuses = [None] * group_size
	for i in range(group_size):
		bi = batch[i]
		statuses[i] = bi.status

		ids = bi.ids
		dsn = '*** NOT SET ***'
		mac = dsn
		failed = False
		if bi.status == 0:
			flash_remove_nvs(nvs_dir, paths[i])
			try:
				dsn = ids['DSN']
				if dsn == '':
					dsn = '*** NOT SET ***'
					bi.status = 'Failed - DSN not set'
					failed = True
			except KeyError:
				bi.status = 'Failed - DSN not read'
				failed = True
			try:
				mac = ids['eFuse MAC']
			except KeyError:
				bi.status = 'Failed - MAC not read'
				failed = True
		else:
			failed = True

		if failed:
			status = 'Failed'
			failed_count = failed_count + 1
			msg = str(bi.status)
		else:
			status = 'OK'
			good_count = good_count + 1
			msg = 'OK'

		log.info(f'device {bi.index:2d} {status:8s} DSN {dsn:16s}' + \
		    f'MAC {mac:18s} time {bi.duration:.3f} s {msg:s}')

	if failed_count:
		print('')
		for i in range(group_size):
			bi = batch[i]
			if bi.flash_status != 0:
				# TODO put logs in files?
				log.warning(f'device {i:d} Failed ' + \
				    f'status {str(bi.status):s}')
				log.info('-------- failure logs start --------')
				bi.flash_job.get_output()
				log.info('-------- failure logs end --------')
		log.warning(f'failures: {failed_count:d}, good: {good_count:d}')
	else:
		log.info('Success')
	return statuses;

# end
