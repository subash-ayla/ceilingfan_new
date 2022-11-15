#!/usr/bin/env python
# ESP32 secure boot utility
# https://github.com/themadinventor/esptool
#
# Copyright (C) 2016 Espressif Systems (Shanghai) PTE LTD
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
# Street, Fifth Floor, Boston, MA 02110-1301 USA.

''' Change log:
# Date: 2020/09/07
# Change(s): Implemented utility methods for secured upgrade.
# Author: tony.zhao@se.com
'''

from __future__ import division, print_function
import os
import sys
import zlib
import shutil
import struct
import hashlib
import argparse
import subprocess
import se_esp32_sbv2 as sbv2

from se_esp32_sbv2 import FatalError
from se_esp32_sbv2 import FB_Args
from collections import namedtuple
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils
from cryptography.utils import int_to_bytes
from cryptography import exceptions
from Crypto.Signature import pss
from Crypto.Hash import SHA256
from Crypto.PublicKey import RSA


### Constants. Do NOT modify unless you really know.
OTA_SIGNING_KEY_DIR = './PKI_secrets/su_signing_key.pem'
ESP32_OTA_CER_DIR = './PKI_secrets/ESP32OTASig.cer'
PKI_SECRETS_PATH = sbv2.PKI_SECRETS_PATH
PKI_SIGS_PATH = sbv2.PKI_SIGS_PATH
TEMP_CER_PEM_SUFFIX = sbv2.TEMP_CER_PEM_SUFFIX
FILES_PATH = sbv2.FILES_PATH
SIG_BLK_SUFFIX = sbv2.SIG_BLK_SUFFIX
DEFAULT_BIN_FORMAT = sbv2.DEFAULT_BIN_FORMAT
SIG_BLOCK_SIZE = sbv2.SIG_BLOCK_SIZE
SECTOR_SIZE = sbv2.SECTOR_SIZE
SIGNED_BIN_SUFFIX = '-esp32signed_ota.bin'
OTA_SUFFIX = '_ota.bin'
SIG_BLK_HASH_SIZE = 32


### Static methods.
def _check_alignment(contents):
    """ SBv2 signed firmware should be 4K aligned. """
    if len(contents) % sbv2.SECTOR_SIZE != 0:
        return False
    else:
        return True


def _sig_blk_assembler(sig_path, bin_path, cert_path):
    """ Modified from sign_secure_boot_v2() """
    with open(bin_path, "rb") as fd:
        bin_contents = fd.read()

    if _check_alignment(bin_contents):
        aligned_contents = bin_contents
    else:
        raise FatalError("Sbv2 binary is not 4KB aligned! Unknown binary...")

    # Calculate digest of data file
    digest = hashlib.sha256()
    digest.update(aligned_contents)
    digest = digest.digest()

    # Extract public info for signature block.
    public_key = sbv2._extract_pubkey_from_x509_cert(cert_path)
    rsa_primitives = sbv2._get_sbv2_rsa_primitives(public_key)

    # Read from an signature in binary.
    with open(sig_path, "rb") as fd:
        sig_contents = fd.read()
    signature = sig_contents

    # Encode in signature block format
    #
    # Note: the [::-1] is to byte swap all of the bignum
    # values (signatures, coefficients) to little endian
    # for use with the RSA peripheral, rather than big endian
    # which is conventionally used for RSA.
    signature_block = struct.pack("<BBxx32s384sI384sI384s",
                                    0xe7,  # magic byte
                                    0x02,  # version
                                    digest,
                                    int_to_bytes(rsa_primitives.n)[::-1],
                                    rsa_primitives.e,
                                    int_to_bytes(rsa_primitives.rinv)[::-1],
                                    rsa_primitives.m & 0xFFFFFFFF,
                                    signature[::-1])

    signature_block += struct.pack("<I", zlib.crc32(signature_block) & 0xffffffff)
    signature_block += b'\x00' * 16   # padding

    assert len(signature_block) == 1216

    # Get hash of the signature block.
    digest_sig_blk = hashlib.sha256()
    digest_sig_blk.update(signature_block)
    #print(digest_sig_blk.hexdigest())
    digest_sig_blk = digest_sig_blk.digest()
    ota_sig = digest_sig_blk + signature_block

    # Sig block: no padding to the 4K sector. Write to sig_blk file
    with open(bin_path + SIG_BLK_SUFFIX, "wb") as fd:
        fd.write(ota_sig)


