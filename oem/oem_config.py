#! /usr/bin/env python3
# coding: utf-8
#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#
import sys
import os
import io
import argparse
import json
import hashlib
import base64
import shlex
import pexpect
import serial
from pexpect import fdpexpect
import logging as log

#
# Configure production agent with OEM-specific configuration items.
#
version = '0.4 2021-04-30'

#
# Set up logging and log version
#
def log_init():
	name = sys.argv[0]
	log.basicConfig(level=log.INFO,
	    format=name + ': %(levelname)s: %(message)s')
	log.info('version ' + version)

#
# Script default configuration variables.
#
class oem_config:
	baud = 115200		# module baud rate

	prompt_re = '(setup|-)-> ' # re for module prompt
	load_cmd = 'conf load'	# module command to load config

	# Exceptions

	class MOD_ERROR(Exception):	# error communicating with module
		pass
	class PARSE_ERROR(Exception):	# error parsing files
		pass
	class OPEN_ERROR(Exception):	# error opening files
		pass

	lines = []		# index of input line numbers per item

#
# Definitions for command-line arguments
#
def parse_args():
	parser = argparse.ArgumentParser(description =
		'Ayla OEM Configuration for Production Agents')
	parser.add_argument('--in', '-i', required=False,
		default='oem_config.txt',
		dest='infile',
		help='input file with configuration values')
	parser.add_argument('--logfile', '-l', required=False,
		help='file for log of interaction with module')
	parser.add_argument('--port', '-p', required=True,
		help='UART / COM  port connected to module')
	return parser.parse_args()

#
# Parse shell-syntax input file
# Return a dictionary with arrays of dictionaries.
#
def parse_config(path):
	conf = []
	verify_id = []
	prod = dict()
	errors = False

	def _err(msg):
		log.error(f'config file "{path:s} ' +
		    f'line {line_no:d}: {msg:s} at "{line:s}')
		errors = True

	hash = hashlib.sha256()
	with open(path, 'rb') as fp:
		hash.update(fp.read())
	digest = hash.hexdigest()[:8]
	log.info(f'config hash {digest:s}')
	conf.append(dict([['app/conf/hash', digest]]))

	with open(path, 'r', encoding='utf-8') as fp:
		line_no = 0
		for line in fp:
			line_no = line_no + 1
			line = line.strip()
			if len(line) > 0 and line[0] != '#':
				try:
					shell = shlex.shlex(line, posix=True)
					shell.whitespace_split = True
					args = list(shell)
				except:
					err = shell.token
					excerpt = err.partition('\n')[0]
					log.error('config file "%s" ' +
					    'line %d - syntax error: ' +
					    'at "%s"',
					    path, line_no, excerpt);
					errors = True
					continue
				if len(args) < 1:
					_err('no command')
					errors = True
					continue
				cmd = args[0]
				args = args[1:]
				if cmd == 'verify_id':
					if len(args) != 2:
						_err('verify_id needs 2 args')
						continue
					verify_id.append(dict([args]))
					continue
				if cmd == 'conf':
					if len(args) != 2:
						_err('conf needs 2 args')
						continue
					oem_config.lines.append(line_no)
					conf.append(dict([args]))
					continue
				if cmd == 'prod':
					if len(args) != 2:
						_err('prod needs 2 args')
						continue
					prod[args[0]] = args[1]
					continue
				_err('unrecognized cmd')
				continue
	if errors:
		raise oem_config.PARSE_ERROR
	body = dict([['config', conf],['verify_id', verify_id],['prod', prod]])
	return body

#
# Parse shell-syntax input file
# Return JSON string with array of config items.
#
def parse_config_to_json(path):
	body = parse_config(path)
	del body['verify_id']

	# encode the dictionary as JSON

	return json.JSONEncoder(separators=[',', ':']).encode(body)

#
# Encode the JSON string as a bytearray,
# appending sha256, and base64-encoding.
#
def config_encode(conf):
	# convert to byte array
	conf = bytearray(conf, encoding='utf-8')

	# append sha256 hash and base-64 encode

	m = hashlib.sha256()
	m.update(conf)
	b = base64.b64encode(conf + m.digest())
	return b.decode(encoding='utf-8')

#
# Open COM port
#
def open_port(port):
	path = port

	log.debug('Using port ' + path)
	try:
		fd = serial.Serial(path, oem_config.baud)
	except:
		log.error('open port: unexpected error: %s', sys.exc_info()[0])
		log.error('open of "%s" failed', path)
		raise oem_config.OPEN_ERROR
	return fd

