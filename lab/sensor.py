#!/usr/bin/env python3
"""Reusable wrapper of the Goodix 27c6:5125 sensor for the enrollment GUI.

Wraps the life cycle already validated in driver_5125.py (transport sync, TLS-PSK, MCU config)
and exposes simple operations: open(), capture_clear(), capture_touch(), close(). It NEVER
writes to the sensor (no E0 provisioning): it assumes the PSK is already there (state/linux_psk.bin).
Thanks to the udev uaccess rule it runs as a normal user, without sudo.

Frames come back as (H, W) uint16 arrays in the same format enroll_engine expects
(preprocessor.decode of the image body)."""

import os
import sys
import time
import glob
import fcntl

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(_ROOT, "goodix-fp-dump"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # driver_5125.py lives in lab/
sys.path.insert(0, os.path.dirname(__file__))

import usb.core
import usb.util
import goodix
import protocol

import driver_5125 as drv
from preprocessor import decode

W, H = drv.SENSOR_WIDTH, drv.SENSOR_HEIGHT

# literal FDT command sequences, from the Windows driver session (as in driver_5125)
_FDT_ARM = b"\x0d\x01\x80\xb1\x80\xc1\x80\xa6\x80\xb6\x80\xa5\x80\xb6"
_FDT_DOWN_ARM = b"\x0c\x01\x80\xb1\x80\xc1\x80\xa6\x80\xb6\x80\xa5\x80\xb6"
_FDT_DOWN_WAIT = b"\x0c\x01\x80\xb2\x80\xc2\x80\xa7\x80\xb6\x80\xa6\x80\xb6"
_FDT_UP = b"\x0e\x01\x80\x90\x80\x9a\x80\x7b\x80\x95\x80\x8c\x80\xa3"


def reset_usb(vendor=drv.VENDOR, product=drv.PRODUCT):
    """USB reset of the device (re-enumeration) through USBDEVFS_RESET. Finds the node by VID/PID in
    sysfs (no hard-coded bus/address) -> portable to any machine. Non-destructive.
    Used to always start from a clean state, even if the previous session died halfway."""
    vid, pid = f"{vendor:04x}", f"{product:04x}"
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if (open(os.path.join(d, "idVendor")).read().strip() == vid and
                    open(os.path.join(d, "idProduct")).read().strip() == pid):
                bus = int(open(os.path.join(d, "busnum")).read())
                dev = int(open(os.path.join(d, "devnum")).read())
                fd = os.open(f"/dev/bus/usb/{bus:03d}/{dev:03d}", os.O_WRONLY)
                try:
                    fcntl.ioctl(fd, (ord("U") << 8) | 20, 0)
                finally:
                    os.close(fd)
                return True
        except OSError:
            continue
    return False


class Sensor:
    def __init__(self):
        self.device = None
        self.server = None

    # ---- life cycle ----
    def _connect(self):
        """Opens the transport and synchronizes. -> firmware string, or raises."""
        drv.detach_kernel(None)
        self.device = goodix.Device(drv.PRODUCT, protocol.USBProtocol)
        for _ in range(5):
            try:
                self.device.empty_buffer()
                self.device.nop()
                return self.device.firmware_version()
            except Exception:
                try:
                    self.device.empty_buffer()
                except Exception:
                    pass
                time.sleep(0.2)
        raise SystemError("sync failed")

    def open(self):
        # FIRST try to talk to it directly: after a clean teardown the device is already fine.
        # The USB reset re-enumerates the sensor (new address) and, done on EVERY open, makes it vanish
        # for a moment on consecutive verifies -> "sensor not found". So the reset stays only
        # as SELF-HEALING, when the direct sync fails (dirty state after a crash halfway through TLS).
        # (detach_kernel calls sys.exit if the device is not enumerated: SystemExit is caught too.)
        fw = None
        try:
            fw = self._connect()
        except (Exception, SystemExit):
            for wait in (0.5, 0.9):
                reset_usb()
                time.sleep(wait)
                try:
                    fw = self._connect()
                    break
                except (Exception, SystemExit):
                    fw = None
            if fw is None:
                raise SystemError(
                    "sensor cannot be synchronized even after a USB reset: it may be in a "
                    "deep stuck state (recovery needed). See lab/sensor_recover.py")
        if not fw.startswith("GF_ST411SEC_APP_125"):
            raise SystemError(f"unexpected firmware: {fw}")
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
        # CLEAN TEARDOWN: soft reset of the MCU so the firmware restarts and the device is never
        # left stuck halfway through a TLS session (the main cause of the wedge).
        try:
            if self.device is not None:
                try:
                    self.device.reset(False, True, 20)   # reset_sensor=False, soft_reset_mcu=True
                except Exception:
                    pass
                usb.util.dispose_resources(self.device.protocol.device)
        except Exception:
            pass
        self.device = None

    # ---- capture ----
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
            raise SystemError("image not decrypted correctly")
        return decode(plain[8:8 + 7680])

    def capture_clear(self):
        """Baseline frame (no finger). To be called once after open()."""
        return self._read_image()

    def grab(self):
        """CONTINUOUS capture: a single image through mcu_get_image, without the FDT dance
        (no waiting for finger-down / lift events). For swipe mapping: the user keeps the
        finger pressed and moves it, we capture one frame after another. Much faster and smoother."""
        return self._read_image()

    def arm(self):
        """Arms finger detection for the next touch."""
        d = self.device
        d.mcu_switch_to_fdt_mode(_FDT_ARM, True)
        d.mcu_switch_to_fdt_down(_FDT_DOWN_ARM, True)
        d.nop()
        d.query_mcu_state(b"\x55", True)

    def wait_touch_and_capture(self, tries=40):
        """Waits for the finger to touch and captures the frame. Must be called after arm()."""
        for _ in range(tries):
            try:
                self.device.mcu_switch_to_fdt_down(_FDT_DOWN_WAIT, True)
                break
            except usb.core.USBError:
                pass
        return self._read_image()

    def wait_lift(self, tries=40):
        """Waits for the finger to be lifted before the next touch."""
        for _ in range(tries):
            try:
                self.device.mcu_switch_to_fdt_up(_FDT_UP)
                return
            except usb.core.USBError:
                pass
