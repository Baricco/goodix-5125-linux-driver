# Patch al driver libfprint goodixgf (2026-10-24)

Modifiche al fork `~/goodix-linux-27c6-5125/libfprint` (Rockytkg), driver
`libfprint/drivers/goodixgf/`. Da contribuire upstream a goodix-fp-linux-dev.
Il driver goodixgf è untracked nel repo del fork, quindi queste note sono il
record delle modifiche (i file live sono quelli già compilati e installati).

Rebuild: `ninja -C ~/goodix-linux-27c6-5125/libfprint/build`
Install: `sudo cp build/libfprint/libfprint-2.so.2.0.0 /usr/lib/ && sudo systemctl restart fprintd`
Backup pristino della lib: `~/libfprint-2.so.2.0.0.orig`

## 1. core/goodix_capture.c — finger-detect 0x80/0x82 ("reverse")

Problema: l'enroll/verify si piantava ("no image frame received"). Quando il
tocco arriva come `IrqStatus=0x80/0x82` (reverse: baseline FDT imparata storta)
il driver lo ignorava (gestiva solo 0x2/0x200/0x100).

- `dispatch_payload()`: richiedi l'immagine (SetMode Image 0x20) anche su
  `irq==0x80||0x82`, non solo `irq==2`. I frame cattivi li scarta già il loop
  a valle (gx_fdt_check_finger -> void -> re-arm).
- `handle_fdt_frame()`: emetti `event_cb(GX_EV_FINGER_DOWN)` anche su 0x80/0x82
  (prima solo 2). Senza questo libfprint non entra in stato CAPTURE e
  `fpi_image_device_image_captured` fa `assertion 'state==CAPTURE' failed`
  (= lo stallo/crash). NB: NON imparare la baseline da un frame reverse.

## 2. goodixgf.c — capacità template enroll 8 -> 30

Problema strutturale del recall: il template teneva max 8 frame, ma il sensore
(80x64 @500DPI = ~4.1x3.25mm/frame, polpastrello ~12x14mm) ne richiede ~30-60
per coprire la zona di pressione. Con 8 frame il match funziona solo centrando
una zona registrata.

- `GF_ENROLL_MIN_STAGES 3 -> 15`, `GF_ENROLL_MAX_STAGES 8 -> 30`,
  `GF_ENROLL_DUP_STOP 2 -> 4`.
- Il buffer `accepted[MAX_STAGES][NPIX]` era inline nella struct GObject: a 30
  supera il limite `instance_size <= G_MAXUINT16` (64KB) e il tipo non si
  registra. Spostato su heap: campo `uint8_t (*accepted)[GF_ENROLL_NPIX]`,
  `g_malloc0` in `gf_activate` (solo se enroll), `g_clear_pointer(g_free)` in
  `gf_deactivate`.

Risultato: enroll fino a 30 frame, convergenza guidata dalla diversità.
Recall ~38%/tocco su tocchi variati (match binario: 0 o >=20, impostori ~0).

## 3. Config sistema (non codice)

- `pam_fprintd.so max-tries=10 timeout=20` in /etc/pam.d/sudo (e greetd) ->
  login a impronta affidabile a più tentativi. Backup: sudo.bak2 / greetd.bak2.

## 4. sigfm/sigfm.cpp — FALSI POSITIVI (fix di sicurezza, 2026-10-24 sera)

Causa misurata col banco offline (`tools/fp_tune`): i frame del driver
contengono un **fantasma fisso sul reticolo dei pixel** (residuo di
guadagno/baseline: strisce verticali + macchie). Nei tocchi deboli domina
l'immagine; SIFT mette i keypoint sul fantasma e due frame qualsiasi, di dita
DIVERSE, matchano con geometria perfetta (spostamento 0) -> impostori a
2000-13000 contro soglia 20. Il 93% di questi match ha spostamento <= 1 px.

- Scarto delle corrispondenze con |p1 - p2| <= 2 px (`min_displacement`):
  un dito vero non si riappoggia mai al sub-pixel, il fantasma sì.
  Dataset 24/10: impostori max 13256 -> **0**; anche su impronte specchiate
  (dita "inesistenti") e sui dati grezzi dell'era Python FAR = 0.
- Bug upstream: `match::operator<` confrontava solo `p1.y`, quindi `std::set`
  buttava tutti i match sulla stessa riga. Ora ordine lessicografico completo.

Costo noto: due frame dello STESSO tocco col dito fermo non si matchano più
(~45% delle coppie in raffica); verify ed enroll sono tocchi distinti.
Non richiede re-enroll (cambia solo il confronto).

