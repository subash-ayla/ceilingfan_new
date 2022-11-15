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
# Date: 2020/08/11
# Change(s): Modified the original scripts to work with SE PKI policies.
#            Remove irrelevant methods from espsecure.py.
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
SIGNING_KEY_DIR = './PKI_secrets/sbv2_signing_key.pem'
ESP32_CER_DIR = './PKI_secrets/ESP32Sig.cer'
PKI_SECRETS_PATH = './PKI_secrets/'
PKI_SIGS_PATH = './PKI_sigs/'
FILES_PATH = './Files/'
FORMATTED_BIN_SUFFIX = "-formatted.bin"
SIGNED_BIN_SUFFIX = "-esp32signed.bin"
SIG_BLK_SUFFIX = ".sigblk"
DEFAULT_BIN_FORMAT = ".bin"
TEMP_CER_PEM_SUFFIX = ".cer.pem"
SECTOR_SIZE = 4096
SIG_BLOCK_SIZE = 1216


### Self-defined FatalError to replace esptool.FatalError.
class FatalError(Exception):
    def __init___(self, error_msg):
        Exception.__init__(self, "Fatal error as " + str(error_msg))


### Self-defined FB attributes.
class FB_Args(object):
    def __init__(self):
        pass 


### Static methods.
def _extract_pubkey_from_pem(pem_path):
    with open(pem_path, "rb") as fd:
        pem_contents = fd.read()

    pubkey = serialization.load_pem_public_key(pem_contents, backend=default_backend())
    if not isinstance(pubkey, rsa.RSAPublicKey):
        raise FatalError("Public key incorrect. Secure boot v2 requires RSA 3072 public key")
    if pubkey.key_size != 3072:
        raise FatalError("Key file has length %d bits. Secure boot v2 only supports RSA-3072." % pubkey.key_size)
    return pubkey


def _extract_pubkey_from_x509_cert(cert_path):
    # Only allow DER encoded file.
    try:
        cmd = "openssl x509 -inform DER -in {} -pubkey -noout".format(cert_path, cert_path)
        process = subprocess.Popen(cmd.split(), stdout=subprocess.PIPE)
        output, error = process.communicate()
        if error:
            print(error)
            raise FatalError()
        with open(cert_path + '.pem', "wb") as fd:
            fd.write(output)
        return _extract_pubkey_from_pem(cert_path + '.pem')
    except Exception:
        raise FatalError("Failed to load certificate.")


def _get_sbv2_rsa_primitives(public_key):
    primitives = namedtuple('primitives', ['n', 'e', 'm', 'rinv'])
    numbers = public_key.public_numbers()
    primitives.n = numbers.n  #
    primitives.e = numbers.e  # two public key components

    # Note: this cheats and calls a private 'rsa' method to get the modular
    # inverse calculation.
    primitives.m = - rsa._modinv(primitives.n, 1 << 32)

    rr = 1 << (public_key.key_size * 2)
    primitives.rinv = rr % primitives.n
    return primitives


def _sector_boundary_align(contents):
    if len(contents) % SECTOR_SIZE != 0:
        pad_by = SECTOR_SIZE - (len(contents) % SECTOR_SIZE)
        #print("Padding data contents by %d bytes so signature sector aligns at sector boundary" % pad_by)
        contents += b'\xff' * pad_by
    return contents


def _sig_blk_assembler(sig_path, bin_path, cert_path):
    """ Modified from sign_secure_boot_v2() """
    with open(bin_path, "rb") as fd:
        bin_contents = fd.read()

    aligned_contents = _sector_boundary_align(bin_contents)

    # Calculate digest of data file
    digest = hashlib.sha256()
    digest.update(aligned_contents)
    digest = digest.digest()

    # Extract public info for signature block.
    public_key = _extract_pubkey_from_x509_cert(cert_path)
    rsa_primitives = _get_sbv2_rsa_primitives(public_key)

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
    signature_sector = b""
    signature_sector += signature_block
    assert len(signature_sector) > 0
    # Pad signature_sector to sector
    signature_sector = signature_sector + \
        (b'\xff' * (SECTOR_SIZE - len(signature_sector)))
    assert len(signature_sector) == SECTOR_SIZE
    # Write to sig_blk file
    with open(bin_path + SIG_BLK_SUFFIX, "wb") as fd:
        fd.write(signature_sector)
    

def _signed_bin_packer(sig_blk_path, bin_path):
    with open(bin_path, "rb") as fd:
        bin_contents = fd.read()

    aligned_contents = _sector_boundary_align(bin_contents)
    
    with open(sig_blk_path, "rb") as fd:
        sig_blk_contents = fd.read()

    with open(bin_path + SIGNED_BIN_SUFFIX, "wb") as fd:
        fd.write(aligned_contents + sig_blk_contents)


