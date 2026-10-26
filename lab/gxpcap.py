#!/usr/bin/env python3
"""Decodes Goodix 27c6:5125 traffic from a usbmon pcap (e.g. QEMU usb-host pcap=).

Reassembles the "message pack" messages (flags A0/B0/B2) from the 64-byte bulk transfers
and decodes the protocol commands (cmd, length, checksum).

Usage: gxpcap.py capture.pcap [--raw]
"""

import argparse
import struct
import sys

USBMON_HDR = 64
EP_OUT = 0x01
EP_IN = 0x81

COMMANDS = {
    0x00: "NOP", 0x20: "MCU_GET_IMAGE", 0x32: "MCU_SWITCH_TO_FDT_DOWN",
    0x34: "MCU_SWITCH_TO_FDT_UP", 0x36: "MCU_SWITCH_TO_FDT_MODE", 0x50: "NAV",
    0x60: "MCU_SWITCH_TO_SLEEP_MODE", 0x70: "MCU_SWITCH_TO_IDLE_MODE",
    0x80: "WRITE_SENSOR_REGISTER", 0x82: "READ_SENSOR_REGISTER", 0x90: "UPLOAD_CONFIG_MCU",
    0x92: "SWITCH_TO_SLEEP_MODE", 0x94: "SET_POWERDOWN_SCAN_FREQUENCY", 0x96: "ENABLE_CHIP",
    0xa2: "RESET", 0xa4: "MCU_ERASE_APP", 0xa6: "READ_OTP", 0xa8: "FIRMWARE_VERSION",
    0xac: "SET_POV_CONFIG", 0xae: "QUERY_MCU_STATE", 0xb0: "ACK", 0xc4: "SET_DRV_STATE",
    0xd0: "REQUEST_TLS_CONNECTION", 0xd2: "MCU_GET_POV_IMAGE", 0xd4: "TLS_SUCCESSFULLY_ESTABLISHED",
    0xd6: "POV_IMAGE_CHECK", 0xe0: "PRESET_PSK_WRITE", 0xe4: "PRESET_PSK_READ",
    0xf0: "WRITE_FIRMWARE", 0xf2: "READ_FIRMWARE", 0xf4: "CHECK_FIRMWARE", 0xf6: "GET_IAP_VERSION",
}


def read_pcap(path):
    """Returns (t, endpoint, data) for every bulk transfer carrying data."""
    with open(path, "rb") as f:
        gh = f.read(24)
        magic = struct.unpack("<I", gh[:4])[0]
        if magic not in (0xA1B2C3D4, 0xA1B23C4D):
            sys.exit("unsupported pcap format")
        linktype = struct.unpack("<I", gh[20:24])[0]
        if linktype not in (189, 220):
            sys.exit(f"linktype {linktype} is not usbmon")
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                break
            ts, tus, incl, _orig = struct.unpack("<IIII", rh)
            rec = f.read(incl)
            urb_type = chr(rec[8])
            xfer = rec[9]
            ep = rec[10]
            data = rec[USBMON_HDR:]
            if xfer != 3 or not data:  # bulk only
                continue
            # OUT: data in the submit; IN: data in the complete
            if (ep == EP_OUT and urb_type == "S") or (ep == EP_IN and urb_type == "C"):
                yield ts + tus / 1e6, ep, data


def split_messages(chunks):
    """Reassembles the message packs from a sequence of transfers in the same direction."""
    pending = None
    for t, data in chunks:
        if pending is None:
            if len(data) < 4:
                yield t, None, data
                continue
            flags = data[0]
            length = struct.unpack("<H", data[1:3])[0]
            ok = (sum(data[0:3]) & 0xFF) == data[3]
            pending = [t, flags, length, ok, bytearray(data[4:])]
        else:
            pending[4] += data
        t0, flags, length, ok, body = pending
        if len(body) >= length:
            pending = None
            yield t0, (flags, ok), bytes(body[:length])
    if pending is not None:
        yield pending[0], (pending[1], pending[3]), bytes(pending[4])


def describe_protocol(payload):
    if len(payload) < 4:
        return "corto", payload
    cmd = payload[0]
    length = struct.unpack("<H", payload[1:3])[0]
    body = payload[3:2 + length]
    csum = payload[2 + length] if 2 + length < len(payload) else None
    if csum == 0x88:
        cs = "nochk"
    elif csum is not None and csum == (0xAA - sum(payload[0:2 + length])) & 0xFF:
        cs = "ok"
    else:
        cs = "BAD"
    name = COMMANDS.get(cmd, f"cmd_{cmd:02x}")
    return f"{name}({cmd:#04x}) len={length - 1} chk={cs}", body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--max", type=int, default=96, help="payload bytes shown")
    args = ap.parse_args()

    out_chunks, in_chunks = [], []
    events = list(read_pcap(args.pcap))
    if not events:
        return
    t0 = events[0][0]
    msgs = []
    # Separate reassembly per direction, keeping the time of the first fragment
    for ep, store in ((EP_OUT, out_chunks), (EP_IN, in_chunks)):
        for t, e, d in events:
            if e == ep:
                store.append((t, d))
        for t, hdr, body in split_messages(store):
            msgs.append((t, ep, hdr, body))
    msgs.sort(key=lambda m: m[0])

    for t, ep, hdr, body in msgs:
        arrow = "H->D" if ep == EP_OUT else "D->H"
        if hdr is None:
            print(f"{t - t0:9.3f} {arrow} ???  {body.hex()}")
            continue
        flags, ok = hdr
        tag = f"{flags:02x}{'' if ok else '!'}"
        if flags == 0xA0:
            desc, pl = describe_protocol(body)
            shown = pl[:args.max].hex() + ("..." if len(pl) > args.max else "")
            print(f"{t - t0:9.3f} {arrow} {tag} {desc} {shown}")
        else:
            shown = body[:args.max].hex() + ("..." if len(body) > args.max else "")
            print(f"{t - t0:9.3f} {arrow} {tag} TLS len={len(body)} {shown}")


if __name__ == "__main__":
    main()
