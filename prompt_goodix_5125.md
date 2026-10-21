# Prompt per Claude Code — driver Linux per sensore d'impronta Goodix 27c6:5125

Incolla tutto il contenuto di questo file come primo messaggio in una sessione di
Claude Code eseguita **sul laptop con il sensore** (CachyOS, Huawei MateBook).

---

## Chi sei e cosa stiamo facendo

Mi aiuti a fare reverse engineering del sensore d'impronta **Goodix `27c6:5125`** del
mio laptop Huawei MateBook (modello CREM-WXX9 / M1010), per farlo funzionare su Linux
(CachyOS). Oggi non esiste nessun driver funzionante per questo chip, ne' in libfprint
mainline ne' in fork community.

L'obiettivo finale e' un progetto **open source**: un prototipo in Python per capire il
protocollo, e poi un driver in C per libfprint da portare upstream.

Sono uno studente di ingegneria informatica, me la cavo con Linux, C e Python. Spiegami
le scelte architetturali a grandi linee mentre procediamo, cosi' seguo anche io.
Stile: diretto e conciso, prediligi i comandi alle spiegazioni lunghe. Niente emoticon,
soprattutto nel codice.

---

## REGOLE DI SICUREZZA (leggi prima di tutto)

Questo sensore cifra le immagini via TLS-PSK con una chiave pre-condivisa scritta nel
chip dal driver Windows. Lo script originale di goodix-fp-dump, se la chiave del sensore
non coincide con la loro, **CANCELLA il firmware** per riscriverlo. Sul mio sensore la
chiave e' quella di Huawei.

Quindi, vincoli assoluti:

1. **NON eseguire mai** `run_*.py`, `driver_*.py`, `flash_*.py` o qualunque funzione che
   chiami `erase_firmware`, `write_psk`, `preset_psk_write`, `update_firmware`,
   `write_firmware`, `mcu_erase_app`, `write_sensor_register`, `upload_config_mcu`
   **senza mia conferma esplicita e senza aver prima fatto il backup del firmware
   originale**.
2. **Fase 1 solo lettura.** Prima di qualunque scrittura sul sensore dobbiamo:
   (a) identificare la famiglia del chip in sola lettura, (b) catturare il traffico USB
   del driver Windows, (c) avere una copia del firmware originale.
3. Prima di ogni comando che tocca il sensore, dimmi cosa fa e aspetta il mio OK.
4. Se un passo e' irreversibile o rischia di rompere il sensore (saldato nel tasto di
   accensione, non sostituibile), fermati e chiedi.

---

## Cosa so gia' del sensore (output reale dal mio laptop)

`sudo lsusb -v -d 27c6:5125`:

- `idVendor 0x27c6` (Goodix), `idProduct 0x5125`, `bcdDevice 2.00`
- Il device si presenta come **CDC ACM** (porta seriale virtuale), `bDeviceClass 2`.
- 2 interfacce:
  - **Interface 0**: CDC Communications, 1 endpoint interrupt IN `0x82` (8 byte) — canale
    di controllo CDC.
  - **Interface 1**: CDC Data, 2 endpoint **bulk** da 64 byte: **EP OUT `0x01`**,
    **EP IN `0x81`**. Questo e' il canale dati vero.
- `Negotiated speed: Full Speed (12Mbps)`.
- `iSerial 00000000001A`.

