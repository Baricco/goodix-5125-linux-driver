# Porting the prototype to a C libfprint driver

Map between the Python prototype (`lab/`) and the C driver
(`drivers/goodix5125.c` (removed, see the git history), skeleton). Target: a fork of
gitlab.freedesktop.org/libfprint/libfprint, LGPL-2.1 license.

| Python prototype | C / libfprint |
|---|---|
| `protocol.USBProtocol` (goodix-fp-dump) | `fpi_usb_transfer_*` on interface 1, bulk 0x01/0x81 |
| `goodix.Device` A0 framing (encode_message_pack/protocol) | A0 functions in the driver (reusable from goodix-fp-dump C) |
| `lab/pairing.build_envelope` | `goodix5125_build_envelope` with OpenSSL (HMAC-SHA256 KDF, AES-256-GCM); cf. grold `goodix_pairing_crypto.c` |
| `driver_5125.provision` (E0) | `goodix5125_provision`, state `ACT_PROVISION` |
| `TlsPskServer` (Python ssl) | OpenSSL `SSL_CTX` PSK server, cf. grold `goodix_tls_server.c` |
| `tls_handshake` (B0 bridge) | `goodix5125_tls_handshake`, state `ACT_TLS` |
| `init_and_capture` (config90, register, FDT) | states `ACT_INIT`/`ACT_CONFIG`/`ACT_ARM_FDT` |
| `decode_image` (column major) | `goodix5125_capture` -> `FpImage` |
| `lab/preprocessor.py` | preprocessing before `fpi_image_device_image_captured` |
| `lab/match.py` (SIGFM) | SIGFM matcher in the fork (the standard minutiae matcher does poorly at 80x64) |

## Persistent state

The Linux PSK must be stored outside the device (as `state/linux_psk.bin`, mode
600). On activation: read `bb020003` and compare it with the validator of the
local PSK; if it matches, skip provisioning (no write), otherwise a single E0.
This is the "self-contained pairing" scheme of the open source driver.

## Suggested order of work

1. A0 + USB transfers (reused from goodix-fp-dump C).
2. init + config90 + TLS-PSK (handshake) -> first decrypted clear image.
3. conditional E0 provisioning.
4. preprocessing + SIGFM + libfprint enroll/verify integration.
5. packaging (AUR / TOD) and upstream PR; fprintd + PAM.
