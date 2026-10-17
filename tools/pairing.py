#!/usr/bin/env python3
"""Porting Python dell'algoritmo di pairing PSK di grold/sorguido (goodix_pairing_crypto.c, LGPL-2.1).

Costanti pubblicate dal driver open source, qualificate per APP12509. Qui le si usa per:
 - verificare se trasferiscono al firmware APP12508 (tag AES-GCM su una busta catturata);
 - costruire una busta 0xbb010003 da una PSK a nostra scelta (per il driver Linux).

Non ricostruisce nulla del firmware: esegue solo matematica gia' pubblica.
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
    """48 byte: AES key = [:32], chiave HMAC esterna = [16:48]. Indipendente dalla PSK."""
    out = b""
    for rnd in (1, 2):
        counter = struct.pack(">I", rnd)
        out += hmac.new(KDF_KEY, counter + LABEL + LENGTH_BE, hashlib.sha256).digest()
    return out[:48]


def build_envelope(psk):
    """Costruisce la busta 0xbb010003 (102 byte) e il validator 0xbb020003 (SHA256) per una PSK."""
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
    """Scompone una busta catturata."""
    return dict(outer=env[:32], header=env[32:38], nonce=env[38:54],
                ct=env[54:86], tag=env[86:102])


def verify_capture(env):
    """Verifica contro una busta catturata: tag GCM (chiave corretta) + HMAC esterno + validator."""
    parts = inspect(env)
    derived = derived_key_material()
    aes_key, mac_key = derived[:32], derived[16:48]
    # 1) HMAC esterno: non dipende da segreti ulteriori, solo da derived (pubblico)
    outer = hmac.new(mac_key, parts["header"] + parts["ct"] + parts["tag"], hashlib.sha256).digest()
    hmac_ok = hmac.compare_digest(outer, parts["outer"])
    # 2) decifra con tag GCM: se autentica, la chiave AES (quindi KDF_KEY) vale per questo firmware
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
    print(f"tag AES-GCM:  {'OK (costanti grold valide per questo firmware)' if gcm_ok else 'NO'}")
    if len(sys.argv) > 2 and sys.argv[2] == "--validator":
        print(f"validator SHA256(busta) = {hashlib.sha256(env).hexdigest()}")
    if gcm_ok:
        print(f"PSK recuperata: {len(psk)} byte")
