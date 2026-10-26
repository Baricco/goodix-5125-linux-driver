#!/usr/bin/env python3
"""Python port of the PSK pairing algorithm by grold/sorguido (goodix_pairing_crypto.c, LGPL-2.1).

Constants published by the open source driver, qualified for APP12509. Used here to:
 - check whether they carry over to firmware APP12508 (AES-GCM tag on a captured envelope);
 - build a 0xbb010003 envelope from a PSK of our choice (for the Linux driver).

It reconstructs nothing of the firmware: it only runs math that is already public.
"""

import hashlib
import hmac
import struct
import sys

from Crypto.Cipher import AES

KDF_KEY = bytes.fromhex(
    "5cba6e25819518de2d53e96dc0347ab0d427d4084bda4fae1bff2b09112a57e5")
AAD = bytes.fromhex("522dc1f099567d07f47f37a32a84427d")
HEADER = bytes.fromhex("02ff20000000")
LABEL = b"kgoodwixg\x00kaelrgnoerlithm"
LENGTH_BE = bytes.fromhex("00000180")
WORD_THREE = bytes.fromhex("03000000")


def derived_key_material():
    """48 bytes: AES key = [:32], outer HMAC key = [16:48]. Independent of the PSK."""
    out = b""
    for rnd in (1, 2):
        counter = struct.pack(">I", rnd)
        out += hmac.new(KDF_KEY, counter + LABEL + LENGTH_BE, hashlib.sha256).digest()
    return out[:48]


def build_envelope(psk):
    """Builds the 0xbb010003 envelope (102 bytes) and the 0xbb020003 validator (SHA256) for a PSK."""
    assert len(psk) == 32
    derived = derived_key_material()
    aes_key, mac_key = derived[:32], derived[16:48]
    nonce = hashlib.sha256(HEADER + psk[:8] + WORD_THREE * 16).digest()[:16]
    cipher = AES.new(aes_key, AES.MODE_GCM, nonce=nonce, mac_len=16)
    cipher.update(AAD)
    ct, tag = cipher.encrypt_and_digest(psk)
    outer = hmac.new(mac_key, HEADER + ct + tag, hashlib.sha256).digest()
    env = outer + HEADER + nonce + ct + tag
    assert len(env) == 102
    return env, hashlib.sha256(env).digest()


def inspect(env):
    """Splits a captured envelope."""
    return dict(outer=env[:32], header=env[32:38], nonce=env[38:54],
                ct=env[54:86], tag=env[86:102])


def verify_capture(env):
    """Checks against a captured envelope: GCM tag (correct key) + outer HMAC + validator."""
    parts = inspect(env)
    derived = derived_key_material()
    aes_key, mac_key = derived[:32], derived[16:48]
    # 1) outer HMAC: depends on no further secret, only on derived (public)
    outer = hmac.new(mac_key, parts["header"] + parts["ct"] + parts["tag"], hashlib.sha256).digest()
    hmac_ok = hmac.compare_digest(outer, parts["outer"])
    # 2) decrypt with the GCM tag: if it authenticates, the AES key (hence KDF_KEY) is valid for this firmware
    cipher = AES.new(aes_key, AES.MODE_GCM, nonce=parts["nonce"], mac_len=16)
    cipher.update(AAD)
    try:
        psk = cipher.decrypt_and_verify(parts["ct"], parts["tag"])
        gcm_ok = True
    except ValueError:
        psk, gcm_ok = None, False
    return hmac_ok, gcm_ok, psk


if __name__ == "__main__":
    env = open(sys.argv[1], "rb").read()
    hmac_ok, gcm_ok, psk = verify_capture(env)
    print(f"HMAC esterno: {'OK' if hmac_ok else 'NO'}")
    print(f"AES-GCM tag:  {'OK (grold constants valid for this firmware)' if gcm_ok else 'NO'}")
    if len(sys.argv) > 2 and sys.argv[2] == "--validator":
        print(f"validator SHA256(envelope) = {hashlib.sha256(env).hexdigest()}")
    if gcm_ok:
        print(f"PSK recovered: {len(psk)} bytes")