def _signed_bin_packer(sig_blk_path, bin_path):
    with open(bin_path, "rb") as fd:
        bin_contents = fd.read()

    if _check_alignment(bin_contents):
        aligned_contents = bin_contents
    else:
        raise FatalError("Sbv2 binary is not 4KB aligned! Unknown binary...")
    
    with open(sig_blk_path, "rb") as fd:
        sig_blk_contents = fd.read()

    assert('.bin' in bin_path)
    # Rename the suffix .bin to _ota.bin. E.g., xxx-esp32signed.bin is renamed to xxx-esp32signed_ota.bin.
    temp = bin_path[:-4]
    ota_path = temp + OTA_SUFFIX

    # Sig block: no padding to the 4K sector. Write to sig_blk file
    with open(ota_path, "wb") as fd:
        fd.write(aligned_contents + sig_blk_contents)


def _housekeeping():
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        """ Delete temp files. """
        if SIG_BLK_SUFFIX in name:
            os.unlink(file_path)

    dir = os.listdir(PKI_SECRETS_PATH)
    for name in dir:
        file_path = os.path.join(PKI_SECRETS_PATH, name)
        """ Delete temp pem files. """
        if TEMP_CER_PEM_SUFFIX in name:
            os.unlink(file_path)


### Extern methods.
def pki_simulator(args):
    sbv2.pki_simulator(args)


def sign_ota_pki_se(args):
    print("Executing sign_ota_pki_se...")
    """ Check signature and binary come in pair. """    
    if args.otaFwSig and args.sbv2Fw:
        _sig_blk_assembler(args.otaFwSig, args.sbv2Fw, args.cert)
        _signed_bin_packer(args.sbv2Fw + SIG_BLK_SUFFIX, args.sbv2Fw)
    elif not args.otaFwSig and not args.sbv2Fw:
        print("Failed to detect firmware signature and sbv2 signed firmware.")
    else:
        raise FatalError("Failed to pair firmware signature and sbv2 signed firmware.")

    # Post-action: housekeeping
    _housekeeping()


def verify_signed_ota_bin(args):
    print("Executing verify_signed_ota_bin...")
    """ Modified from verify_signature_v2() """
    pubkey = sbv2._extract_pubkey_from_x509_cert(args.cert)

    for file in args.files:
        contents = None
        with open(file, "rb") as fd:
            binary_content = fd.read() 

        """ Check the total signed OTA firmware size. """
        remainder = len(binary_content) % SECTOR_SIZE
        if remainder != SIG_BLK_HASH_SIZE + SIG_BLOCK_SIZE:
            raise FatalError("Invalid format of signed OTA firmware!")

        """ Get the hash of the image excluding signature block. """
        digest = hashlib.sha256()
        digest.update(binary_content[:-(SIG_BLOCK_SIZE + SIG_BLK_HASH_SIZE)])
        digest = digest.digest()

        """ Get the hash of the signature block """
        sig_block = binary_content[-SIG_BLOCK_SIZE:]
        digest_sig_blk = hashlib.sha256()
        digest_sig_blk.update(sig_block)
        #print(digest_sig_blk.hexdigest())
        digest_sig_blk = digest_sig_blk.digest()

        """ Get the hash padding in the image. """
        hash_sig_blk = binary_content[-(SIG_BLOCK_SIZE + SIG_BLK_HASH_SIZE):-SIG_BLOCK_SIZE]

        if not (digest_sig_blk == hash_sig_blk):
            raise FatalError("OTAU signature hash is invalid!")

        for sig_blk_num in range(1):
            sig_blk = binary_content[-SIG_BLOCK_SIZE:]
            assert(len(sig_blk) == SIG_BLOCK_SIZE)

            sig_data = struct.unpack("<BBxx32s384sI384sI384sI16x", sig_blk)
            crc = zlib.crc32(sig_blk[:1196])

            if sig_data[0] != 0xe7:
                print("Signature block has invalid magic byte %d. " 
                      "Expected 0xe7 (231)." % sig_data[0])
                raise FatalError("Signature block has invalid magic byte %d. " 
                                 "Expected 0xe7 (231)." % sig_data[0])
            if sig_data[1] != 0x02:
                print("Signature block has invalid version %d. "
                      "This version  of espsecure only supports version 2." % sig_data[1])
                raise FatalError("Signature block has invalid version %d. "
                                 "This version  of espsecure only supports version 2." % sig_data[1])
            if sig_data[-1] != crc & 0xffffffff:
                print("Signature block crc does not match %d. Expected %d. " % (sig_data[-1], crc))
                raise FatalError("Signature block crc does not match %d. Expected %d. " % (sig_data[-1], crc))
            if sig_data[2] != digest:
                print("Signature block image digest does not match the actual image digest %s." % (digest))
                FatalError("Signature block image digest does not match the actual image digest %s. "
                           "Expected %s." % (digest, sig_data[2]))

            #print("Verifying %d bytes of data against %s ..." % (len(sig_data[-2]), file))
            try:
                pubkey.verify(
                    sig_data[-2][::-1],
                    digest,
                    padding.PSS(
                        mgf=padding.MGF1(hashes.SHA256()),
                        salt_length=32
                    ),
                    utils.Prehashed(hashes.SHA256())
                )
                print("Signature BLK%d verified against %s" % (sig_blk_num, file))
                continue
            except exceptions.InvalidSignature:
                print("Signature BLK %d is not signed by %s." % (sig_blk_num, file))
                continue