def _housekeeping():
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        """ Delete temp files. Beautify signed file. """
        if FORMATTED_BIN_SUFFIX in name or SIG_BLK_SUFFIX in name:
            os.unlink(file_path)
        elif SIGNED_BIN_SUFFIX in name and name.count(DEFAULT_BIN_FORMAT) >= 2:
            new_name = name.replace(DEFAULT_BIN_FORMAT, "", name.count(DEFAULT_BIN_FORMAT) - 1)
            if "-unsigned" in new_name:
                final_name = new_name.replace("-unsigned", "")
            else:
                final_name = new_name
            os.rename(file_path, FILES_PATH + final_name)

    dir = os.listdir(PKI_SECRETS_PATH)
    for name in dir:
        file_path = os.path.join(PKI_SECRETS_PATH, name)
        """ Delete temp pem files. """
        if TEMP_CER_PEM_SUFFIX in name:
            os.unlink(file_path)


### Extern methods.
def formatter(args):
    print("Executing formatter...")
    files = args.files

    for file in files:
        contents = None
        with open(file, "rb") as fd:
            contents = fd.read()
        aligned_contents = _sector_boundary_align(contents)     
        with open(file + FORMATTED_BIN_SUFFIX, "wb") as fd:
            fd.write(aligned_contents)


def pki_simulator(args):
    print("Executing pki_simulator...")

    # Remove files under PKI_SIGS_PATH
    shutil.rmtree(PKI_SIGS_PATH)
    os.mkdir(PKI_SIGS_PATH)

    # Sign the formatted binary.
    try:
        with open(args.sign_key, "rb") as fd:
            sign_key_contents = fd.read()
        key = RSA.importKey(sign_key_contents)
        if not key.has_private() or not key.can_sign():
            print("Invalid sign_key.")
            raise FatalError()
    except Exception:
        raise FatalError("Failed to load sign_key file.")
    
    files = args.files

    for file in files:
        contents = None
        with open(file, "rb") as fd:
            contents = fd.read()
        hash = SHA256.new(contents)
        sig_raw = pss.new(key).sign(hash)
        with open(file + ".sigraw", "wb") as fd:
            fd.write(sig_raw)
        # Move to the PKI_sig directory.
        try:
            shutil.move(file + ".sigraw", PKI_SIGS_PATH)
        except Exception:
            pass


def sign_pki_se(args):
    print("Executing sign_pki_se...")
    """ Check signature and binary come in pair. """
    if args.btlsig and args.btlbin:
        _sig_blk_assembler(args.btlsig, args.btlbin, args.cert)
        _signed_bin_packer(args.btlbin + SIG_BLK_SUFFIX, args.btlbin)
    elif not args.btlsig and not args.btlbin:
        print("Failed to detect bootloader signature and binary files.")
    else:
        raise FatalError("Failed to pair bootloader signature and binary files.")
    
    if args.fwsig and args.fwbin:
        _sig_blk_assembler(args.fwsig, args.fwbin, args.cert)
        _signed_bin_packer(args.fwbin + SIG_BLK_SUFFIX, args.fwbin)
    elif not args.fwsig and not args.fwbin:
        print("Failed to detect firmware signature and binary files.")
    else:
        raise FatalError("Failed to pair firmware signature and binary files.")

    # Post-action: housekeeping
    _housekeeping()


def verify_signed_bin(args):
    print("Executing verify_signed_bin...")
    """ Modified from verify_signature_v2() """
    pubkey = _extract_pubkey_from_x509_cert(args.cert)

    for file in args.files:
        contents = None
        with open(file, "rb") as fd:
            binary_content = fd.read() 

        assert(len(binary_content) % SECTOR_SIZE == 0)
        digest = hashlib.sha256()
        digest.update(binary_content[:-SECTOR_SIZE])
        digest = digest.digest()

        for sig_blk_num in range(1):
            offset = -SECTOR_SIZE + sig_blk_num * SIG_BLOCK_SIZE
            sig_blk = binary_content[offset: offset + SIG_BLOCK_SIZE]
            assert(len(sig_blk) == SIG_BLOCK_SIZE)

            sig_data = struct.unpack("<BBxx32s384sI384sI384sI16x", sig_blk)
            crc = zlib.crc32(sig_blk[:1196])

            if sig_data[0] != 0xe7:
                print("Signature BLK %d is not signed by %s." % (sig_blk_num, file))
                raise FatalError("Signature block has invalid magic byte %d. Expected 0xe7 (231)." % sig_data[0])
            if sig_data[1] != 0x02:
                print("Signature BLK %d is not signed by %s." % (sig_blk_num, file))
                raise FatalError("Signature block has invalid version %d. This version  of espsecure only supports version 2." % sig_data[1])
            if sig_data[-1] != crc & 0xffffffff:
                print("Signature BLK %d is not signed by %s." % (sig_blk_num, file))
                raise FatalError("Signature block crc does not match %d. Expected %d. " % (sig_data[-1], crc))
            if sig_data[2] != digest:
                print("Signature BLK %d is not signed by %s." % (sig_blk_num, file))
                FatalError("Signature block image digest does not match the actual image digest %s. Expected %s." % (digest, sig_data[2]))

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


