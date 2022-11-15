#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#
'''
Class prod_install:  Uses esptool.py to flash a module in the background.

Example usage:
	from pyayla import prod_install

	p = prod_install.prod_install()
	p.at(offset1, file2, False)	# add files for each partition desired
	p.at(offset2, file2, False)	# ...
	p.install(port, timeout=120)	# do actual flashing
	while p.poll() == None:		# wait for completion or timeout
		time.sleep(1)
	p.get_output()			# show output of job
	p.close()			# alternative if output not needed
'''
import os
import subprocess
import tempfile
import time
import logging
import serial

_esptool_path = os.path.join('prod', 'esp-idf', 'components',
	'esptool_py', 'esptool')
_esptool_default = os.path.join(_esptool_path, 'esptool.py')
_espsecure = os.path.join(_esptool_path, 'espsecure.py')
_espefuse = os.path.join(_esptool_path, 'espefuse.py')

_prod_tool = os.path.join('prod', 'prod_tool.py')
_baud_rate = 921600
_tmp_dir = 'tmp'
_log_index = 0

def _log_init():
	_log = logging
	name = os.path.basename(sys.argv[0])
	if name.endswith('.py'):
		name = name[:-3]
	_log.basicConfig(level=logging.DEBUG,
	    format=name + ': %(levelname)s: %(message)s')
	return _log

def mkdir_try(path):
	try:
		os.mkdir(path)
	except FileExistsError:
		pass