def sign_ota_pki_sim(args):
    print("Executing sign_ota_pki_sim...")
    unsigned_fw_name = os.path.basename(args.sbv2Fw)

    # Preparation: delete existing signed binaries
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        if SIGNED_BIN_SUFFIX in name:
            os.unlink(file_path)

    # FB: SE PKI (i.e., PKI simulator)
    fb_args = FB_Args()
    fb_args.sign_key = OTA_SIGNING_KEY_DIR
    fb_args.files = []
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        if sbv2.SIGNED_BIN_SUFFIX in name:
            fb_args.files.append(file_path)
    pki_simulator(fb_args)    

    # FB: OTA Signature Block Assembler & Signed Binary Packer (i.e., Sign standalone)
    fb_args = FB_Args()
    fb_args.cert = ESP32_OTA_CER_DIR
    fb_args.sbv2Fw = args.sbv2Fw
    dir = os.listdir(PKI_SIGS_PATH)
    for name in dir:
        file_path = os.path.join(PKI_SIGS_PATH, name)
        if unsigned_fw_name in name:
            fb_args.otaFwSig = file_path
    sign_ota_pki_se(fb_args)

    # Post-action: housekeeping
    _housekeeping()
    
    # Post-action: Verify the signed binary using ESP32OTASig.cer.
    verify_args = FB_Args()
    verify_args.cert = ESP32_OTA_CER_DIR
    verify_args.files = []
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        if SIGNED_BIN_SUFFIX in name:
            verify_args.files.append(file_path)
    verify_signed_ota_bin(verify_args)


def main():
    parser = argparse.ArgumentParser(description='se_espsecure.py - ESP32 Secure Upgrade tool for SE PKI')

    subparsers = parser.add_subparsers(
        dest='operation',
        help='Run espsecure.py {command} -h for additional help')
        
    # Command: pki_simulator
    p = subparsers.add_parser('pki_simulator',
                              help='Generate raw signature by simulating SE PKI procedures.')
    p.add_argument('--input', '-i', action='append',
                   help="Input formatted binary. Could be a sequence of sbv2 signed binaries followed "
                        "by --input/-i, e.g., -i xxx-esp32signed_ota.bin -i xxx-esp32signed_ota.bin", 
                   dest='files', default=[], required=True)
    p.add_argument('--sign_key',
                   help="Path to signing key PEM file.", required=True)

    # Command: sign_ota_pki_se
    p = subparsers.add_parser('sign_ota_pki_se',
                              help='Create ESP32 OTA signed binary from sbv2 signed firmware by SE PKI.')
    p.add_argument('--cert',
                   help="CER file including the public key.", required=True)
    p.add_argument('--otaFwSig', 
                   help="Firmware raw (binary) signature. Has to be the signature of sbv2 signed firmware binary.")
    p.add_argument('--sbv2Fw', 
                   help="Sbv2 signed firmware binary. Has to be in pair with the given raw signature.")

    # Command: sign_ota_pki_sim
    p = subparsers.add_parser('sign_ota_pki_sim',
                              help='Create ESP32 signed binary from unsigned binary with PKI simulation. '
                                   'Verification is conducted at last using ESP32OTASig.cer.')
    p.add_argument('--sbv2Fw', 
                   help="ESP32 (secure boot) signed firmware.", required=True)

    # Command: verify_signed_ota_bin
    p = subparsers.add_parser('verify_signed_ota_bin',
                              help='Verify ESP32 signed OTA binary using a given .cer file')
    p.add_argument('--cert',
                   help="CER file including the public key.", required=True)
    p.add_argument('--input', '-i', action='append',
                   help="Input ESP32 signed binary. Could be a sequence of binaries followed "
                        "by --input/-i, e.g., -i xxx-esp32signed_ota.bin -i xxx-esp32signed_ota.bin", 
                   dest='files', default=[], required=True)

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
