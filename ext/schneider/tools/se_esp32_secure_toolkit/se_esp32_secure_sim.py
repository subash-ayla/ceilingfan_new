''' Change log:
# Date: 2020/09/08
# Change(s): Integrate secure boot V2 and secure upgrade simulation into one.
# Author: tony.zhao@se.com
'''

from __future__ import division, print_function
from se_esp32_sbv2 import FatalError
import se_esp32_sbv2 as sbv2
import se_esp32_su as su
import argparse
import os


### Constants. Do NOT modify unless you really know.
FILES_PATH = sbv2.FILES_PATH
SIGNED_BIN_SUFFIX = sbv2.SIGNED_BIN_SUFFIX


def secure_sim(args):
    print("Executing secure_sim...")
    sbv2.sign_pki_sim(args)
    
    args.sbv2Fw = None
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        """ Delete temp files. """
        if SIGNED_BIN_SUFFIX in name and not "bootloader" in name:
            args.sbv2Fw = file_path
            break

    if args.sbv2Fw == None:
        raise FatalError("Failure in secure boot V2!")

    su.sign_ota_pki_sim(args)


def main():
    parser = argparse.ArgumentParser(description='se_espsecure.py - ESP32 Secure Upgrade tool for SE PKI')

    subparsers = parser.add_subparsers(
        dest='operation',
        help='Run espsecure.py {command} -h for additional help')
        
    # Command: secure_sim
    p = subparsers.add_parser('secure_sim',
                              help='Generate raw signature by simulating SE PKI procedures.')
    p.add_argument('--btlbin',
                   help="Unsigned bootloader raw (binary) image.", required=True)
    p.add_argument('--fwbin', 
                   help="Unsigned firmware raw (binary) image.", required=True)

    args = parser.parse_args()
    if args.operation is None:
        parser.print_help()
        parser.exit(1)

    # each 'operation' is a module-level function of the same name
    operation_func = globals()[args.operation]
    operation_func(args)


def _main():
    try:
        main()
        print("Done")
    except FatalError as e:
        print('\nA fatal error occurred: %s' % e)
        sys.exit(2)


if __name__ == '__main__':
    _main()
