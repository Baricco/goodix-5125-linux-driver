#!/usr/bin/env python3
"""Prototipo driver Linux per Goodix 27c6:5125 (firmware GF_ST411SEC_APP_12508).

Provisioning PSK autonomo (come i driver open source per questo chip): genera una PSK
nostra, la scrive nel sensore con un solo comando E0 preservando il blob bb010002
esistente, poi stabilisce la sessione TLS-PSK e acquisisce immagini grezze.

Riusa il trasporto USB e il framing A0 di goodix-fp-dump (goodix.py, protocol.py).
L'algoritmo della busta PSK e' quello pubblicato da grold/sorguido (tools/pairing.py),
verificato byte-per-byte contro il firmware 12508.

Sicurezza: la scrittura E0 tocca solo il settore 4 della flash (store PSK), e' reversibile
e non puo' bloccare il chip (vedi re/analisi-driver-windows.md). Richiede --write per
eseguire davvero la scrittura.

Uso:
  sudo .venv/bin/python driver_5125.py --probe                # sola lettura
  sudo .venv/bin/python driver_5125.py --write --capture      # provisioning + immagine
"""

import argparse
import hashlib
import os
import socket
import ssl
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "goodix-fp-dump"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))

import usb.core

import goodix
import protocol
import pairing

VENDOR = 0x27C6
PRODUCT = 0x5125
SENSOR_WIDTH = 80
SENSOR_HEIGHT = 64
TLS_IDENTITY = b"Client_identity"
PSK_STORE = os.path.join(os.path.dirname(__file__), "state", "linux_psk.bin")
WAIT_FOR_FINGER = False
CAPTURE_TAG = "fingerprint"
ENROLL_COUNT = 1

# Blocco config catturato dalla sessione del driver Windows (0x90), 224 byte.
CONFIG90 = open(os.path.join(os.path.dirname(__file__), "captures", "config90.bin"), "rb").read()


def log(msg):
    print(f"[5125] {msg}")


# ----------------------------------------------------------------------------- PSK

