#!/usr/bin/env python3
"""Recupero del sensore Goodix 27c6:5125 quando enumera ma non risponde (APP firmware non attivo).

Il device accetta le scritture USB ma non manda risposte: probabile MCU fermo / modalita' IAP dopo
un reset andato male. Qui si tenta di FAR RIPARTIRE l'MCU con un soft-reset mandato "alla cieca"
(l'ACK non arriva, lo ignoriamo), piu' clear-halt sugli endpoint e reset USB, poi si risincronizza
con timeout lunghi. TUTTO NON DISTRUTTIVO: niente mcu_erase_app, niente scritture in flash (E0).

Uso:  tools/.venv/bin/python tools/sensor_recover.py
"""

import os
import sys
import time
import struct
import fcntl

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "goodix-fp-dump"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import usb.core
import usb.util
import goodix
import protocol

VENDOR, PRODUCT = 0x27C6, 0x5125


def log(msg):
    print(f"[recover] {msg}")


def try_step(name, fn):
    try:
        fn()
        log(f"ok: {name}")
        return True
    except Exception as e:
        log(f"-- {name}: {type(e).__name__}")
        return False


def resync(dev, attempts=6, t=1.0):
    """Prova a parlare col device con timeout lunghi. -> firmware str o None."""
    for i in range(attempts):
        try:
            dev.empty_buffer()
        except Exception:
            pass
        try:
            dev.protocol.write(goodix.encode_message_pack(
                goodix.encode_message_protocol(b"\x00\x00\x00\x00",
                                               goodix.COMMAND_NOP, checksum=False)))
        except Exception:
            pass
        try:
            fw = dev.firmware_version()
            return fw
        except Exception as e:
            log(f"  resync {i + 1}/{attempts}: {type(e).__name__}")
            time.sleep(t)
    return None


def blind_reset(dev, soft_mcu):
    """Manda il comando RESET senza aspettare l'ACK (che non arriva se l'APP e' ferma)."""
    rs = 1
    byte = (rs) | (1 if soft_mcu else 0) << 1 | (rs) << 2
    dev.protocol.write(goodix.encode_message_pack(
        goodix.encode_message_protocol(struct.pack("<BB", byte, 100),
                                       goodix.COMMAND_RESET)))


def usb_reset():
    d = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
    node = f"/dev/bus/usb/{d.bus:03d}/{d.address:03d}"
    fd = os.open(node, os.O_WRONLY)
    try:
        fcntl.ioctl(fd, (ord("U") << 8) | 20, 0)   # USBDEVFS_RESET
    finally:
        os.close(fd)


def main():
    d = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
    if d is None:
        sys.exit("sensore 27c6:5125 non trovato")
    for cfg in d:
        for intf in cfg:
            try:
                if d.is_kernel_driver_active(intf.bInterfaceNumber):
                    d.detach_kernel_driver(intf.bInterfaceNumber)
            except Exception:
                pass
    usb.util.dispose_resources(d)

    log("apro il trasporto…")
    dev = goodix.Device(PRODUCT, protocol.USBProtocol)

    # 0) gia' risponde?
    fw = resync(dev, attempts=2)
    if fw:
        log(f"GIA' ATTIVO: firmware = {fw}"); return

    # 1) clear-halt sugli endpoint
    try_step("clear_halt IN", lambda: usb.util.clear_halt(dev.protocol.device, dev.protocol.endpoint_in))
    try_step("clear_halt OUT", lambda: usb.util.clear_halt(dev.protocol.device, dev.protocol.endpoint_out))
    fw = resync(dev, attempts=3)
    if fw:
        log(f"RECUPERATO (clear_halt): firmware = {fw}"); return

    # 2) soft-reset MCU alla cieca, poi risincronizza
    for soft in (True, False):
        try_step(f"blind reset soft_mcu={soft}", lambda: blind_reset(dev, soft))
        time.sleep(1.5)
        try:
            dev.protocol.device.set_configuration()
        except Exception:
            pass
        fw = resync(dev, attempts=5, t=1.2)
        if fw:
            log(f"RECUPERATO (reset MCU): firmware = {fw}"); return

    # 3) reset USB vero e proprio, riapri, risincronizza
    try_step("USB reset", usb_reset)
    time.sleep(2.0)
    try:
        dev2 = goodix.Device(PRODUCT, protocol.USBProtocol)
        fw = resync(dev2, attempts=6, t=1.2)
        if fw:
            log(f"RECUPERATO (USB reset): firmware = {fw}"); return
    except Exception as e:
        log(f"riapertura fallita: {type(e).__name__}")

    log("NON recuperato dal lato Linux. Il firmware applicativo non riparte: "
        "serve il driver ufficiale (VM Windows) per rimetterlo in modalita' APP.")
    sys.exit(1)


if __name__ == "__main__":
    main()
