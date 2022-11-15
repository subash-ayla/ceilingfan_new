#! /usr/bin/env python3
# coding: utf-8
#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#
import os
import sys
import io
import csv
import time
import argparse
import base64
from Crypto.Cipher import PKCS1_v1_5
from Crypto.PublicKey import RSA
from xml.parsers.expat import ParserCreate, ExpatError, errors
from datetime import datetime,timezone
from . import ayla_conf_tokens

#
# Generate ESP-IDF csv files from Ayla Dashboard Reserve job .xml files
#
# The XML files will be in a directory called dsns.
# As they are used, they'll be removed so they are not reused.
#

#
# State for XML parsing one device
#
class parse_state:
	def __init__(self):
		self.tag = None
		self.text = ''
		self.dict = dict()

	def tag_start(self, tag):
		if tag == 'dsn' or tag == 'public-key':
			self.tag = tag
		else:
			self.tag = None

	def tag_end(self, tag):
		if tag == 'f-device':
			return
		if tag != self.tag:
			print('not end of expected tag', self.tag)
		self.dict[tag] = self.text
		self.tag = None
		self.text = ''

	def text_append(self, text):
		global xml_tag
		global xml_text

		if self.tag == None:
			return

		#
		# for public-key, drop starting and ending lines, strip blanks
		#
		if self.tag == 'public-key':
			if text == '-----BEGIN RSA PUBLIC KEY-----':
				return
			if text == '-----END RSA PUBLIC KEY-----':
				return
			text = text.strip(' \t\n')
		self.text = self.text + text

xml_parse_state = None

#
# Handlers for fields in XML
#
def xml_start_element(name, attrs):
	global xml_parse_state
	xml_parse_state.tag_start(name)

def xml_end_element(name):
	global xml_parse_state
	xml_parse_state.tag_end(name)

def xml_text_element(text):
	global xml_parse_state
	xml_parse_state.text_append(text)

#
# device info class.
# Includes parse_xml method to get info from file
#
class Device_info:

	def __init__(self):
		self.dsn = ''
		self.key = ''
		self.tag = None
		self.text = ''
		self.dict = None

	def parse_xml(self, path):
		global xml_parse_state

		file = io.FileIO(path, mode='r')

		parser = ParserCreate()

		xml_parse_state = parse_state()

		parser.StartElementHandler = xml_start_element
		parser.EndElementHandler = xml_end_element
		parser.CharacterDataHandler = xml_text_element
		try:
			parser.ParseFile(file)
		except ExpatError as err:
			print("error:", errors.messages[err.code])
		self.dict = xml_parse_state.dict
		xml_parse_state = None

	def csv_row(self):
		return [self.dict['dsn'], self.dict['public-key']]

	def value(self, dict_key):
		return self.dict[dict_key]

class CONF_NAME_NOT_MAPPED(Exception):
	pass
class CONF_NAME_TOO_LONG(Exception):
	pass
class CONF_NAME_NOT_FOUND(Exception):
	pass