## 5. core/goodix_imgproc.c — pipeline "ridge" + dump grezzi

- Nuova `GX_IMGPROC_MODE_RIDGE` (ora default): raw - base in float ->
  de-ghost opzionale (regressione sulla forma della baseline) -> DoG
  (σ 1.0/4.0) -> stretch percentile 1/99 -> CLAHE 2.5 / 4x4. Validata
  bit-a-bit (±1-3 livelli) contro OpenCV/numpy. Sulle immagini ripulite dal
  fantasma è quella con recall migliore a FAR 0.
- `gx_imgproc_signal()`: dc = mean(raw-base), ridge = std DoG (conteggi 12 bit).
  Era Python: dito pieno dc ~ -1200 / ridge 110-175; vuoto dc -45 / ridge 2.
- Dump calibrazione (GOODIX_DUMP_IMGPROC=1): `raw16`, `base16`, `fresh16`,
  `pre16` (PGM 16 bit), `ridge8`. Permettono di rifare offline QUALSIASI
  pipeline col codice C reale (`tools/fp_tune/imgproc_run`).
- Env: GOODIX_IMGPROC_MODE=legacy|ridge, _FRESH_BASE, _DEGHOST, _DOG_S1/_S2,
  _CLAHE, _CLAHE_TILES.

## 6. core/goodix_capture.c + goodixgf.c — baseline fresca + gate qualità

- `gx_capture_fresh_image()`: prima di ogni armo del finger-detect scatta un
  frame diretto senza dito (100 ms dopo il sollevamento) -> `img_fresh`, base
  temporalmente adiacente al tocco. Scartato se sembra un dito (|dc| > 300 vs
  ultima base fresca/archiviata, salvo assenza di struttura a creste), in tal
  caso resta l'ultima fresca valida (raffica a dito premuto). Env
  GOODIX_IMGPROC_FRESH_BASE=0 disattiva, GOODIX_FRESH_MAXDC/_MAXRIDGE.
- Worker: logga `frame signal dc=.. ridge=..`; sotto GOODIX_MIN_RIDGE (10)
  il frame è "riprova" (né registrato né confrontato).
- Bug trovato al primo test (enroll fermo dopo 1 stadio): tra gli stadi
  libfprint fa deactivate/activate; il cancel arrivava con il frame fresco
  già richiesto -> immagine lasciata a metà nel canale -> nessuna immagine
  successiva. Ora quel `wait_image` non è annullabile (dura ~150 ms) e su
  timeout il frame fresco viene disattivato per la sessione (base archiviata).

Diff completo rispetto ai sorgenti di Rockytkg (`src/`): `libfprint-goodixgf.diff`.
Sessione di calibrazione: `tools/fp_tune/calib_session.sh`, analisi:
`tools/fp_tune/analyze_session.py`.

## 7. Validazione su hardware (calib_session.sh, 2026-10-24 22:4x)

Config installata = default del sorgente: RIDGE + baseline fresca + deghost,
SIGFM ratio 0.75 + filtro spostamento, soglia 20, gate ridge 10, enroll 31 stadi.
- Dal vivo: genuini **22/25** (FRR 12%), impostori **0/25** (FAR 0).
- Punteggi (journal): impostori **0 su tutti i 30 frame del template** in tutti
  i 25 tentativi; genuini accettati 29..25689; i 3 rifiuti sono 0 pieno
  (tocco fuori dalla zona registrata).
- Offline (analyze_session.py, stessi grezzi): riproduce FRR 0.12 / FAR 0;
  impronte specchiate max 0. Con ratio 0.8 gli impostori salgono a 1-6 -> 0.75.
- Incrociato: frame dell'era Python (altro driver, 21-22/10) del dito
  registrato riconosciuti (22/30, 11/29), dita diverse 0/13.
- Segnale: dita dc -900..-1450, ridge 98..219; baseline salvata vs frame
  vuoto fresco entro ±6 conteggi. Il frame fresco viene spesso scartato perché
  il dito è già sul sensore all'avvio del verify: si usa la base archiviata,
  con risultati equivalenti.

## TODO (prossima sessione) — vedi memoria windows-enroll-blueprint

- Self-learning (identifyUpdate): fprintd non ripersiste cambiamenti post-verify;
  farlo fare alla GUI come re-enroll esplicito su print esistente (UPDATE_PRINT).
- Enroll overlap-aware (ChooseLessOverlay), verify multi-frame, GUI coverage map.
