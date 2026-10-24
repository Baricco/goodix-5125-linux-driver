#!/usr/bin/env python3
"""Recupero del sensore Goodix 27c6:5125 quando enumera ma non risponde (APP firmware non attivo).

Il device accetta le scritture USB ma non manda risposte: probabile MCU fermo / modalita' IAP dopo
un reset andato male. Qui si tenta di FAR RIPARTIRE l'MCU con un soft-reset mandato "alla cieca"
(l'ACK non arriva, lo ignoriamo), piu' clear-halt sugli endpoint e reset USB, poi si risincronizza
con timeout lunghi. TUTTO NON DISTRUTTIVO: niente mcu_erase_app, niente scritture in flash (E0).

Uso:  tools/.venv/bin/python lab/sensor_recover.py
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


def hard_reenumerate():
    """Unbind + rebind del device via sysfs = equivalente del Disable/Enable di Windows
    (teardown + ri-enumerazione + riconfigurazione complete). Richiede root. -> busid o None."""
    import glob
    vid, pid = f"{VENDOR:04x}", f"{PRODUCT:04x}"
    for p in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if (open(os.path.join(p, "idVendor")).read().strip() == vid and
                    open(os.path.join(p, "idProduct")).read().strip() == pid):
                busid = os.path.basename(p)      # es. "3-3"
                try:
                    open("/sys/bus/usb/drivers/usb/unbind", "w").write(busid)
                    time.sleep(1.0)
                    open("/sys/bus/usb/drivers/usb/bind", "w").write(busid)
                    return busid
                except PermissionError:
                    log("unbind/rebind richiede root: rilancia con "
                        "'sudo tools/.venv/bin/python lab/sensor_recover.py'")
                    return None
        except OSError:
            continue
    return None


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

    # 4) unbind/rebind = equivalente del Disable/Enable di Windows (ri-enumerazione completa).
    #    La cattura USB del driver Windows mostra che il recupero NON usa comandi vendor: e' solo
    #    USB reset + ri-enumerazione + SET_CONFIGURATION. Questo e' il passo piu' vicino. Serve root.
    busid = hard_reenumerate()
    if busid:
        log(f"unbind/rebind {busid} ok, riapro…")
        time.sleep(2.0)
        try:
            dev3 = goodix.Device(PRODUCT, protocol.USBProtocol)
            fw = resync(dev3, attempts=6, t=1.2)
            if fw:
                log(f"RECUPERATO (unbind/rebind): firmware = {fw}"); return
        except Exception as e:
            log(f"riapertura dopo rebind fallita: {type(e).__name__}")

    log("NON recuperato dal lato Linux. Secondo la cattura USB il driver Windows non usa comandi "
        "speciali: il recupero e' USB reset + ri-enumerazione. Se nemmeno unbind/rebind basta, il "
        "firmware e' in un wedge profondo: serve il driver ufficiale (VM Windows). "
        "Prevenzione: teardown pulito (gia' attivo) per non arrivarci.")
    sys.exit(1)


if __name__ == "__main__":
    main()
