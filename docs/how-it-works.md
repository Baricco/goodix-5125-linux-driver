# Come funziona

## La catena

```
sensore Goodix (nel tasto di accensione)
   │  USB CDC, canale TLS-PSK (AES-128-GCM) con chiave scritta da noi
   ▼
driver goodixgf (dentro libfprint)
   │  finger-detect (FDT) -> immagine 80×64 a 12 bit
   │  pipeline immagine "ridge"  -> immagine 8 bit delle creste
   ▼
SIGFM (matcher in libfprint)        template = ~30 frame del dito
   ▼
fprintd  ──  pam_fprintd  ──  sudo / greeter / blocco schermo
```

Il sensore è minuscolo: un frame copre circa **4×3 mm** di dito a 500 DPI. Una
singola immagine contiene troppo poco per i matcher a minuzie classici, perciò
si usa **SIGFM**. SIGFM estrae punti SIFT e conta le coppie di corrispondenze la
cui geometria (distanze e angoli) è coerente con una rototraslazione del dito.
Il **template** è un insieme di circa 30 frame presi in punti leggermente
diversi della zona che usi. Un tocco viene accettato se combacia con almeno uno
di essi oltre la soglia (20).

## Il problema che c'era: il fantasma fisso del sensore

Il primo dataset di taratura mostrava impostori con punteggi da **2 000 a 13 000**
contro una soglia di 20. Cioè dita sbagliate accettate.

La causa: ogni frame contiene un **disegno fisso legato ai pixel del sensore**
(strisce verticali e macchie), il residuo di guadagno e baseline del sensore.
Nei tocchi deboli domina l'immagine. Siccome sta sempre agli stessi pixel, i
punti SIFT del fantasma di un frame combaciano perfettamente con quelli di
qualsiasi altro frame, di qualsiasi dito. Il 93% di queste false corrispondenze
ha spostamento ≤ 1 pixel.

## Le difese

1. **Matcher: niente corrispondenze ferme.** SIGFM scarta le coppie di punti
   distanti ≤ 2 pixel. Un dito vero non si riappoggia mai al pixel esatto; il
   fantasma sì. Questa difesa da sola porta gli impostori a 0 su tutti i
   dataset. Corretto anche un bug di SIGFM upstream: l'ordinamento confrontava
   solo la coordinata y e scartava corrispondenze valide.
2. **Immagine delle creste.** Al posto di stretch + nitidezza, che amplificava
   il fantasma, la pipeline fa: `raw − baseline` in virgola mobile, rimozione
   per regressione della componente con la forma della baseline (guadagno),
   filtro passa-banda DoG sulla frequenza delle creste (σ 1/4 px), stretch e
   CLAHE.
3. **Baseline fresca.** Prima di armare il finger-detect il driver scatta un
   frame senza dito, che diventa la baseline del tocco successivo. Viene
   scartato se sembra contenere un dito; in quel caso si usa l'ultima baseline
   valida.
4. **Controllo qualità.** I frame con contrasto delle creste troppo basso
   (`ridge` < 10; un tocco pieno è 100–200) diventano "riprova" invece di
   essere registrati o confrontati.

## Risultati misurati (sensore reale, 24/10/2026)

Template di 31 tocchi. Poi 25 verifiche col dito registrato e 25 con altre
quattro dita:

| | Esito |
|---|---|
| Dito registrato | **22/25 accettati** (FRR 12%) |
| Altre dita | **0/25 accettate** (FAR 0) |
| Punteggio impostori | **0** contro tutti i 30 frame del template, in tutti i tentativi |
| Impronte specchiate (dita "inesistenti", stress test) | 0 |
| Frame di 2–3 giorni prima, altro driver | dito registrato riconosciuto, altre dita 0 |

I 3 rifiuti sono tocchi caduti fuori dalle zone registrate (punteggio 0 pieno,
non casi al limite). Al login `max-tries=10` li assorbe: si ritocca.

## Riproducibilità

Ogni modifica a matcher o pipeline si misura **prima** di toccare il sensore,
con [`tools/fp_tune/`](../tools/fp_tune):

- `calib_session.sh` — una sessione al sensore: enroll, 25 genuini, 25 impostori,
  con dump grezzi a 16 bit;
- `imgproc_run` — la pipeline C **reale** del driver su quei grezzi;
- `libsigfm_prod.so` — lo SIGFM di produzione, chiamabile da Python;
- `analyze_session.py` — rifà enroll→verify per ogni variante e soglia.

## Cosa resta da migliorare

- **Aggiornamento del template con l'uso** (come Windows): i tocchi riconosciuti
  con margine ampio arricchiscono il template. fprintd oggi non salva il
  template dopo un verify, quindi va fatto dentro la libreria.
- Upstream: SIGFM è nella merge request libfprint !530, ancora aperta; il
  driver goodixgf ha senso upstream solo dopo SIGFM.
