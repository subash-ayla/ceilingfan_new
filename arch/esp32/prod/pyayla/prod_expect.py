#! /usr/bin/env python3
# coding: utf-8
#
# Copyright 2020-2021 Ayla Networks, Inc.  All rights reserved.
#
'''
Module handling expect interactions for Ayla module manufacture and
OEM configuration.
'''
import sys
import os
import io
import time
import re
import serial
import logging
import threading

#
# prod_expect class
#
# Note that pexpect doesn't work from serial ports on Windows and nothing
# better was found, including wexpect, without going to TCL-based expect.
#
# This class satisfies the needed use-cases and not much more.
#
class prod_expect(object):
	baud = 115200		# module baud rate
	prompts = ['setup-> ', '--> ']
	log = None
	read_size = 1024	# bytes to read per call
	debug = False

	# Exceptions

	# Error detected on module
	class MOD_ERROR(Exception):
		pass
	# Error opening serial port
	class OPEN_ERROR(Exception):
		pass
	# Error in expect dialog
	class EXPECT_ERROR(Exception):
		pass
	# timeout in expect dialog
	class EXPECT_TIMEOUT(Exception):
		pass

	def __init__(self, log=None):
		if log == None:
			log = logging
			name = os.path.basename(sys.argv[0])
			if name.endswith('.py'):
				name = name[:-3]
			log.basicConfig(level=logging.DEBUG,
			    format=name + ': %(levelname)s: %(message)s')
		self.log = log
		self.thread = None
		self.ids = None
		self.port = None
		self.serial = None
		self.match = None
		self.rx_buf = ''

	#
	# Open COM port
	#
	def open_port(self, port):
		self.port = port
		try:
			self.serial = serial.Serial(port, self.baud, timeout=0)
		except:
			self.log.error('open port: unexpected error: %s',
			    sys.exc_info()[0])
			self.log.error('open of "%s" failed', port)
			raise prod_expect.OPEN_ERROR

	#
	# Read as much as possible from the port into the buffer.
	#
	def _read(self):
		added = 0
		while True:
			s = self.serial.read(self.read_size)
			if not s:
				break
			s = s.decode(errors='ignore')
			self.rx_buf += s
			added = added + len(s)
		return added

	def send(self, data):
		self.serial.write(data.encode())
		self.serial.flush()
		if self.debug:
			out = str(data.encode(encoding='unicode_escape'))
			self.log.debug(f'expect: sent {data:s}')

	#
	# Match one pattern against the buffer
	# Remove the buffer up to the match.
	#
	def _match(self, patt, match_re):
		if match_re:
			match = re.search(patt, self.rx_buf)
			if match:
				self.match = match
				head = match.end()
				self.rx_buf = self.rx_buf[head:]
				return True
		else:
			patt = patt.replace('\r\n', '\n')
			index = self.rx_buf.find(patt)
			match = (index >= 0)
			if index >= 0:
				head = index + len(patt)
				self.rx_buf = self.rx_buf[head:]
				return True
		return False

	#
	# Try to match one of the supplied patterns with data read from the
	# serial port.
	#
	def expect(self, patterns, timeout=10, match_re=False):
		def debug(str):
			if self.debug:
				self.log.debug(str)

		start = time.monotonic()
		self.match = None

		if match_re:
			mtype = 're'
		else:
			mtype = 'patt'

		while True:
			self._read()
			debug(f'expect: rx_buf: {str(self.rx_buf):s}')

			for i in range(len(patterns)):
				patt = patterns[i]
				match = self._match(patt, match_re)
				if match:
					result = 'matched'
				else:
					result = 'tried  '
				pe = str(patt.encode(encoding='unicode_escape'))
				debug(f'{result:s} {mtype:s} {pe:s}')
				if match:
					return i
			if time.monotonic() - start > timeout:
				raise prod_expect.EXPECT_TIMEOUT
			time.sleep(0.05)
		return None

	def send_reset_cmd(self):
		log = self.log

		matches = [
			'[\r\n]+CLI ready.[\r\n]',
			'[\r\n]+' + self.prompts[0],
			'[\r\n]+' + self.prompts[1],
		]

		# send reset command and wait for 'CLI ready.' message.

		retries = 3
		timeout = 120		# initial timeout to wait for encrypt
		while retries > 0:
			self.send('reset' + '\r')
			try:
				i = self.expect(matches, match_re=True,
				    timeout=timeout)
			except prod_expect.EXPECT_TIMEOUT:
				retries = retries - 1
				continue
			if i == 0:
				break
			timeout=10
		if retries == 0:
			log.error('reset cmd retries exhausted')
			raise prod_expect.EXPECT_ERROR
		self.prompt_wait()

	def prompt_wait(self):
		log = self.log

		matches = [
			'[\r\n]+' + self.prompts[0],
			'[\r\n]+' + self.prompts[1]
		]
		retries = 8
		while retries > 0:
			self.send('\r')
			try:
				i = self.expect(matches, timeout=0.5,
				    match_re=True)
			except prod_expect.EXPECT_TIMEOUT:
				retries = retries - 1
				continue
			break
		if retries == 0:
			log.error('empty cmd retries exhausted')
			raise prod_expect.EXPECT_ERROR

	#
	# Sends 'get id' command.
	# Returns a dictionary of name/value pairs.
	#
	def send_conf_id(self, timeout=15):
		log = self.log

		self.send('conf id\r')
		ids = dict()

		#
		# Form list of patterns to look for.
		# Look for echoed command first.
		#
		echo_seen = False
		echo_match = 0
		matches = ['conf id\r']

		#
		# Create a pattern matching the field at the start of
		# the line followed by one or more tabs followed by
		# any characters except CR/LF followed by CR/LF
		#
		patt_match = len(matches)
		matches.append('([^\n\r\t:]*):\t+([^\r\n]*)[\r\n]+')

		# skip CR/LF before prompts, since it is in the ID pattern
		prompt0_match = len(matches)
		matches.append(self.prompts[0])
		prompt1_match = len(matches)
		matches.append(self.prompts[1])

		while True:
			i = self.expect(matches, timeout=timeout, match_re=True)
			if not echo_seen:
				if i == echo_match:
					echo_seen = True
				continue
			if i == prompt0_match or i == prompt1_match:
				break
			if i == patt_match:
				name = self.match.group(1)
				val = self.match.group(2)
				ids[name] = val
			else:
				log.warning('matched pattern %d', i)
				raise prod_expect.MOD_ERROR
		return ids

	#
	# Interact with device to get the device ID information
	#
	# Returns a dictionary of name/value pairs.
	# Returns an empty dictionary on failure
	#
	def _id_get(self, timeout=15, attempt=0):
		log = self.log
		try:
			return self.send_conf_id(timeout=timeout)
		except prod_expect.EXPECT_TIMEOUT:
			log.warning('timeout during send ID ' +
				f'attempt {attempt:d}')
		except prod_expect.MOD_ERROR:
			log.warning('MOD_ERROR during send ID ' +
				f'attempt {attempt:d}')
		except prod_expect.EXPECT_ERROR:
			log.warning(f'EXPECT_ERROR during send ID ' +
				f'attempt {attempt:d}')
		return dict()

	#
	# Interact with device to get the device ID information
	#
	# Do this twice in case there are extra messages that interfere
	# with the output.  If there is a mismatch repeat up to 5 times.
	#
	# Returns a dictionary of name/value pairs.
	# Returns an empty dictionary on failure.
	#
	def id_get(self):
		timeout = 3
		log = self.log
		ids = self._id_get(timeout=timeout)
		for attempt in range(5):
			timeout = timeout * 1.5
			ids2 = self._id_get(timeout=timeout)
			if ids == ids2 and len(ids) > 0:
				return ids
			ids = ids2
		log.error(f'conf id had mismatches')
		return dict()

	def close(self):
		if self.serial:
			self.serial.close()
			self.serial = None

	#
	# Open and get ID.
	#
	def open_id_get(self, port):
		log = self.log
		self.ids = None
		try:
			step = "open"
			self.open_port(port)
			step = "reset"
			self.send_reset_cmd()
			step = "get"
			self.ids = self.id_get()
		except:
			log.error('open_id_get: ' +  \
			    'exception %s during %s, port %s',
			    sys.exc_info()[0], step, port)
			raise

	#
	# Start a background thread that gets ID using this script.
	#
	def id_get_job(self, port):
		log = self.log
		thread = threading.Thread(target=self.open_id_get, args=[port])
		thread.start()
		self.thread = thread
		return self

	#
	# Get status of thread.
	# Returns, None if thread is running or never started, 0 otherwise.
	#
	def poll(self):
		thread = self.thread
		if not thread:
			return None
		if thread.is_alive():
			return None
		thread.join()
		return 0

	#
	# Get dictionary of IDs (e.g., DSN, MAC) from thread
	#
	def id_get_job_ids(self):
		return self.ids

#
# Test program - this module is expected to be imported, not used directly.
#
def _main():
	com = prod_expect()
	log = com.log
	com.debug = True

	try:
		com.open_port('/dev/ttyUSB0')
	except prod_expect.OPEN_ERROR:
		com.log.error('open error')
		sys.exit(1)

	try:
		com.send_reset_cmd()
	except prod_expect.EXPECT_ERROR:
		log.error('EXPECT_ERROR during reset')
		sys.exit(1)

	try:
		ids = com.id_get()
		print('\n\nIDs:')
		for name in list(ids):
			print(f'    "{name:s}"\t"{ids[name]:s}"')
	except prod_expect.EXPECT_TIMEOUT:
		log.error('expect timeout')
		sys.exit(1)
	except prod_expect.EXPECT_ERROR:
		log.error('expect error')
		sys.exit(1)
	except prod_expect.MOD_ERROR:
		log.error('module error')
		sys.exit(1)
	except prod_expect.OPEN_ERROR:
		log.error('open error')
		sys.exit(1)
	com.close()

	log.info('Success')

if __name__ == "__main__":
	_main()
# end
