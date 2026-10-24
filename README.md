# goodix-5125 — impronta digitale su Linux (Huawei MateBook)

Login a impronta funzionante su Linux per il sensore Goodix USB `27c6:5125`
(Huawei MateBook CREM-WXX9): backend **fprintd + libfprint** con driver goodix
patchato, una **GUI di gestione impronte**, lo **sblocco del keyring via TPM** e
la messa in sicurezza del boot (**Secure Boot**, LUKS). Più il laboratorio di
reverse-engineering in Python da cui è nato tutto.

Target: `27c6:5125`, firmware `GF_ST411SEC_APP_12508`, MCU STM32F4. Si presenta
come CDC ACM; canale dati bulk EP OUT `0x01` / EP IN `0x81`.

## Cosa fa (produzione)

- **Driver libfprint goodix** (fork goodix-fp-linux-dev / Rockytkg) con le nostre
  patch — vedi `patches/libfprint-goodixgf-fixes.md`:
  - fix del finger-detect `0x80/0x82` ("reverse") che piantava enroll/verify;
  - capacità del template enroll portata da 8 a 30 frame (il tetto del recall su
    un sensore a tocco 80×64 @500 DPI);
  - **fix dei falsi positivi**: SIGFM scarta le corrispondenze ancorate al
    reticolo del sensore (fantasma fisso che faceva combaciare dita diverse) e
    una nuova pipeline immagine (DoG + CLAHE, baseline senza dito presa accanto
    al tocco, gate di qualità). Misurato sul sensore: **FAR 0/25, FRR 3/25**,
    impostori a punteggio 0 su tutti i frame del template.
- **Login a impronta** su `sudo`, greeter e lockscreen via `pam_fprintd`
  (`auth sufficient pam_fprintd.so max-tries=10`). Vedi `docs/pam-integration.md`.
- **`tools/fingerprint_manager.py`** — GUI (PySide6 + QtDBus) per registrare,
  elencare, cancellare e provare le impronte via fprintd, per utente.
- **`service/keyring-tpm/`** — sblocco automatico del keyring GNOME al login a
  impronta, sigillando la password nel TPM (PCR 7), come fa Windows. Opzionale.
- **Sicurezza del boot** — `docs/secure-boot.md` (abilitazione Secure Boot con
  `sbctl` su CachyOS/limine) e `SECURITY.md` (posizione complessiva, LUKS).

## Laboratorio (reverse engineering, Python)

Il prototipo da cui è partito tutto, utile per studio/debug del protocollo:

- `driver_5125.py` — provisioning PSK + sessione TLS-PSK + acquisizione immagine.
- `tools/pairing.py` — provisioning PSK (busta `0xbb010003`), algoritmo e costanti
  dal driver open di sorguido/grold (LGPL-2.1), verificati sul firmware 12508.
- `tools/sigfm.py` — matcher SIGFM (voto geometrico a coppie), port da
  goodix-fp-linux-dev/sigfm, vettorizzato con early-out.
- `tools/coverage.py`, `tools/enroll_engine.py`, `tools/fragment_mapper.py`,
  `tools/enroll_gui.py` — modello di enroll a copertura e GUI con mappa (mosaico).
- `docs/protocol.md`, `docs/matching.md` — protocollo e riconoscimento.

## Come funziona il riconoscimento

Il sensore è piccolo e a tocco: un frame 80×64 @500 DPI copre solo ~4×3 mm di
dito, e due pressioni combaciano solo se cadono quasi sullo stesso punto. Il
template è quindi **multi-frame** (insieme di zone del dito) e la verifica accetta
se il tocco si sovrappone ad almeno una zona registrata. Per un recall buono
serve un **enroll denso** che copra la zona di pressione. Vedi `docs/matching.md`
e `patches/libfprint-goodixgf-fixes.md`.

Taratura: `tools/fp_tune/` è il banco di prova offline. `calib_session.sh`
installa la libreria, ri-registra il dito e cattura genuini/impostori con dump
grezzi a 16 bit; `analyze_session.py` rifà enroll→verify con la pipeline C reale
(`imgproc_run`) e lo SIGFM di produzione su più varianti e soglie, così ogni
modifica al matcher si misura prima di toccare il sensore.

## Stato e sicurezza della scrittura

**Sperimentale.** Il provisioning può riscrivere la chiave PSK del sensore: tocca
solo lo store PSK (settore 4 della flash), è reversibile e non cancella il
firmware, ma il sensore è saldato nel tasto di accensione — usare con cautela.
Firmware Goodix, DLL Huawei, catture e chiavi restano locali (`.gitignore`) e non
vengono mai pubblicati.

## Crediti

- goodix-fp-linux-dev/goodix-fp-dump (MIT) — trasporto USB e framing A0.
- goodix-fp-linux-dev/sigfm — matcher SIGFM.
- Rockytkg/goodix-linux-27c6-5125 — driver libfprint goodixgf (base delle patch).
- sorguido / grold (goodix-27c6-5125, LGPL-2.1) — algoritmo di provisioning PSK.

## Note legali

Reverse engineering per interoperabilità su hardware proprio. Il repo contiene
solo codice proprio e algoritmi già pubblicati.
