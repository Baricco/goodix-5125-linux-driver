#!/usr/bin/env python3
"""Wrapper riutilizzabile del sensore Goodix 27c6:5125 per la GUI di enroll.

Incapsula il ciclo di vita gia' validato in driver_5125.py (sync trasporto, TLS-PSK, config MCU)
ed espone operazioni semplici: open(), capture_clear(), capture_touch(), close(). Non esegue MAI
scritture sul sensore (niente provisioning E0): assume la PSK gia' presente (state/linux_psk.bin).
Grazie alla regola udev uaccess gira da utente normale, senza sudo.

I frame tornano come array (H, W) uint16 nello stesso formato che si aspetta enroll_engine
(preprocessor.decode del corpo immagine)."""

import os
import sys
import time

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(_ROOT, "goodix-fp-dump"))
sys.path.insert(0, _ROOT)               # per driver_5125.py (nella root del progetto)
sys.path.insert(0, os.path.dirname(__file__))

import usb.core
import usb.util
import goodix
import protocol

import driver_5125 as drv
from preprocessor import decode

W, H = drv.SENSOR_WIDTH, drv.SENSOR_HEIGHT

# sequenze di comando FDT letterali, dalla sessione del driver Windows (come in driver_5125)
_FDT_ARM = b"\x0d\x01\x80\xb1\x80\xc1\x80\xa6\x80\xb6\x80\xa5\x80\xb6"
_FDT_DOWN_ARM = b"\x0c\x01\x80\xb1\x80\xc1\x80\xa6\x80\xb6\x80\xa5\x80\xb6"
_FDT_DOWN_WAIT = b"\x0c\x01\x80\xb2\x80\xc2\x80\xa7\x80\xb6\x80\xa6\x80\xb6"
_FDT_UP = b"\x0e\x01\x80\x90\x80\x9a\x80\x7b\x80\x95\x80\x8c\x80\xa3"


class Sensor:
    def __init__(self):
        self.device = None
        self.server = None

    # ---- ciclo di vita ----
    def open(self):
        drv.detach_kernel(None)
        self.device = goodix.Device(drv.PRODUCT, protocol.USBProtocol)
        fw = None
        for _ in range(5):
            try:
                self.device.empty_buffer()
                self.device.nop()
                fw = self.device.firmware_version()
                break
            except Exception:
                try:
                    self.device.empty_buffer()
                except Exception:
                    pass
                time.sleep(0.2)
        if fw is None:
            raise SystemError("impossibile sincronizzare il trasporto col sensore")
        if not fw.startswith("GF_ST411SEC_APP_125"):
            raise SystemError(f"firmware non previsto: {fw}")
        self.firmware = fw
        self.device.enable_chip(True)

        psk = drv.load_or_create_psk()
        self.server = drv.TlsPskServer(psk)

        d = self.device
        d.reset(True, False, 20)
        d.read_sensor_register(0x0000, 4)
        d.read_otp()
        d.reset(True, False, 20)
        d.upload_config_mcu(drv.CONFIG90)
        d.set_powerdown_scan_frequency(100)
        drv.tls_handshake(d, self.server)
        d.nop()
        d.query_mcu_state(b"\x55", True)
        d.mcu_switch_to_fdt_mode(b"\x0d\x01\xb2\xb2\xc2\xc2\xa7\xa7\xb6\xb6\xa6\xa6\xb6\xb6", True)
        d.nav()
        d.mcu_switch_to_fdt_mode(b"\x0d\x01\x80\xb0\x80\xc0\x80\xa4\x80\xb4\x80\xa3\x80\xb4", True)
        d.read_sensor_register(0x0082, 2)
        for addr, val in ((0x0220, b"\x78\x0b"), (0x0236, b"\xb9\x00"),
                          (0x0238, b"\xb8\x00"), (0x023a, b"\xb7\x00")):
            d.write_sensor_register(addr, val)

    def close(self):
        try:
            if self.device is not None:
                usb.util.dispose_resources(self.device.protocol.device)
        except Exception:
            pass
        self.device = None

    # ---- acquisizione ----
    def _read_image(self):
        enc = self.device.mcu_get_image(b"\x01\x00", goodix.FLAGS_TRANSPORT_LAYER_SECURITY)
        raw = bytes(enc)
        plain = self.server.decrypt(raw)
        for _ in range(8):
            if plain:
                break
            try:
                more = drv._b0_read(self.device)
                plain += self.server.decrypt(more)
            except usb.core.USBError:
                break
        if not plain or len(plain) < 8 + 7680:
            raise SystemError("immagine non decifrata correttamente")
        return decode(plain[8:8 + 7680])

    def capture_clear(self):
        """Frame di baseline (nessun dito). Da chiamare una volta dopo open()."""
        return self._read_image()

    def arm(self):
        """Arma il rilevamento dito per il prossimo tocco."""
        d = self.device
        d.mcu_switch_to_fdt_mode(_FDT_ARM, True)
        d.mcu_switch_to_fdt_down(_FDT_DOWN_ARM, True)
        d.nop()
        d.query_mcu_state(b"\x55", True)

    def wait_touch_and_capture(self, tries=40):
        """Attende il contatto del dito e acquisisce il frame. Va chiamato dopo arm()."""
        for _ in range(tries):
            try:
                self.device.mcu_switch_to_fdt_down(_FDT_DOWN_WAIT, True)
                break
            except usb.core.USBError:
                pass
        return self._read_image()

    def wait_lift(self, tries=40):
        """Attende il sollevamento del dito prima del tocco successivo."""
        for _ in range(tries):
            try:
                self.device.mcu_switch_to_fdt_up(_FDT_UP)
                return
            except usb.core.USBError:
                pass