#
# Class to put DSN info into CSV files for manufacturing config.
#
class prod_csv(object):

	def __init__(self, model, start_sn = None,
	    start_id = 1, xml_dir = 'dsns', out_file = 'master.csv',
	    omit_dsn = False):
		if start_sn == None:
			now = datetime.utcnow()
			start_sn = now.isoformat(timespec='minutes')
		self.start_sn = start_sn
		self.line = start_id
		self.xml_dir = xml_dir
		self.out_file = out_file
		self.omit_dsn = omit_dsn
		self.config = dict([
		    ['sys/mfg_model', model],
		    ['sys/mfg_serial', None]])
		if not omit_dsn:
			self.config['id/dev_id'] = None
			self.config['id/key'] = None
		self.oem_secret = None
		self.verify = dict()
		self.dsns = []

	def config_name_to_tokens(self, config_path):
		# table of token mappings
		tokens = ayla_conf_tokens.tokens
		nvs_name = ''
		literal = False
		for part in config_path.split('/'):
			short_token = None
			for tok in tokens:
				if part == tok[1]:
					short_token = tok[0]
					break
			#
			# If a short token was not found, and does not contain
			# uppercase, use it literally, it may be a number.
			#
			if short_token:
				literal = False
			else:
				if part != part.lower():
					print('path', config_path,
					    'token', part, 'not found')
					raise CONF_NAME_NOT_MAPPED

				# Insert a slash between two literals in a row.
				if literal:
					nvs_name = nvs_name + '/'
				literal = True
				short_token = part;
			nvs_name = nvs_name + short_token
		return nvs_name

	#
	# Convert a config path name to an NVS name
	#
	def config_to_nvs(self, name):
		#
		# convert '/app' to NVS name for factory app
		#
		if name[0:4] == 'app/':
			nvs_name = '.' + name[4:]
		#
		# if name is short enough and doesn't start with
		# upper case, use it
		#
		elif len(name) < 15 and not name[0].isupper():
			nvs_name = name
		else:
			nvs_name = self.config_name_to_tokens(name)

		if len(nvs_name) > 15:
			print('path', name, 'nvs_name', nvs_name, 'too long')
			raise CONF_NAME_TOO_LONG
		return nvs_name

	def config_field_type(self, name):
		#
		# Most config items are binary and supplied in CSV as strings.
		# The OEM key is binary and supplied as base-64 encoded.
		#
		if name == 'oem/key' and not self.omit_dsn:
			return 'base64'
		return 'binary'

	#
	# Add dictionary entries of config items to be put in NVS.
	#
	def config_update(self, entries):
		self.config.update(entries)

	#
	# Encrypt the OEM secret, OEM ID and model using module's public key.
	#
	def oem_key_get(self):
		conf = self.config
		required = ['oem/oem', 'oem/model']
		if not self.omit_dsn:
			required.append('id/key')

		for item in required:
			if item not in conf:
				print('Error: config item', item,
				    'is missing and required.')
				sys.exit(1)
		try:
			oem_id = conf['oem/oem']
			oem_model = conf['oem/model']
			if not self.omit_dsn:
				pub_key = conf['id/key']
		except KeyError:
			print('oem_key_get: missing item')
			sys.exit(1)

		#
		# If the config file specifies oem/key/model, it is the
		# model to be encrypted along with the OEM secret.
		# It may be specified as '*' to allow the OEM model to be
		# changed to anything later.
		#
		if 'oem/key/model' in conf:
			oem_model = conf['oem/key/model']

		plaintext = self.oem_secret + ' ' + oem_id + ' ' + oem_model

		#
		# If we don't have the DSN and public key, the module will
		# have to encrypt it later.
		#
		if self.omit_dsn:
			plaintext = "encr " + plaintext
			return plaintext

		plaintext = str(int(time.time())) + ' ' + plaintext

		plaintext = plaintext.encode()		# convert to bytes
		der = base64.b64decode(pub_key)		# decode public key
		rsa_key = RSA.import_key(der)		# import it
		cipher = PKCS1_v1_5.new(rsa_key)	# set up encryptor
		ciphertext = cipher.encrypt(plaintext)	# encrypt
		b64 = base64.b64encode(ciphertext)	# base64 encode as bytes
		b64 = b64.decode()			# convert to str
		return b64

	#
	# Make config.csv with fields to be populated per DSN
	#
	def gen_config_csv(self, out_file, config=None):
		if config:
			for node in list(config):
				for name in list(node):
					nvs_name = self.config_to_nvs(name)
					self.config[nvs_name] = node[name]
			if 'oem/key' in self.config:
				self.oem_secret = self.config['oem/key']
		with open(out_file, 'w', newline='') as csv_file:
			writer = csv.writer(csv_file)
			writer.writerow(['ayla_namespace','namespace',''])
			for key in list(self.config):
				writer.writerow([key, 'data',
				    self.config_field_type(key)])

	#
	# Process XML file, update object dictionary, output csv line
	#
	def dsn_file_read(self, xml_file):
		global args

		dev = Device_info()
		dev.parse_xml(xml_file)

		conf = self.config
		conf['id/dev_id'] = dev.value('dsn')
		conf['id/key'] = dev.value('public-key')

	def csv_row_write(self, writer):
		conf = self.config
		conf['sys/mfg_serial'] = self.start_sn + '-' + str(self.line)
		if 'oem/key' in conf:
			conf['oem/key'] = self.oem_key_get()
		row = [self.line] + list(conf.values())
		writer.writerow(row)

	#
	# Return the count of (unused) DSN files in XML dir
	#
	def dsn_count(self):
		count = 0
		with os.scandir(self.xml_dir) as it:
			for entry in it:
				if entry.name.endswith('.xml'):
					count = count + 1
		return count

	#
	# Indicate DSNs are starting to be programmed so they won't be reused.
	# Rename the DSN file by index.
	# Return the list of actual DSNs, extracted from the file name.
	#
	def dsn_reserve(self, count=1):
		if self.omit_dsn:
			return None
		dsns = [None] * count
		if count > len(self.dsns):
			print(f'reserve error: count {count:d} ' + \
			    f'dsns {str(self.dsns):s}')
		for i in range(count):
			old_name = self.dsns[i]
			new_name = old_name[:-4] + '-used.txt'
			os.rename(old_name, new_name)
			self.dsns[i] = new_name

			old_name = os.path.basename(old_name)
			dsns[i] = old_name[:-4]		# remove .xml
		return dsns

	#
	# Indicate a DSN was successfully flashed.
	# Remove the DSN file by its index in the list
	#
	def dsn_used(self, index):
		if self.omit_dsn:
			return
		try:
			name = self.dsns[index]
		except IndexError:
			print('remove fails', index, 'dsns', self.dsns)
			raise
		self.dsns[index] = None
		os.remove(name)

	#
	# Indicate the reserved group of DSNs that should be forgotten.
	#
	def dsn_delete(self, count=1):
		if self.omit_dsn:
			return
		del self.dsns[0:count]

	#
	# Get a list of the DSNs in the group.
	#
	def dsns_get(self):
		dsns = self.dsns
		for i in range(len(dsns)):
			dsn = dsns[i]
			dsn = os.path.basename(dsn)
			if dsn.endswith('.xml'):
				dsn = dsn[:-4]
			dsns[i] = dsn
		return dsns

	#
	# Main function - Make CSV file line for each DSN
	#
	def gen(self, limit=100):
		count = 0
		self.dsns = []
		#
		# Open output CSV file
		#
		with open(self.out_file, 'w', newline='') as csv_file:
			writer = csv.writer(csv_file)

			#
			# write header row
			#
			row = ['id'] + list(self.config)
			writer.writerow(row)

			#
			# If reflashing, don't use DSNs, just write CSV row
			# for each potential part.
			#
			if self.omit_dsn:
				for count in range(limit):
					self.csv_row_write(writer)
				return limit

			#
			# Go through XML files in the xml directory
			#
			with os.scandir(self.xml_dir) as it:
				for entry in it:
					path = entry.name
					if path.endswith('.xml'):
						relpath = \
						    os.path.join(self.xml_dir,
						    path)
						self.dsns.append(relpath)
						self.dsn_file_read(relpath)
						self.csv_row_write(writer)

						self.line = self.line + 1
						count = count + 1
						if count >= limit:
							break
		return count