Implicazione: il `USBProtocol` di goodix-fp-dump cerca esattamente l'interfaccia DATA con
endpoint bulk (match su `CLASS_DATA` o `CLASS_VENDOR_SPEC`), quindi la parte di trasporto
USB dovrebbe funzionare cosi' com'e'. In piu' il sensore si lega a `cdc_acm` nel kernel:
va staccato con `detach_kernel_driver` prima di parlarci (lo fa gia' il loro codice).

---

## Analisi della repo goodix-fp-dump (gia' fatta, ti riporto i risultati)

Repo: https://github.com/goodix-fp-linux-dev/goodix-fp-dump (MIT).
Fork libfprint: https://github.com/goodix-fp-linux-dev/libfprint (LGPL-2.1).

Stato: **abbandonata**. Ultimo commit su goodix-fp-dump 2023-05-30, sul fork libfprint
2023-07-23. Le issue aperte (es. #60, un altro utente con lo stesso `27c6:5125`) sono
richieste di supporto senza sviluppo. Non fare PR sulla loro repo: non vengono riviste.

Struttura utile del codice:

- `protocol.py` — `USBProtocol` (trasporto USB, riusabile as-is per noi) e `SPIProtocol`.
- `goodix.py` — classe `Device` con tutti i comandi del protocollo Goodix. Metodi di
  **lettura** sicuri: `nop`, `firmware_version`, `read_otp`, `preset_psk_read`,
  `get_iap_version`, `read_sensor_register`, `read_firmware`. Metodi di **scrittura**
  pericolosi: vedi regole di sicurezza.
- Driver per famiglia: `driver_51x0.py`, `driver_51x7.py`, `driver_52xd.py`,
  `driver_53x5.py`, `driver_53xd.py`, `driver_5503.py`, `driver_55x4.py`.
- `run_5110.py`, `run_5117.py`, ... — entrypoint per ogni product id.
- Firmware blob (submodule goodix-firmware): cartelle `51x0`, `51x7`, `52xd`, `53x5`,
  `53xd`, `5503`, `55x4`.

Fatto importante: **`driver_51x0.py` e `driver_51x7.py` sono quasi identici**. Stessa
famiglia firmware `GF_ST411SEC_APP_*`, stesso sensore 80x88, cambiano solo versione
firmware e un blocco `DEVICE_CONFIG`. Il 5125, per numerazione e struttura USB, e'
molto probabilmente della stessa famiglia **51xx**. Ipotesi di lavoro: si adatta il
driver 51x0/51x7, non si parte da zero.

Cosa NON e' risolto nemmeno per i chip supportati: il **matching**. Il sensore e' piccolo
(80x88), il matcher standard di libfprint va male con immagini cosi'. Avevano iniziato un
matcher dedicato nel fork, mai finito. Questo e' il punto dove il progetto si e' arenato.

---

## Script di probe gia' pronto

Esiste gia' uno script `goodix_probe.py` (in sola lettura) scritto apposta: stacca
`cdc_acm`, poi chiama solo `nop`, `firmware_version`, `enable_chip`, `read_otp`,
`preset_psk_read`. Non scrive nulla. Se non ce l'ho nella cartella, ricrealo con quella
logica (importa `goodix` e `protocol` dalla repo, vendor `0x27c6`, product da `sys.argv`
o default `0x5125`).

---

## Piano di lavoro (fasi)

### Fase 0 — Setup
- Clona la repo con submodule, crea venv, installa `requirements.txt`.
- Verifica che `goodix_probe.py` compili e importi.

### Fase 1 — Identificazione in SOLA LETTURA
- Esegui `goodix_probe.py` sul sensore.
- Interpreta `firmware_version`:
  - `GF_ST411SEC_APP_*` -> famiglia 51xx, caso migliore, si adatta driver esistente.
  - altra stringa -> famiglia diversa, piu' lavoro ma il trasporto vale ancora.
  - errori/timeout -> protocollo diverso, si rivede tutto.
- Registra OTP e hash PSK (NON la chiave). Confronta con i valori hardcoded nei driver
  per capire a quale famiglia somiglia il 5125.

### Fase 2 — Cattura del traffico Windows (serve Windows o VM con USB passthrough)
- Obiettivo: vedere come il driver ufficiale Goodix/Huawei inizializza il sensore, gestisce
  PSK/TLS e acquisisce l'immagine. E backup del firmware originale.
- Strumenti: Wireshark con USBPcap (Windows) oppure usbmon (Linux, se si cattura una VM).
- Deliverable: dump `.pcapng` delle sequenze: enroll, verify, init a freddo.

### Fase 3 — Reverse engineering del driver Windows
- Decompila la DLL del driver Goodix (Ghidra, o il server MCP "Reverse Engineer Anything"
  / REA se disponibile: https://github.com/morluto/rea).
- Scopo mirato: capire i comandi che differiscono dal 51x0/51x7, la gestione della
  chiave PSK, il formato del config.
- NON copiare codice decompilato nel driver finale: serve a capire il protocollo, poi si
  reimplementa pulito.

### Fase 4 — Prototipo Python per il 5125
- Crea `driver_5125.py` + `run_5125.py` partendo da `driver_51x7.py`, adattando firmware
  target, `DEVICE_CONFIG` e sequenza di init in base a fasi 1-3.
- Prima milestone: tirare fuori una **immagine grezza** dal sensore e salvarla su file.
- Solo DOPO aver fatto il backup del firmware si valuta se serve scrivere qualcosa.

### Fase 5 — Preprocessing immagine
- Riusa/adatta `preprocessor.py` (crop, threshold, mean filter) per ottenere immagini
  pulite e ripetibili.

### Fase 6 — Driver C per libfprint
- Fork di libfprint (gitlab.freedesktop.org/libfprint/libfprint).
- Implementa il driver in C basandoti sul protocollo capito nel prototipo.
- Affronta il matching: valuta il matcher interno di libfprint vs un approccio dedicato
  per sensori piccoli.

### Fase 7 — Packaging e distribuzione
- Pacchetto AUR per l'uso immediato (prototipo o driver TOD).
- PR upstream su libfprint come destinazione finale.
- Integrazione `fprintd` + PAM per login, sudo e sblocco Bitwarden/Vaultwarden.

---

## Impostazione open source (voglio capirla anch'io)

Due componenti, due licenze:

- **Prototipo Python** (`goodix-5125-dump` o simile): licenza **MIT**, come l'originale.
  Mio repo o fork del loro. Contiene solo codice mio + credito all'originale.
- **Driver C**: va in libfprint, quindi **LGPL-2.1** obbligatoria. Destinazione upstream.

Vincoli legali/pratici da rispettare in ogni commit:
- Reverse engineering per interoperabilita' e' lecito in UE, ma si pubblica **solo codice
  proprio**. Mai firmware Goodix, mai DLL Huawei/Goodix, mai chiavi. Mettili in
  `.gitignore` e tienili solo in locale come riferimento.
- Nessun segreto (PSK, chiavi) nel repo o nella history.
- `README` con stato del progetto, hardware target (`27c6:5125`, MateBook CREM-WXX9),
  avviso di rischio (puo' rompere il sensore), e credito a goodix-fp-linux-dev.

Struttura repo prototipo proposta:
```
goodix-5125-dump/
  README.md
  LICENSE            (MIT)
  .gitignore         (firmware/, *.bin, *.dll, captures/, *.pcapng)
  requirements.txt
  probe.py           (sola lettura)
  driver_5125.py
  run_5125.py
  preprocessor.py
  docs/
    protocol.md      (note sul protocollo ricostruito)
    usb-descriptors.md
```

---

## Primo passo concreto

Parti dalla Fase 0 e Fase 1. Mostrami i comandi per setup e probe, poi fermati e aspetta
l'output del probe prima di proporre qualunque scrittura. Spiegami in 3-4 righe cosa ci
aspettiamo di vedere e perche'.
