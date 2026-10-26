#!/usr/bin/env python3
"""Recovery of the Goodix 27c6:5125 sensor when it enumerates but does not answer (APP firmware not running).

The device accepts USB writes but sends no replies: probably the MCU is stopped / in IAP mode after
a bad reset. Here we try to RESTART the MCU with a soft reset sent "blindly" (the ACK does not
arrive, we ignore it), plus clear-halt on the endpoints and a USB reset, then resynchronize with
long timeouts. ALL NON-DESTRUCTIVE: no mcu_erase_app, no flash writes (E0).

Usage:  tools/.venv/bin/python lab/sensor_recover.py
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
    """Tries to talk to the device with long timeouts. -> firmware str or None."""
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
    """Sends the RESET command without waiting for the ACK (which does not arrive if the APP is stopped)."""
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


def hard_reenumerate():
    """Unbind + rebind of the device through sysfs = the equivalent of Windows' Disable/Enable
    (complete teardown + re-enumeration + reconfiguration). Needs root. -> busid or None."""
    import glob
    vid, pid = f"{VENDOR:04x}", f"{PRODUCT:04x}"
    for p in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if (open(os.path.join(p, "idVendor")).read().strip() == vid and
                    open(os.path.join(p, "idProduct")).read().strip() == pid):
                busid = os.path.basename(p)      # e.g. "3-3"
                try:
                    open("/sys/bus/usb/drivers/usb/unbind", "w").write(busid)
                    time.sleep(1.0)
                    open("/sys/bus/usb/drivers/usb/bind", "w").write(busid)
                    return busid
                except PermissionError:
                    log("unbind/rebind needs root: run it again with "
                        "'sudo tools/.venv/bin/python lab/sensor_recover.py'")
                    return None
        except OSError:
            continue
    return None


def main():
    d = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
    if d is None:
        sys.exit("27c6:5125 sensor not found")
    for cfg in d:
        for intf in cfg:
            try:
                if d.is_kernel_driver_active(intf.bInterfaceNumber):
                    d.detach_kernel_driver(intf.bInterfaceNumber)
            except Exception:
                pass
    usb.util.dispose_resources(d)

    log("opening the transport…")
    dev = goodix.Device(PRODUCT, protocol.USBProtocol)

    # 0) does it already answer?
    fw = resync(dev, attempts=2)
    if fw:
        log(f"ALREADY ACTIVE: firmware = {fw}"); return

    # 1) clear-halt on the endpoints
    try_step("clear_halt IN", lambda: usb.util.clear_halt(dev.protocol.device, dev.protocol.endpoint_in))
    try_step("clear_halt OUT", lambda: usb.util.clear_halt(dev.protocol.device, dev.protocol.endpoint_out))
    fw = resync(dev, attempts=3)
    if fw:
        log(f"RECUPERATO (clear_halt): firmware = {fw}"); return

    # 2) blind MCU soft reset, then resynchronize
    for soft in (True, False):
        try_step(f"blind reset soft_mcu={soft}", lambda: blind_reset(dev, soft))
        time.sleep(1.5)
        try:
            dev.protocol.device.set_configuration()
        except Exception:
            pass
        fw = resync(dev, attempts=5, t=1.2)
        if fw:
            log(f"RECOVERED (MCU reset): firmware = {fw}"); return

    # 3) a real USB reset, reopen, resynchronize
    try_step("USB reset", usb_reset)
    time.sleep(2.0)
    try:
        dev2 = goodix.Device(PRODUCT, protocol.USBProtocol)
        fw = resync(dev2, attempts=6, t=1.2)
        if fw:
            log(f"RECOVERED (USB reset): firmware = {fw}"); return
    except Exception as e:
        log(f"reopen failed: {type(e).__name__}")

    # 4) unbind/rebind = the equivalent of Windows' Disable/Enable (complete re-enumeration).
    #    The USB capture of the Windows driver shows that recovery uses NO vendor command: it is only
    #    USB reset + re-enumeration + SET_CONFIGURATION. This is the closest step. Needs root.
    busid = hard_reenumerate()
    if busid:
        log(f"unbind/rebind {busid} ok, reopening…")
        time.sleep(2.0)
        try:
            dev3 = goodix.Device(PRODUCT, protocol.USBProtocol)
            fw = resync(dev3, attempts=6, t=1.2)
            if fw:
                log(f"RECUPERATO (unbind/rebind): firmware = {fw}"); return
        except Exception as e:
            log(f"reopen after rebind failed: {type(e).__name__}")

    log("NOT recovered from the Linux side. According to the USB capture the Windows driver uses no special "
        "commands: recovery is USB reset + re-enumeration. If not even unbind/rebind is enough, the "
        "firmware is in a deep wedge: the official driver is needed (Windows VM). "
        "Prevention: clean teardown (already active) so it never gets there.")
    sys.exit(1)


if __name__ == "__main__":
    main()