#
# Defaults for command-line arguments
#
def parse_args():
	parser = argparse.ArgumentParser(description =
		'Ayla Production File CSV Generator')
	parser.add_argument('--config', '--conf', required=False,
		help='additional config input file')
	parser.add_argument('--csv', required=True,
		default='config.csv',
		help='output file for field definitions for NVS')
	parser.add_argument('--out', required=True,
		default='master.csv',
		help='output file for comma-separated-values for NVS')
	parser.add_argument('--dsns', required=False,
		default='dsns',
		help='directory containing xml files for each device')
	parser.add_argument('--serial', required=False,
		default=None,
		help='batch serial number or date code')
	parser.add_argument('--model', required=True,
		help='manufacturer\'s module part name')
	return parser.parse_args()

#
# Main program
#
def _main():
	args = parse_args()
	csv = prod_csv(args.model, xml_dir=args.dsns, out_file=args.out,
	    start_sn=args.serial)
	if args.config:
		try:
			csv.config_read(args.config)
		except FileNotFoundError:
			print('file', args.config, 'not found')
			return
		except CONF_NAME_NOT_MAPPED:
			return
		except CONF_NAME_TOO_LONG:
			return
	csv.gen_config_csv(args.csv)
	count = csv.gen()
	print('success: wrote', count, 'device ids to', args.out)

if __name__ == "__main__":
	_main()

# end