class prod_install(object):
	def __init__(self, debug=False, log=None, conf=dict(), trace=False):
		if log:
			self.log = log
		else:
			self.log = _log_init()
		self.debug = debug
		self.trace = trace
		self.parts = []
		self.parts_encrypt = []
		self.subprocess = None
		self.start_time = None
		self.duration = None
		self.terminated = False
		self.output = ''
		self.conf = conf
		self.no_stub = False
		self.erase = True
		self.flash_mode = 'keep'
		self.flash_freq = 'keep'
		self.flash_size = 'keep'
		if self._chip_get() == 'esp32c3':
			self.flash_size = '4MB'

		global _tmp_dir
		global _log_index
		mkdir_try(_tmp_dir)
		self.stdout_path = os.path.join(_tmp_dir,
		    f'log{_log_index:d}.txt')
		_log_index = _log_index + 1
		self.stdout_fd = open(self.stdout_path, mode='a')

		self.enc_dir = tempfile.TemporaryDirectory(dir=_tmp_dir)
		self.key_path = os.path.join(self.enc_dir.name, 'key')
		self.jobs = list()

	#
	# add a partition to the list to be installed.
	#
	def at(self, offset, path, encrypt):
		if encrypt:
			self.parts_encrypt.append([hex(offset), path])
		else:
			self.parts.append([hex(offset), path])
		return 0

	def _check_port(self, port):
		try:
			with serial.Serial(port) as fd:
				pass
		except serial.serialutil.SerialException:
			self.output = 'error: failed to open ' + port
			return 1
		return 0

	def _conf_get(self, item, default):
		if item in self.conf:
			value = self.conf[item]
		else:
			value = default
		return value

	def _chip_get(self):
		return self._conf_get('chip', 'esp32')

	#
	# Add esptool to the jobs list to perform write flash
	#
	def _flash_write(self):
		try:
			cmd = os.getenv('ESPTOOL')
		except NameError:
			cmd = None
			pass

		if cmd == None:
			cmd = _esptool_default

		chip = self._chip_get()
		argv = ['python', cmd, '--port', self.port,
			'--chip', chip,
			'--baud', str(_baud_rate)]

		if self.trace:
			argv.append('--trace')

		argv = argv + ['--before', 'default_reset']
		argv = argv + ['--after', 'hard_reset']

		#
		# no_stub should be specified for C3 in some cases
		# This is a workaround for an esptool issue if
		# encryption is enabled.
		#
		if self.parts_encrypt and self._chip_get() == 'esp32c3':
			self.no_stub = True

		#
		# Note the stub is required for full chip erase
		#
		if self.no_stub:
			argv = argv + ['--no-stub']
			self.erase = False

		#
		# Write flash with specified parameters.
		# Use the defaults if nothing specified.
		#
		argv = argv + ['write_flash',
			'--no-progress',
			'--flash_mode', self.flash_mode,
			'--flash_freq', self.flash_freq,
			'--flash_size', self.flash_size,
		]

		if self.erase:
			argv.append('--erase-all')

		for part in self.parts:
			argv = argv + part

		if self.parts_encrypt:
			argv = argv + ['--encrypt-files']
			for part in self.parts_encrypt:
				argv = argv + part

		self.jobs.append(argv)
		return 0

	#
	# Run the next job in the list as a subprocess in the background.
	#
	def _job_run(self):
		job = self.job
		argv = self.jobs[job]
		self.log.debug(f'running job {job:d}: {argv}')

		#
		# Launch subprocess to do install
		#
		self.subprocess = subprocess.Popen(argv,
		    stdout=self.stdout_fd, stderr=subprocess.STDOUT)
		return 0

	#
	# Set up jobs to encrypt the necessary partitions offline.
	# Add the encrypted files to the non-encrypted partition list.
	#
	def _parts_pre_encrypt(self):
		chip = self._chip_get()
		for part in self.parts_encrypt:
			offset = part[0]
			imagename = part[1]

			dir_path, file_name = os.path.split(imagename)
			cipher_file = os.path.join(self.enc_dir.name, file_name)

			argv = ['python', _espsecure, 'encrypt_flash_data',
				'--keyfile', self.key_path,
				'--output', cipher_file,
				'--address', offset]
			if chip == 'esp32c3' or chip == 'esp32s2':
				argv.append('--aes_xts')
			argv.append(imagename)

			self.jobs.append(argv)
			self.parts.append([offset, cipher_file])

		self.parts_encrypt = list()	# do no encrypting downloads

	#
	# Add prod_tool.py to jobs list
	#
	def _prod_tool(self, options):
		argv = ['python', _prod_tool, '--port', self.port,
			'--chip', self._chip_get()] + options
		self.jobs.append(argv)

	#
	# Use prod_tool.py in subprocess to verify encryption is off.
	#
	def _verify_encrypt_off(self):
		self._prod_tool(['verify_encrypt_off'])

	#
	# Add job to generate key
	#
	def _encrypt_key_gen(self):
		argv = ['python', _espsecure, 'generate_flash_encryption_key',
			'--keylen', '256', self.key_path]
		self.jobs.append(argv)

	#
	# Add jobs using espefuse.py to set efuses for key and encryption.
	#
	def _encrypt_enable(self):
		chip = self._chip_get()

		cmd = ['python', _espefuse,
			'--port', self.port,
			'--chip', self._chip_get(),
			'--do-not-confirm']

		if chip == 'esp32c3':
			fuses = ['SPI_BOOT_CRYPT_CNT', '1']
			key_block = 'BLOCK_KEY0'
			key_purpose = 'XTS_AES_128_KEY'
		else:
			fuses = ['FLASH_CRYPT_CNT', '1',
				 'FLASH_CRYPT_CONFIG', '0xf']
			key_block = 'flash_encryption'
			key_purpose = None

		argv = cmd + ['burn_key', key_block, self.key_path]
		if key_purpose:
			argv.append(key_purpose)
		self.jobs.append(argv)

		argv = cmd + ['burn_efuse'] + fuses
		self.jobs.append(argv)

	#
	# Set erase option.
	#
	def erase_all_set(self, val):
		self.erase = val

	#
	# Set no_stub option:
	#
	def no_stub_set(self, val):
		self.no_stub = val

	def flash_mode_set(val):
		self.flash_mode = val

	def flash_size_set(val):
		self.flash_size = val

	def flash_freq_set(val):
		self.flash_freq = val

	#
	# start the install
	#
	def install(self, port, timeout=120, encrypt=False):
		self.port = port
		self.timeout = timeout;
		self.encrypt = encrypt
		self.start_time = time.monotonic()
		self.job = 0
		self.jobs = list()
		if encrypt:
			self.erase = False

		if self._check_port(port):
			return 1

		#
		# The encrypt argument on the first call implies reflashing.
		#
		# If encrypt is false, we are doing the initial flashing and
		# should set the key and do offline encryption of the files.
		#
		# If encrypt is true, we flash with the encrypt option so
		# that the chip will encrypt it internally, since we won't have
		# a key.
		#
		if not encrypt:
			self._verify_encrypt_off()
			self._encrypt_key_gen()
			self._encrypt_enable()
			self._parts_pre_encrypt()
		self._flash_write()
		self._job_run()

	def set_duration(self):
		if self.duration == None:
			self.duration = time.monotonic() - self.start_time

	#
	# Determine whether the subprocess is finished or not.
	# Returns returncode or None
	#
	def poll(self):
		proc = self.subprocess
		if proc == None:		# never started
			return -1
		status = proc.poll()
		if status == None and (not self.terminated) and \
		    time.monotonic() > self.timeout + self.start_time:
			proc.terminate()
			self.terminated = True
			print('job timed out')	# TODO
			return status
		if status != None and status == 0:
			#
			# A process is complete.  Run the next job, if any.
			#
			self.job += 1
			if self.job < len(self.jobs):
				self._job_run()
				return None
		if status != None:
			#
			# All processes complete
			#
			self.set_duration()
		return status

	#
	# Show output and status of the prod_install job
	#
	def get_output(self):
		proc = self.subprocess
		if proc == None:		# never started
			self.log.info(self.output)
			stderr = None
		else:
			#
			# Get output from subprocess
			#
			stdout, stderr = proc.communicate()
			status = self.poll()
			if stdout != None:
				stdout = stdout.decode(encoding='utf-8')

			if status == None:
				self.log.debug('status None')
			else:
				self.log.debug(f'status {status:d}')
				self.log.debug(f'args: {self.jobs[self.job]}')

			if stdout != None:
				for line in stdout.splitlines():
					self.log.info('output: ' + line)

		#
		# We should not see stderr, because it was redirected to stdout,
		# Do this anyway.
		#
		if stderr != None:
			stderr = stderr.decode(encoding='utf-8')
			for line in stderr.splitlines():
				self.log.info('err out: ' + line)
		self.subprocess = None

		#
		# Show the log file, if any
		#
		if self.stdout_fd:
			self.stdout_fd.close()
			with open(self.stdout_path) as f:
				for line in f:
					self.log.info('output: ' + line.strip())

	#
	# Clean up resources without showing output
	#
	def close(self):
		proc = self.subprocess
		if proc != None:
			stdout, stderr = proc.communicate()
			self.subprocess = None
			if self.stdout_fd:
				self.stdout_fd.close()