def load_or_create_psk():
    os.makedirs(os.path.dirname(PSK_STORE), exist_ok=True)
    if os.path.exists(PSK_STORE):
        psk = open(PSK_STORE, "rb").read()
        if len(psk) == 32:
            log(f"PSK esistente caricata da {PSK_STORE}")
            return psk
    psk = os.urandom(32)
    fd = os.open(PSK_STORE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.write(fd, psk)
    os.close(fd)
    log(f"PSK nuova generata e salvata in {PSK_STORE}")
    return psk


def read_bb010002(device):
    """Legge il blob bb010002 attuale dal sensore, da preservare nella scrittura E0."""
    ok, flags, value = device.preset_psk_read(0xBB010002)
    if not ok:
        return None
    return value


def provision(device, psk):
    """Scrive la PSK nel sensore con un solo E0: TLV bb010002 (preservato) + bb010003 (nostro).

    Layout del corpo E0 identico a grold e alla cattura: 8+332 + 8+102 = 450, pad a 452.
    """
    envelope, validator = pairing.build_envelope(psk)
    log(f"validator atteso (bb020003) = {validator.hex()}")

    existing = read_bb010002(device)
    if existing is None or len(existing) != 332:
        raise SystemError(f"bb010002 assente o lunghezza inattesa: {existing and len(existing)}")
    log(f"bb010002 esistente preservato ({len(existing)} byte)")

    body = struct.pack("<II", 0xBB010002, 332) + existing
    body += struct.pack("<II", 0xBB010003, 102) + envelope
    body += b"\x00" * ((-len(body)) % 4)
    assert len(body) == 452, len(body)

    # E0 grezzo (preset_psk_write incapsula un solo TLV, qui ne servono due): framing A0 diretto
    device.protocol.write(
        goodix.encode_message_pack(
            goodix.encode_message_protocol(body, goodix.COMMAND_PRESET_PSK_WRITE_R)))
    goodix.check_ack(
        goodix.check_message_protocol(
            goodix.check_message_pack(device.protocol.read()), goodix.COMMAND_ACK),
        goodix.COMMAND_PRESET_PSK_WRITE_R)
    resp = goodix.check_message_protocol(
        goodix.check_message_pack(device.protocol.read()), goodix.COMMAND_PRESET_PSK_WRITE_R)
    if not resp or resp[0] != 0x00:
        raise SystemError(f"E0 rifiutato dal sensore: {resp.hex() if resp else None}")
    log("E0 accettato, verifico validator sul sensore")

    ok, flags, got = device.preset_psk_read(0xBB020003)
    if not ok or got != validator:
        raise SystemError(f"validator non combacia: sensore={got.hex() if got else None}")
    log("validator confermato dal sensore: PSK attiva e nota a noi")


# --------------------------------------------------------------------- TLS bridge

class TlsPskServer:
    """Server TLS-PSK in-process. Fa da ponte tra i record TLS del sensore (canale B0)
    e un contesto SSL OpenSSL via memory BIO."""

    def __init__(self, psk):
        self.psk = psk
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.set_ciphers("PSK")
        self.ctx.set_psk_server_callback(self._cb, TLS_IDENTITY.decode())
        self.incoming = ssl.MemoryBIO()
        self.outgoing = ssl.MemoryBIO()
        self.sslobj = self.ctx.wrap_bio(self.incoming, self.outgoing, server_side=True)

    def _cb(self, identity):
        return self.psk

    def feed(self, data):
        self.incoming.write(data)

    def pull(self):
        return self.outgoing.read()

    def do_handshake_step(self):
        try:
            self.sslobj.do_handshake()
            return True
        except ssl.SSLWantReadError:
            return False

    def decrypt(self, data):
        self.incoming.write(data)
        out = b""
        while True:
            try:
                chunk = self.sslobj.read(4096)
            except ssl.SSLWantReadError:
                break
            if not chunk:
                break
            out += chunk
        return out


def _b0_write(device, data):
    device.protocol.write(
        goodix.encode_message_pack(data, goodix.FLAGS_TRANSPORT_LAYER_SECURITY))


def _b0_read(device):
    return goodix.check_message_pack(device.protocol.read(),
                                     goodix.FLAGS_TRANSPORT_LAYER_SECURITY)


def tls_handshake(device, server):
    """Handshake TLS-PSK sul canale B0. Flusso (dalla cattura):
    device: ClientHello -> host: ServerHello+Done -> device: CKE+CCS+Finished -> host: CCS+Finished.
    """
    client_hello = device.request_tls_connection()
    server.feed(client_hello)

    done = False
    for _ in range(24):
        done = server.do_handshake_step()   # consuma l'input, produce output se pronto
        out = server.pull()
        if out:
            _b0_write(device, out)           # ServerHello+Done, poi CCS+Finished
        if done:
            break
        # il server attende altri record dal device (CKE, CCS, Finished)
        try:
            server.feed(_b0_read(device))
        except usb.core.USBError:
            time.sleep(0.01)
    if not done:
        raise SystemError("handshake TLS-PSK non completato")
    log(f"handshake TLS-PSK completato lato host (cipher {server.sslobj.cipher()})")
    time.sleep(0.05)  # il device deve processare il nostro Finished (cfr. goodix-fp-dump)
    for attempt in range(3):
        try:
            device.tls_successfully_established()
            break
        except usb.core.USBError as e:
            log(f"tls_successfully_established ritento ({attempt + 1}): {type(e).__name__}")
            time.sleep(0.1)
    log("sessione TLS-PSK stabilita")


# ------------------------------------------------------------------- init + image

def decode_image(data):
    """Spacchetta l'immagine grezza: 4 pixel a 12 bit ogni 6 byte, poi rimappa dall'ordine
    di trasmissione (colonna-major) al raster 80x64. Mappatura dal decoder di grold.
    """
    n = SENSOR_WIDTH * SENSOR_HEIGHT
    samples = [0] * n
    for group in range(0, (n // 4) * 6, 6):
        c = data[group:group + 6]
        values = (((c[0] & 0xF) << 8) | c[1],
                  (c[3] << 4) | (c[0] >> 4),
                  ((c[5] & 0xF) << 8) | c[2],
                  (c[4] << 4) | (c[5] >> 4))
        wire_base = (group // 6) * 4
        for item in range(4):
            wire = wire_base + item
            raster = (wire % SENSOR_HEIGHT) * SENSOR_WIDTH + wire // SENSOR_HEIGHT
            samples[raster] = values[item]
    return samples


def write_pgm(pixels, path):
    """Salva come PGM 8 bit, scalando i valori 12 bit a 0-255."""
    with open(path, "w") as f:
        f.write(f"P2\n{SENSOR_WIDTH} {SENSOR_HEIGHT}\n255\n")
        for i, p in enumerate(pixels[:SENSOR_WIDTH * SENSOR_HEIGHT]):
            f.write(f"{(p >> 4) & 0xFF} ")
            if (i + 1) % SENSOR_WIDTH == 0:
                f.write("\n")


def init_and_capture(device, server):
    device.reset(True, False, 20)
    chip = device.read_sensor_register(0x0000, 4)
    log(f"chip id = {chip.hex()}")
    otp = device.read_otp()
    log(f"OTP = {otp[:16].hex()}...")
    device.reset(True, False, 20)
    device.upload_config_mcu(CONFIG90)
    device.set_powerdown_scan_frequency(100)

    tls_handshake(device, server)
    device.nop()
    device.query_mcu_state(b"\x55", True)

    device.mcu_switch_to_fdt_mode(
        b"\x0d\x01\xb2\xb2\xc2\xc2\xa7\xa7\xb6\xb6\xa6\xa6\xb6\xb6", True)
    device.nav()
    device.mcu_switch_to_fdt_mode(
        b"\x0d\x01\x80\xb0\x80\xc0\x80\xa4\x80\xb4\x80\xa3\x80\xb4", True)
    device.read_sensor_register(0x0082, 2)
    for addr, val in ((0x0220, b"\x78\x0b"), (0x0236, b"\xb9\x00"),
                      (0x0238, b"\xb8\x00"), (0x023a, b"\xb7\x00")):
        device.write_sensor_register(addr, val)

    _capture_frame(device, server, "clear-0")

    if WAIT_FOR_FINGER or ENROLL_COUNT > 1:
        n = ENROLL_COUNT if ENROLL_COUNT > 1 else 1
        for i in range(n):
            # arma il rilevamento dito (FDT)
            device.mcu_switch_to_fdt_mode(
                b"\x0d\x01\x80\xb1\x80\xc1\x80\xa6\x80\xb6\x80\xa5\x80\xb6", True)
            device.mcu_switch_to_fdt_down(
                b"\x0c\x01\x80\xb1\x80\xc1\x80\xa6\x80\xb6\x80\xa5\x80\xb6", True)
            device.nop()
            device.query_mcu_state(b"\x55", True)
            if n > 1:
                log(f">>> tocco {i + 1}/{n}: appoggia il dito (sposta un po' ogni volta) <<<")
            else:
                log(">>> appoggia il dito sul sensore (hai ~30s) <<<")
            # attende il contatto
            for attempt in range(10):
                try:
                    device.mcu_switch_to_fdt_down(
                        b"\x0c\x01\x80\xb2\x80\xc2\x80\xa7\x80\xb6\x80\xa6\x80\xb6", True)
                    break
                except usb.core.USBError:
                    pass
            tag = f"{CAPTURE_TAG}_{i:02d}" if n > 1 else CAPTURE_TAG
            _capture_frame(device, server, tag)
            if n > 1 and i < n - 1:
                # attende il sollevamento del dito prima del tocco successivo
                log("    solleva il dito")
                for attempt in range(10):
                    try:
                        device.mcu_switch_to_fdt_up(
                            b"\x0e\x01\x80\x90\x80\x9a\x80\x7b\x80\x95\x80\x8c\x80\xa3")
                        break
                    except usb.core.USBError:
                        pass


def _capture_frame(device, server, name):
    # mcu_get_image restituisce gia' il record TLS sbustato (payload B2)
    enc = device.mcu_get_image(b"\x01\x00", goodix.FLAGS_TRANSPORT_LAYER_SECURITY)
    # l'immagine puo' arrivare su piu' pacchetti B0: leggi finche' il TLS non produce output
    raw = bytes(enc)
    plain = server.decrypt(raw)
    for _ in range(8):
        if plain:
            break
        try:
            more = _b0_read(device)
            raw += more
            plain += server.decrypt(more)
        except usb.core.USBError:
            break
    os.makedirs("captures", exist_ok=True)
    open(f"captures/{name}_enc.bin", "wb").write(raw)
    log(f"[{name}] record cifrato ricevuto: {len(raw)} byte")
    if plain:
        open(f"captures/{name}_plain.bin", "wb").write(plain)
        body = plain[8:-5] if len(plain) > 13 else plain
        img = decode_image(body)
        write_pgm(img, f"{name}.pgm")
        log(f"[{name}] immagine decifrata {len(plain)} byte -> {len(img)} pixel, salvata {name}.pgm")
    else:
        log(f"[{name}] nessun dato immagine decifrato: controlla captures/{name}_enc.bin")


# ---------------------------------------------------------------------------- main

def detach_kernel(dev):
    import usb.core, usb.util
    d = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
    if d is None:
        sys.exit("sensore non trovato sull'host")
    for intf in d.get_active_configuration():
        n = intf.bInterfaceNumber
        try:
            if d.is_kernel_driver_active(n):
                d.detach_kernel_driver(n)
        except Exception as e:
            print(f"intf {n}: {e}")
    usb.util.dispose_resources(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="esegui la scrittura PSK (E0)")
    ap.add_argument("--capture", action="store_true", help="init TLS e acquisisci immagine")
    ap.add_argument("--probe", action="store_true", help="solo lettura")
    ap.add_argument("--finger", action="store_true", help="dopo il clear, attendi il dito e acquisisci l'impronta")
    ap.add_argument("--tag", default="fingerprint", help="nome file per l'impronta acquisita")
    ap.add_argument("--enroll", type=int, default=1, help="numero di tocchi da acquisire in sessione (template multi-frame)")
    args = ap.parse_args()
    global WAIT_FOR_FINGER, CAPTURE_TAG, ENROLL_COUNT
    WAIT_FOR_FINGER = args.finger
    CAPTURE_TAG = args.tag
    ENROLL_COUNT = args.enroll

    detach_kernel(None)
    device = goodix.Device(PRODUCT, protocol.USBProtocol)

    # Il buffer del sensore puo' contenere una risposta stale da una sessione precedente:
    # drena e ritenta finche' firmware_version non si sincronizza.
    fw = None
    for attempt in range(5):
        try:
            device.empty_buffer()
            device.nop()
            fw = device.firmware_version()
            break
        except Exception as e:
            log(f"sync tentativo {attempt + 1}: {type(e).__name__}, ridreno il buffer")
            try:
                device.empty_buffer()
            except Exception:
                pass
            time.sleep(0.2)
    if fw is None:
        sys.exit("impossibile sincronizzare il trasporto col sensore")
    log(f"firmware = {fw}")
    if not fw.startswith("GF_ST411SEC_APP_125"):
        sys.exit(f"firmware non previsto: {fw}")
    device.enable_chip(True)

    if args.probe:
        ok, flags, val = device.preset_psk_read(0xBB020003)
        log(f"bb020003 attuale = {val.hex() if val else None}")
        return

    psk = load_or_create_psk()
    if args.write:
        provision(device, psk)
    if args.capture or args.finger or args.enroll > 1:
        server = TlsPskServer(psk)
        init_and_capture(device, server)


if __name__ == "__main__":
    main()
