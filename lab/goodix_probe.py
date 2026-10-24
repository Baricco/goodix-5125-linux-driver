#!/usr/bin/env python3
"""
Probe in sola lettura per sensori d'impronta Goodix USB (default 27c6:5125).

Va eseguito dalla cartella di goodix-fp-dump, perche' riusa i suoi moduli
goodix.py e protocol.py.

Comandi inviati al sensore:
  - nop, enable_chip      (gli stessi dell'inizializzazione dei loro driver)
  - firmware_version      (lettura)
  - read_otp              (lettura)
  - preset_psk_read       (lettura dell'hash della chiave, non della chiave)

NON esegue: scrittura PSK, cancellazione o flash del firmware, scrittura
di configurazione o registri.

Uso:
  sudo .venv/bin/python goodix_probe.py          # 27c6:5125
  sudo .venv/bin/python goodix_probe.py 5110     # altro product id
"""

import sys

import usb.core
import usb.util

import goodix
import protocol

VENDOR = 0x27C6
PRODUCT = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x5125


def detach_kernel_drivers():
    """Stacca cdc_acm (il sensore si presenta come seriale CDC ACM)."""
    dev = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
    if dev is None:
        sys.exit(f"Dispositivo {VENDOR:04x}:{PRODUCT:04x} non trovato")

    for intf in dev.get_active_configuration():
        n = intf.bInterfaceNumber
        try:
            if dev.is_kernel_driver_active(n):
                dev.detach_kernel_driver(n)
                print(f"Driver kernel staccato dall'interfaccia {n}")
        except usb.core.USBError as error:
            print(f"Interfaccia {n}: {error}")

    usb.util.dispose_resources(dev)


def step(name, fn):
    try:
        result = fn()
    except Exception as error:  # vogliamo proseguire con gli altri passi
        print(f"[ERR] {name}: {type(error).__name__}: {error}")
        return None
    print(f"[OK]  {name}: {result}")
    return result


def read_psk_hash(device):
    success, flags, value = device.preset_psk_read(0xBB020003)
    if not success:
        return "lettura rifiutata dal sensore"
    return f"flags={flags:#010x} valore={value.hex()}"


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
