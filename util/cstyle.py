#! /usr/bin/env python3
# coding: utf-8

#
# Copyright 2021 Ayla Networks, Inc.  All rights reserved.
#
'''
Check cstyle of files

Runs cstyle on the files supplied as arguments.

Optionally it refers to a cached list previously-cstyled files and skips those
which have been previously cstyled.
'''

import os
import sys
import argparse
import logging
import subprocess
from pathlib import Path, PurePath

log = logging

def _check_file(source, ref_dir=None, cmd='cstyle_ayla'):
	if not os.path.exists(source):
		log.error(f'no such file: {source:s}')
		return False

	ref = None
	if ref_dir:
		ref = os.path.join(ref_dir, source + '.cs')
		path = Path(ref).parent
		if not path.is_dir():
			log.debug(f'mkdir {str(path):s}')
			path.mkdir(parents=True)
		if os.path.exists(ref) and \
		    os.path.getmtime(ref) > os.path.getmtime(source):
			return True

	log.debug(f'file {source:s} needs checking')
	argv = [cmd, source]
	try:
		p = subprocess.Popen(argv)
	except:
		log.error(f'cstyle popen failed: {str(argv):s}')
		return False
	p.communicate()
	status = p.poll()
	if status:
		log.error(f'cstyle {source:s} exited status {status:d}')
		return False
	if ref:
		try:
			Path(ref).touch()
		except:
			log.error(f'touch ${ref:s} failed')
			return False

	return True

def _parse_args():
	parser = argparse.ArgumentParser(description = 'cstyle')
	parser.add_argument('file', nargs='+',
		help='files to check')
	parser.add_argument('--cmd', required=False,
		default='cstyle_ayla')
	parser.add_argument('--cache', required=False,
		default='.',
		help='directory used to determine files already checked')
	parser.add_argument('-d', '--debug', required=False,
		default=False, action='store_true',
		help='enable debug logging')
	args = parser.parse_args()

	name = os.path.basename(sys.argv[0])
	if name.endswith('.py'):
		name = name[:-3]
	level = logging.INFO
	if args.debug:
		level = logging.DEBUG
	log.basicConfig(level=level,
		format=name + ': %(levelname)s: %(message)s')
	return args

def _main():
	args = _parse_args()
	errors = 0

	for f in args.file:
		log.debug(f'file {f:s}')
		if not _check_file(f, ref_dir=args.cache, cmd=args.cmd):
			errors += 1
	if errors:
		log.error(f'{errors:d} files had style issues')
		sys.exit(1)

if __name__ == '__main__':
	_main()
