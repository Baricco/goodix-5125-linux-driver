#!/usr/bin/env python3
"""USB reset of the Goodix 27c6:5125 sensor without root (thanks to udev uaccess).

Needed when the sensor stays stuck after a crash (init hangs inside libusb). It finds the
device through sysfs (no libusb, so it cannot hang in turn) and sends USBDEVFS_RESET to the node
/dev/bus/usb/BBB/DDD: the device is re-enumerated and its state (even firmware stuck halfway
through a handshake) is clean again. It touches neither the flash nor the PSK."""

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
        sys.exit("27c6:5125 sensor not found (is it connected?)")
    busnum, devnum = found
    node = f"/dev/bus/usb/{busnum:03d}/{devnum:03d}"
    try:
        fd = os.open(node, os.O_WRONLY)
    except PermissionError:
        sys.exit(f"no permission on {node} (is the udev uaccess rule active? try after logging in again, "
                 f"or: sudo tools/.venv/bin/python lab/usb_reset.py)")
    try:
        fcntl.ioctl(fd, USBDEVFS_RESET, 0)
    finally:
        os.close(fd)
    print(f"reset OK on {node} — now restart the GUI and initialize the sensor.")


if __name__ == "__main__":
    main()
