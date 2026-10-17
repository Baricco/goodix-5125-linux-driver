# goodix-5125-dump

Reverse engineering del sensore d'impronta Goodix USB `27c6:5125` (Huawei MateBook
CREM-WXX9) per Linux. Prototipo in Python; obiettivo finale un driver C per libfprint.

**Stato: sperimentale.** Puo' riscrivere la chiave PSK del sensore. La scrittura tocca solo
lo store PSK (settore 4 della flash), e' reversibile e non puo' cancellare il firmware, ma il
sensore e' saldato nel tasto di accensione e non sostituibile: usare con cautela.

## Hardware target

- `27c6:5125`, firmware `GF_ST411SEC_APP_12508`, MCU STM32F4.
- Si presenta come CDC ACM; canale dati bulk EP OUT 0x01 / EP IN 0x81.

## Componenti

- `tools/pairing.py` — provisioning PSK (busta 0xbb010003), algoritmo e costanti dal driver
  open source di sorguido/grold (LGPL-2.1), verificati sul firmware 12508.
- `tools/gxpcap.py` — decoder del protocollo da catture usbmon/pcap.
- `driver_5125.py` — prototipo: provisioning + sessione TLS-PSK + acquisizione immagine.
- `docs/protocol.md` — protocollo ricostruito.

## Crediti

- goodix-fp-linux-dev/goodix-fp-dump (MIT) — trasporto USB e framing A0.
- sorguido / grold (goodix-27c6-5125, LGPL-2.1) — algoritmo di provisioning PSK.

## Note legali

Reverse engineering per interoperabilita' su hardware proprio. Il repo contiene solo codice
proprio e algoritmi gia' pubblicati. Firmware Goodix, DLL Huawei, catture e chiavi restano
locali (`.gitignore`) e non vengono mai pubblicati.