#
# Send the 'conf load' command to the device
# Parse the response for success or error messages.
#
def send_load_cmd(spawn, blob):
	cmd = oem_config.load_cmd
	prompt_re = oem_config.prompt_re
	matches = [
		# match 0 is the expected success message
		'.*\nconf load: info: Success.*' + prompt_re,
		# match 1 is the specially-formatted error message
		'.*\nconf load: error: failed code ([0-9]+) '
		    'index ([0-9]+): ([^\r\n]+)\r\n.*' + prompt_re,
		# other errors
		'.*\nconf load: error: .*' + prompt_re,
		'.*\nUnrecognized command.*' + prompt_re,
		'.*\n' + prompt_re
	]
	spawn.send(cmd + ' ' + blob + '\r')
	try:
		i = spawn.expect(matches)
	except EOF:
		log.error('expect EOF')
		raise oem_config.MOD_ERROR
	except TIMEOUT:
		log.error('expect timeout')
		raise oem_config.MOD_ERROR
	except:
		log.error('send_load_cmd: unexpected error: %s',
		    sys.exc_info()[0])
		log.error('expect exception')
		raise oem_config.MOD_ERROR
	if i == 1:
		log.debug('cmd "%s" response matched %d "%s"',
		    cmd, i, matches[i])
		re = spawn.match
		code = re.group(1).decode(encoding='utf-8')
		index = re.group(2).decode(encoding='utf-8')
		msg = re.group(3).decode(encoding='utf-8')

		log.error('conf load error %s index %s: "%s"', code, index, msg)
		line = lines[int(index) - 1]
		log.error('failure for item on line %d of input', line)

		# Find line in input file from index
		try:
			line = lines[int(index) - 1]
			log.error('failure for item on line %d of input', line)
		except NameError:
			pass
		except ValueError:
			pass
		except:
			log.error('send_load_cmd: unexpected error: %s',
			    sys.exc_info()[0])
		raise oem_config.MOD_ERROR
	if i != 4:
		log.error('cmd "%s" matched prompt', cmd)
		raise oem_config.MOD_ERROR
	if i != 0:
		log.error('cmd "%s" matched %d "%s"', cmd, i, matches[i])
		raise oem_config.MOD_ERROR

def send_reset_cmd(spawn):
	prompt_re = oem_config.prompt_re

	matches = [
		'CLI ready\..*' + prompt_re,
		'Unrecognized command.*' + prompt_re,
		prompt_re,
	]

	# send reset command and wait for 'CLI ready.' message.

	retries = 3
	while retries > 0:
		spawn.send('reset' + '\r')
		try:
			i = spawn.expect(matches)
		except EOF:
			log.error('expect EOF on reset')
			raise oem_config.MOD_ERROR
		except TIMEOUT:
			log.error('expect timeout on reset')
			raise oem_config.MOD_ERROR
		except:
			log.error('expect exception')
			raise oem_config.MOD_ERROR
		if i == 0:
			break
		if i == 1:
			retries = retries - 1
	if retries == 0:
		log.error('empty cmd retries exhausted')
		raise oem_config.MOD_ERROR

	matches = prompt_re
	retries = 3
	while retries > 0:
		spawn.send('\r')
		try:
			i = spawn.expect(matches)
		except EOF:
			log.error('expect EOF on empty cmd')
			raise oem_config.MOD_ERROR
		except TIMEOUT:
			log.error('expect timeout on empty cmd')
			raise oem_config.MOD_ERROR
		except:
			log.error('expect exception on empty cmd')
			raise oem_config.MOD_ERROR
		if i == 0:
			break
		retries = retries - 1
	if retries == 0:
		log.error('empty cmd retries exhausted')
		raise oem_config.MOD_ERROR

	# discard any remaining console messages

	spawn.read_nonblocking(timeout=2)

#
# Interact with device to send it the configuration
#
def conf_load(fd, blob, logfile=None):
	spawn = pexpect.fdpexpect.fdspawn(fd, timeout=15, maxread=2000)
	if logfile != None:
		try:
			spawn.logfile_read = open(logfile, 'wb')
		except:
			log.error('can\'t open log file "%s"', logfile)
			raise oem_config.OPEN_ERROR
	try:
		send_reset_cmd(spawn)
		send_load_cmd(spawn, blob)
		send_reset_cmd(spawn)
	except:
		log.error('unexpected error: %s', sys.exc_info()[0])
		raise oem_config.OPEN_ERROR

#
# Main program
#
def _main():
	log_init()
	args = parse_args()

	# Form config string

	out = parse_config_to_json(args.infile)
	out = config_encode(out)

	# optionally, just print generated command and exit

	if args.port == '-':
		log.info('cut and paste this into the module CLI:')
		print(oem_config.load_cmd, out)
		return

	# send conf load command to device

	try:
		with open_port(args.port) as fd:
			conf_load(fd, out, logfile = args.logfile)
	except (oem_config.MOD_ERROR,
	    oem_config.PARSE_ERROR, oem_config.OPEN_ERROR):
		log.error('failed')
		raise SystemExit(1)
	except:
		log.error('failed: error: %s', sys.exc_info()[0])
		raise SystemExit(1)
	log.info('Success')

if __name__ == "__main__":
	_main()
# end
