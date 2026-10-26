# Goodix 27c6:5125 protocol (firmware GF_ST411SEC_APP_12508)

Reconstructed from the USB capture of a session of the Windows driver
1.1.125.13 (`captures/01-attach-provisioning.pcap`, decoded in
`captures/01-decoded.txt`) and from static analysis of `gfusb.dll` and of the
MCU firmware (`re/`). Everything verified on real hardware (MateBook
CREM-WXX9).

## Transport

- USB CDC: data interface 1, bulk OUT `0x01`, bulk IN `0x81`, 64-byte packets.
- Two layers of encapsulation (identical to goodix-fp-dump, reusable as-is):
  - **message pack**: `[flags u8][len u16 LE][checksum u8] + payload`.
    `flags` = `0xA0` command protocol, `0xB0`/`0xB2` TLS records.
  - **message protocol** (inside A0 packs): `[cmd u8][len+1 u16 LE] + payload + [checksum u8]`
    with `checksum = 0xAA - sum`. `cmd` encodes `cmd0<<4 | cmd1<<1`.
- Every command first receives an ACK (`0xB0`, payload `[cmd][flags]`), then
  the reply with the original cmd.

## Secure channel

- TLS-PSK, **the host acts as the server**, the sensor as the client. PSK
  identity `Client_identity`.
- PSK cipher suites (AES-GCM/CBC with SHA256, see the firmware strings). TLS
  records are encapsulated in `0xB0` message packs; images travel as encrypted
  application records.

## PSK provisioning (command E0) — the key point

The PSK is a 32-byte key shared by host and sensor. The sensor keeps it as an
envelope in sector 4 of its flash. Provisioning can be fully reimplemented (no
firmware secret is needed) because the algorithm and the constants are
published by the open source driver by sorguido/grold
(`goodix_pairing_crypto.c`, LGPL-2.1) and were verified here byte for byte
against firmware 12508.

### 0xbb010003 envelope format (102 bytes)

```
constants (public, valid for both 12509 and 12508, verified through the AES-GCM tag of the capture):
  KDF_KEY = 5cba6e25 819518de 2d53e96d c0347ab0 d427d408 4bda4fae 1bff2b09 112a57e5
  AAD     = 522dc1f0 99567d07 f47f37a3 2a84427d
  HEADER  = 02ff20000000
  LABEL   = "kgoodwixg\0kaelrgnoerlithm"

derived[48] = HMAC_SHA256(KDF_KEY, BE32(round) || LABEL || BE32(0x180))  for rounds 1,2   (independent of the PSK)
aes_key  = derived[0:32]
mac_key  = derived[16:48]
nonce    = SHA256(HEADER || PSK[0:8] || (03000000 x16))[0:16]
ct,tag   = AES-256-GCM(aes_key, nonce, aad=AAD, plaintext=PSK)            -> 32+16 bytes
outer    = HMAC_SHA256(mac_key, HEADER || ct || tag)                      -> 32 bytes
envelope = outer(32) || HEADER(6) || nonce(16) || ct(32) || tag(16)       = 102 bytes
validator (0xbb020003) = SHA256(envelope)                                 = 32 bytes
```

Implementation: `lab/pairing.py`. Compatibility check with 12508: the AES-GCM
tag of the captured envelope authenticates with these constants, and the
computed validator `78958b50...eeba7` matches the one the sensor reports after
the write.

### Body of the E0 command (452 bytes)

```
TLV(0xbb010002, 332, <DPAPI blob, preserved by the sensor>)   = 340 bytes
TLV(0xbb010003, 102, <envelope>)                              = 110 bytes
padding to a multiple of 4                                    = 2 bytes
```

The firmware (E0 handler, FUN_080277d4) erases **only** sector 4 and rewrites
these TLVs with length + CRC-32/MPEG. It is reversible and cannot brick the
chip. The `bb010002` blob is the DPAPI-sealed copy used by Windows: on Linux it
is read and written back unchanged (`preset_psk_read 0xbb010002`); it does not
need to be decrypted for operation.

Linux does not need to recover Windows' PSK: it generates its own PSK, builds
the envelope and writes it. This is exactly what the open source driver does.

## Initialization and capture sequence (from the capture)

```
1.  nop / firmware_version (0xa8)      -> "GF_ST411SEC_APP_12508"
2.  enable_chip (0x96)
3.  query_mcu_state (0xae)
4.  preset_psk_read 0xbb020003 (0xe4)  -> current validator
5.  preset_psk_read 0xbb010002         -> DPAPI blob (332 bytes, to preserve)
6.  PRESET_PSK_WRITE E0 (0xe0)         -> writes bb010002+bb010003 (provisioning)
7.  preset_psk_read 0xbb020003         -> confirms the validator
8.  reset (0xa2, 0114)                 -> reply number 2048
9.  read_sensor_register 0x0000 (0x82) -> chip id
10. read_otp (0xa6)                    -> 64 bytes (starts with "S3G666.")
11. reset
12. mcu_switch_to_idle_mode (0x70)
13. write_sensor_register (0x80) x4    -> 0x0220=980b? ... (see the capture; dac values)
14. upload_config_mcu (0x90)           -> 224-byte block (captures/config90.bin),
                                          LE finalizer: (-0xa5a5 - sum(word[0..110])) & 0xffff
15. request_tls_connection (0xd0) + TLS-PSK handshake over 0xB0 records
16. tls_successfully_established (0xd4)
17. query_mcu_state; mcu_switch_to_fdt_mode (0x36); nav (0x50)
18. write_sensor_register x4 (0x0220=780b, 0x0236=b900, 0x0238=b800, 0x023a=b700)
19. mcu_get_image (0x20, flag 0xB2)    -> image encrypted through TLS
20. mcu_switch_to_fdt_down (0x32)      -> wait for the finger, then another get_image
```

80x64 sensor, 5120 samples, raw image as packed 12-bit values.

## Implementation status

- `lab/pairing.py` — complete and verified PSK provisioning.
- `lab/driver_5125.py` — prototype: E0 provisioning + in-process TLS-PSK
  handshake + capture. The generated E0 body matches the Windows driver's byte
  for byte (offline test). The bench validation of image capture through TLS
  was still missing at the time (it needs the sensor on the host).

## Image decoding (confirmed on the bench)

TLS plaintext of 7693 bytes = `[cmd/class u8][len u16][5-byte header] + packed(7680) + CRC32/MPEG(4) + chk(1)`.
The packed part is 5120 12-bit samples, 4 every 6 bytes. **The transmission
order is column-major**: `raster = (wire % 64) * 80 + wire // 64`. Without this
transposition the image looks striped. Record CRC = CRC-32/MPEG-2 over the
packed data. 8-bit scaling: `sample * 255 / 4095`. Minimal preprocessing:
subtracting a clear (baseline) frame reveals the ridges.

Result: a real fingerprint captured end to end on Linux (clear-0.pgm,
fingerprint.pgm).
