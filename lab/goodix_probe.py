#!/usr/bin/env python3
"""
Read-only probe for Goodix USB fingerprint sensors (default 27c6:5125).

It must be run from the goodix-fp-dump folder, because it reuses its
goodix.py and protocol.py modules.

Commands sent to the sensor:
  - nop, enable_chip      (the same as their drivers' initialization)
  - firmware_version      (read)
  - read_otp              (read)
  - preset_psk_read       (reads the hash of the key, not the key)

It does NOT: write the PSK, erase or flash the firmware, write
configuration or registers.

Usage:
  sudo .venv/bin/python goodix_probe.py          # 27c6:5125
  sudo .venv/bin/python goodix_probe.py 5110     # another product id
"""

import sys

import usb.core
import usb.util

import goodix
import protocol

VENDOR = 0x27C6
PRODUCT = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x5125


def detach_kernel_drivers():
    """Detaches cdc_acm (the sensor shows up as a CDC ACM serial device)."""
    dev = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
    if dev is None:
        sys.exit(f"Device {VENDOR:04x}:{PRODUCT:04x} not found")

    for intf in dev.get_active_configuration():
        n = intf.bInterfaceNumber
        try:
            if dev.is_kernel_driver_active(n):
                dev.detach_kernel_driver(n)
                print(f"Kernel driver detached from interface {n}")
        except usb.core.USBError as error:
            print(f"Interfaccia {n}: {error}")

    usb.util.dispose_resources(dev)


def step(name, fn):
    try:
        result = fn()
    except Exception as error:  # we want to go on with the other steps
        print(f"[ERR] {name}: {type(error).__name__}: {error}")
        return None
    print(f"[OK]  {name}: {result}")
    return result


def read_psk_hash(device):
    success, flags, value = device.preset_psk_read(0xBB020003)
    if not success:
        return "read refused by the sensor"
    return f"flags={flags:#010x} value={value.hex()}"


def main():
    detach_kernel_drivers()

    device = goodix.Device(PRODUCT, protocol.USBProtocol)

    step("nop", device.nop)
    step("firmware_version", device.firmware_version)
    step("enable_chip", lambda: device.enable_chip(True))
    step("nop", device.nop)
    step("read_otp", lambda: device.read_otp().hex())
    step("preset_psk_read", lambda: read_psk_hash(device))


if __name__ == "__main__":
    main()
