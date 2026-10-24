# Protocollo Goodix 27c6:5125 (firmware GF_ST411SEC_APP_12508)

Ricostruito dalla cattura USB della sessione del driver Windows 1.1.125.13
(`captures/01-attach-provisioning.pcap`, decodifica in `captures/01-decoded.txt`)
e dall'analisi statica di `gfusb.dll` e del firmware MCU (`re/`).
Tutto verificato su hardware reale (MateBook CREM-WXX9).

## Trasporto

- USB CDC: interfaccia dati 1, bulk OUT `0x01`, bulk IN `0x81`, pacchetti da 64 byte.
- Due livelli di incapsulamento (identici a goodix-fp-dump, riusabili as-is):
  - **message pack**: `[flags u8][len u16 LE][checksum u8] + payload`.
    `flags` = `0xA0` protocollo comandi, `0xB0`/`0xB2` record TLS.
  - **message protocol** (dentro i pack A0): `[cmd u8][len+1 u16 LE] + payload + [checksum u8]`
    con `checksum = 0xAA - sum`. `cmd` codifica `cmd0<<4 | cmd1<<1`.
- Ogni comando riceve prima un ACK (`0xB0`, payload `[cmd][flags]`), poi la risposta col
  cmd originale.

## Canale sicuro

- TLS-PSK, **l'host fa da server**, il sensore da client. PSK identity `Client_identity`.
- Ciphersuite PSK (AES-GCM/CBC con SHA256, vedi stringhe firmware). Record TLS incapsulati
  nei message pack `0xB0`; le immagini viaggiano come record applicativi cifrati.

## Provisioning PSK (comando E0) — il punto chiave

La PSK e' una chiave a 32 byte condivisa host/sensore. Il sensore la conserva in forma di
busta nel settore 4 della flash. Il provisioning e' interamente reimplementabile
(nessun segreto del firmware necessario) perche' l'algoritmo e le costanti sono pubblicati
dal driver open source di sorguido/grold (`goodix_pairing_crypto.c`, LGPL-2.1) e qui
verificati byte-per-byte contro il firmware 12508.

### Formato busta 0xbb010003 (102 byte)

```
costanti (pubbliche, valide sia 12509 sia 12508, verificate via tag AES-GCM sulla cattura):
  KDF_KEY = 5cba6e25 819518de 2d53e96d c0347ab0 d427d408 4bda4fae 1bff2b09 112a57e5
  AAD     = 522dc1f0 99567d07 f47f37a3 2a84427d
  HEADER  = 02ff20000000
  LABEL   = "kgoodwixg\0kaelrgnoerlithm"

derived[48] = HMAC_SHA256(KDF_KEY, BE32(round) || LABEL || BE32(0x180))  per round 1,2   (indip. dalla PSK)
aes_key  = derived[0:32]
mac_key  = derived[16:48]
nonce    = SHA256(HEADER || PSK[0:8] || (03000000 x16))[0:16]
ct,tag   = AES-256-GCM(aes_key, nonce, aad=AAD, plaintext=PSK)            -> 32+16 byte
outer    = HMAC_SHA256(mac_key, HEADER || ct || tag)                      -> 32 byte
busta    = outer(32) || HEADER(6) || nonce(16) || ct(32) || tag(16)       = 102 byte
validator (0xbb020003) = SHA256(busta)                                    = 32 byte
```

Implementazione: `lab/pairing.py`. Verifica di compatibilita' col 12508:
il tag AES-GCM della busta catturata autentica con queste costanti, e il validator calcolato
`78958b50...eeba7` coincide con quello che il sensore riporta dopo la scrittura.

### Corpo del comando E0 (452 byte)

```
TLV(0xbb010002, 332, <blob DPAPI, preservato dal sensore>)   = 340 byte
TLV(0xbb010003, 102, <busta>)                                = 110 byte
padding a multiplo di 4                                       = 2 byte
```

Il firmware (handler E0, FUN_080277d4) cancella **solo** il settore 4 e riscrive questi TLV
con lunghezza + CRC-32/MPEG. Reversibile, non puo' bloccare il chip. Il blob `bb010002` e'
la copia sigillata DPAPI usata da Windows: su Linux va letto e riscritto invariato
(`preset_psk_read 0xbb010002`), non serve decifrarlo per il funzionamento.

Per Linux non serve recuperare la PSK di Windows: si genera una PSK propria, si costruisce la
busta e la si scrive. Questo e' esattamente cio' che fa il driver open source.

## Sequenza di inizializzazione e acquisizione (dalla cattura)

```
1.  nop / firmware_version (0xa8)      -> "GF_ST411SEC_APP_12508"
2.  enable_chip (0x96)
3.  query_mcu_state (0xae)
4.  preset_psk_read 0xbb020003 (0xe4)  -> validator attuale
5.  preset_psk_read 0xbb010002         -> blob DPAPI (332 byte, da preservare)
6.  PRESET_PSK_WRITE E0 (0xe0)         -> scrive bb010002+bb010003 (provisioning)
7.  preset_psk_read 0xbb020003         -> conferma validator
8.  reset (0xa2, 0114)                 -> risposta number 2048
9.  read_sensor_register 0x0000 (0x82) -> chip id
10. read_otp (0xa6)                    -> 64 byte (inizia "S3G666.")
11. reset
12. mcu_switch_to_idle_mode (0x70)
13. write_sensor_register (0x80) x4    -> 0x0220=980b? ... (vedi cattura; valori dac)
14. upload_config_mcu (0x90)           -> blocco 224 byte (captures/config90.bin),
                                          finalizzatore LE: (-0xa5a5 - sum(word[0..110])) & 0xffff
15. request_tls_connection (0xd0) + handshake TLS-PSK sui record 0xB0
16. tls_successfully_established (0xd4)
17. query_mcu_state; mcu_switch_to_fdt_mode (0x36); nav (0x50)
18. write_sensor_register x4 (0x0220=780b, 0x0236=b900, 0x0238=b800, 0x023a=b700)
19. mcu_get_image (0x20, flag 0xB2)    -> immagine cifrata via TLS
20. mcu_switch_to_fdt_down (0x32)      -> attesa dito, poi altra get_image
```

Sensore 80x64, 5120 campioni, immagine grezza 12 bit impacchettati.

## Stato implementazione

- `lab/pairing.py` — provisioning PSK completo e verificato.
- `lab/driver_5125.py` — prototipo: provisioning E0 + handshake TLS-PSK in-process + acquisizione.
  Il corpo E0 generato coincide byte-per-byte con quello del driver Windows (test offline).
  Manca la validazione sul banco della cattura immagine via TLS (serve il sensore su host).

## Decodifica immagine (confermata sul banco)

Plaintext TLS 7693 byte = `[cmd/class u8][len u16][5 byte intestazione] + packed(7680) + CRC32/MPEG(4) + chk(1)`.
Il packed e' 5120 campioni a 12 bit, 4 ogni 6 byte. **Ordine di trasmissione colonna-major**:
`raster = (wire % 64) * 80 + wire // 64`. Senza questa trasposizione l'immagine appare striata.
CRC record = CRC-32/MPEG-2 sul packed. Scala a 8 bit: `sample * 255 / 4095`.
Preprocessing minimo: sottrarre un frame clear (baseline) rivela le creste.

Risultato: impronta reale acquisita end-to-end su Linux (clear-0.pgm, fingerprint.pgm).
