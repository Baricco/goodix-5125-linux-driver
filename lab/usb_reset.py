#!/usr/bin/env python3
"""Reset USB del sensore Goodix 27c6:5125 senza root (grazie a udev uaccess).

Serve quando il sensore resta bloccato dopo un crash (l'init si impianta in libusb). Trova il
device via sysfs (niente libusb, cosi' non si blocca a sua volta) e invia USBDEVFS_RESET al nodo
/dev/bus/usb/BBB/DDD: il device viene ri-enumerato e il suo stato (anche il firmware fermo a meta'
handshake) torna pulito. Non tocca la flash ne' la PSK."""

import os
import sys
import glob
import fcntl

VID, PID = "27c6", "5125"
USBDEVFS_RESET = ord("U") << 8 | 20   # _IO('U', 20)


def find():
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if (open(os.path.join(d, "idVendor")).read().strip() == VID and
                    open(os.path.join(d, "idProduct")).read().strip() == PID):
                busnum = int(open(os.path.join(d, "busnum")).read())
                devnum = int(open(os.path.join(d, "devnum")).read())
                return busnum, devnum
        except OSError:
            continue
    return None


def main():
    found = find()
    if not found:
        sys.exit("sensore 27c6:5125 non trovato (e' collegato?)")
    busnum, devnum = found
    node = f"/dev/bus/usb/{busnum:03d}/{devnum:03d}"
    try:
        fd = os.open(node, os.O_WRONLY)
    except PermissionError:
        sys.exit(f"niente permesso su {node} (regola udev uaccess attiva? prova dopo un relogin, "
                 f"oppure: sudo tools/.venv/bin/python lab/usb_reset.py)")
    try:
        fcntl.ioctl(fd, USBDEVFS_RESET, 0)
    finally:
        os.close(fd)
    print(f"reset OK su {node} — ora rilancia la GUI e inizializza il sensore.")


if __name__ == "__main__":
    main()
