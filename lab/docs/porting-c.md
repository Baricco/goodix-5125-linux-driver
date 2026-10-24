# Porting del prototipo al driver C libfprint

Mappa tra il prototipo Python (`lab/`) e il driver C
(`drivers/goodix5125.c (rimosso, vedi storia git)`, scheletro). Destinazione: fork di
gitlab.freedesktop.org/libfprint/libfprint, licenza LGPL-2.1.

| Prototipo Python | C / libfprint |
|---|---|
| `protocol.USBProtocol` (goodix-fp-dump) | `fpi_usb_transfer_*` su interfaccia 1, bulk 0x01/0x81 |
| `goodix.Device` framing A0 (encode_message_pack/protocol) | funzioni A0 nel driver (riusabili da goodix-fp-dump C) |
| `lab/pairing.build_envelope` | `goodix5125_build_envelope` con OpenSSL (HMAC-SHA256 KDF, AES-256-GCM); cfr. grold `goodix_pairing_crypto.c` |
| `driver_5125.provision` (E0) | `goodix5125_provision`, stato `ACT_PROVISION` |
| `TlsPskServer` (Python ssl) | OpenSSL `SSL_CTX` PSK server, cfr. grold `goodix_tls_server.c` |
| `tls_handshake` (bridge B0) | `goodix5125_tls_handshake`, stato `ACT_TLS` |
| `init_and_capture` (config90, register, FDT) | stati `ACT_INIT`/`ACT_CONFIG`/`ACT_ARM_FDT` |
| `decode_image` (colonna-major) | `goodix5125_capture` -> `FpImage` |
| `lab/preprocessor.py` | preprocessing prima di `fpi_image_device_image_captured` |
| `lab/match.py` (SIGFM) | matcher SIGFM nel fork (il matcher a minuzie standard va male a 80x64) |

## Stato di persistenza

La PSK Linux va salvata fuori dal device (come `state/linux_psk.bin`, mode 600).
All'attivazione: leggere `bb020003`, confrontarlo col validator della PSK locale; se
combacia saltare il provisioning (nessuna scrittura), altrimenti un solo E0. E' lo schema
"self-contained pairing" del driver open source.

## Ordine di lavoro suggerito

1. A0 + USB transfer (riuso da goodix-fp-dump C).
2. init + config90 + TLS-PSK (handshake) -> prima immagine clear decifrata.
3. provisioning E0 condizionale.
4. preprocessing + SIGFM + integrazione enroll/verify libfprint.
5. packaging (AUR / TOD) e PR upstream; fprintd + PAM.