def sign_pki_sim(args):
    print("Executing sign_pki_sim...")
    unsigned_btl_name = os.path.basename(args.btlbin)
    unsigned_fw_name = os.path.basename(args.fwbin)

    # Preparation: delete existing signed binaries
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        if SIGNED_BIN_SUFFIX in name:
            os.unlink(file_path)

    # FB: Formatter
    fb_args = FB_Args()
    fb_args.files = []
    fb_args.files.append(args.btlbin)
    fb_args.files.append(args.fwbin)
    formatter(fb_args)

    # FB: SE PKI (i.e., PKI simulator)
    fb_args = FB_Args()
    fb_args.sign_key = SIGNING_KEY_DIR
    fb_args.files = []
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        if FORMATTED_BIN_SUFFIX in name:
            fb_args.files.append(file_path)
    pki_simulator(fb_args)    

    # FB: Signature Block Assembler & Signed Binary Packer (i.e., Sign standalone)
    fb_args = FB_Args()
    fb_args.cert = ESP32_CER_DIR
    fb_args.btlbin = args.btlbin
    fb_args.fwbin = args.fwbin
    dir = os.listdir(PKI_SIGS_PATH)
    for name in dir:
        file_path = os.path.join(PKI_SIGS_PATH, name)
        if unsigned_btl_name in name:
            fb_args.btlsig = file_path
        elif unsigned_fw_name in name:
            fb_args.fwsig = file_path
    sign_pki_se(fb_args)

    # Post-action: housekeeping
    _housekeeping()
    
    # Post-action: Verify the signed binary using ESP32Sig.cer.
    verify_args = FB_Args()
    verify_args.cert = ESP32_CER_DIR
    verify_args.files = []
    dir = os.listdir(FILES_PATH)
    for name in dir:
        file_path = os.path.join(FILES_PATH, name)
        if SIGNED_BIN_SUFFIX in name:
            verify_args.files.append(file_path)
    verify_signed_bin(verify_args)


def main():
    parser = argparse.ArgumentParser(description='se_esp32_sbv2.py - ESP32 Secure Boot v2 tool for SE PKI')

    subparsers = parser.add_subparsers(
        dest='operation',
        help='Run espsecure.py {command} -h for additional help')

    # Command: formatter
    p = subparsers.add_parser('formatter',
                              help='Ensure the signature is placed at boundary sector.')
    p.add_argument('--input', '-i', action='append',
                   help="Input binary. Could be a sequence of binaries followed by --input/-i, e.g., -i firmware.bin -i bootloader.bin", 
                   dest='files', default=[], required=True)
        
    # Command: pki_simulator
    p = subparsers.add_parser('pki_simulator',
                              help='Generate raw signature by simulating SE PKI procedures.')
    p.add_argument('--input', '-i', action='append',
                   help="Input formatted binary. Could be a sequence of binaries followed "
                        "by --input/-i, e.g., -i firmware-formatted.bin -i bootloader-formatted.bin", 
                   dest='files', default=[], required=True)
    p.add_argument('--sign_key',
                   help="Path to signing key PEM file.", required=True)

    # Command: sign_pki_se
    p = subparsers.add_parser('sign_pki_se',
                              help='Create ESP32 signed binary from unsigned binary and signature by SE PKI.')
    p.add_argument('--cert',
                   help="CER file including the public key.", required=True)
    p.add_argument('--btlsig', 
                   help="Bootloader raw (binary) signature. Has to be specified with bootloader binary.")
    p.add_argument('--fwsig', 
                   help="Firmware raw (binary) signature. Has to be specified with firmware binary.")
    p.add_argument('--btlbin', 
                   help="Unsigned bootloader binary. Has to be specified with bootloader raw signature.")
    p.add_argument('--fwbin', 
                   help="Unsigned firmware binary. Has to be specified with firmware raw signature.")

    # Command: sign_pki_sim
    p = subparsers.add_parser('sign_pki_sim',
                              help='Create ESP32 signed binary from unsigned binary with PKI simulation. '
                                   'Verification is conducted at last using ESP32Sig.cer.')
    p.add_argument('--btlbin',
                   help="Unsigned bootloader raw (binary) image.", required=True)
    p.add_argument('--fwbin', 
                   help="Unsigned firmware raw (binary) image.", required=True)

    # Command: verify_signed_bin
    p = subparsers.add_parser('verify_signed_bin',
                              help='Verify ESP32 signed binary using a given .cer file')
    p.add_argument('--cert',
                   help="CER file including the public key.", required=True)
    p.add_argument('--input', '-i', action='append',
                   help="Input ESP32 signed binary. Could be a sequence of binaries followed "
                        "by --input/-i, e.g., -i firmware-esp32signed.bin -i bootloader-esp32signed.bin", 
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
