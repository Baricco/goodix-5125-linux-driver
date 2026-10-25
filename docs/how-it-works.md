# Come funziona

## La catena

```
sensore Goodix (nel tasto di accensione)
   │  USB CDC, canale TLS-PSK (AES-128-GCM, OpenSSL) con chiave scritta da noi
   ▼
goodixfpd  (al posto del demone di fprintd, stessa interfaccia D-Bus)
   │  finger-detect (FDT) -> immagine 80×64 a 12 bit
   │  pipeline immagine "ridge" -> immagine 8 bit delle creste
   │  SIFT + voto geometrico SIGFM (in C)    impronta = 15-30 frame del dito
   ▼
net.reactivated.Fprint  ──  pam_fprintd / fprintd-* / GNOME / GUI
                            sudo, greeter, blocco schermo
```

Il sensore è minuscolo: un frame copre circa **4×3 mm** di dito a 500 DPI. Una
singola immagine contiene troppo poco per i matcher a minuzie classici, perciò
si usa l'algoritmo **SIGFM**: estrae punti SIFT e conta le coppie di
corrispondenze la cui geometria (distanze e angoli) è coerente con una
rototraslazione del dito. L'**impronta** salvata è un insieme di 15-30 frame
presi in punti leggermente diversi della zona che usi. Un tocco viene accettato
se combacia con almeno uno di essi oltre la soglia (20).

## Perché un servizio a parte e non un driver dentro libfprint

La prima versione era un driver dentro libfprint, con il matcher SIGFM preso
da una merge request non ancora accettata (!530). Funzionava, ma voleva dire
**modificare una libreria di sistema**: a ogni nuova libfprint bisognava
riapplicare una ventina di patch, con il rischio che smettessero di applicarsi.

Il codice del sensore però non dipendeva da libfprint. Così ora vive in un
servizio, `goodixfpd`, che implementa lo stesso contratto di fprintd: stessi
metodi, segnali, errori e controlli polkit (`net.reactivated.fprint.device.*`).
Lo stesso fprintd della distribuzione resta installato: fornisce `pam_fprintd`,
i comandi `fprintd-*`, la descrizione dell'interfaccia e le regole polkit. Un
drop-in di `fprintd.service` avvia `goodixfpd` al posto del suo demone. Quando
il lettore Goodix manca, `goodixfpd` avvia il demone originale.

Risultato: **nessuna libreria modificata**. Gli aggiornamenti di libfprint e
fprintd arrivano come per tutti, e il progetto non dipende dall'accettazione di
patch altrui. SIFT e il voto geometrico sono riscritti in C (niente OpenCV), il
canale TLS usa OpenSSL (niente mbedtls).

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

1. **Matcher: niente corrispondenze ferme.** Il confronto scarta le coppie di
   punti distanti ≤ 2 pixel. Un dito vero non si riappoggia mai al pixel
   esatto; il fantasma sì. Questa difesa da sola porta gli impostori a 0 su
   tutti i dataset. Corretto anche un bug di SIGFM upstream: l'ordinamento
   confrontava solo la coordinata y e scartava corrispondenze valide.
2. **Immagine delle creste.** Al posto di stretch + nitidezza, che amplificava
   il fantasma, la pipeline fa: `raw − baseline` in virgola mobile, rimozione
   per regressione della componente con la forma della baseline (guadagno),
   filtro passa-banda DoG sulla frequenza delle creste (σ 1/4 px), stretch e
   CLAHE.
3. **Baseline fresca.** Prima di armare il finger-detect il servizio scatta un
   frame senza dito, che diventa la baseline del tocco successivo. Viene
   scartato se sembra contenere un dito; in quel caso si usa l'ultima baseline
   valida.
4. **Controllo qualità.** I frame con contrasto delle creste troppo basso
   (`ridge` < 10; un tocco pieno è 100–200) diventano "riprova" invece di
   essere registrati o confrontati.

## Risultati misurati (sensore reale, 24/10/2026)

Misurati dal vivo con la versione precedente (driver in libfprint, SIGFM su
OpenCV). Template di 31 tocchi, poi 25 verifiche col dito registrato e 25 con
altre quattro dita:

| | Esito |
|---|---|
| Dito registrato | **22/25 accettati** (FRR 12%) |
| Altre dita | **0/25 accettate** (FAR 0) |
| Punteggio impostori | **0** contro tutti i 30 frame del template, in tutti i tentativi |
| Impronte specchiate (dita "inesistenti", stress test) | 0 |
| Frame di 2–3 giorni prima, altro driver | dito registrato riconosciuto, altre dita 0 |

I 3 rifiuti sono tocchi caduti fuori dalle zone registrate (punteggio 0 pieno,
non casi al limite). Al login `max-tries=10` li assorbe: si ritocca.

`goodixfpd` è stato verificato offline sugli stessi frame grezzi di quella
sessione. La sua pipeline immagine riproduce **bit per bit** le 81 immagini di
produzione, e il confronto in C dà **le stesse decisioni**: 22/25 genuini,
0/25 impostori. Su 1550 coppie frame-frame una sola cambia esito, e nessuna
cambia l'esito di una verifica.

## L'impronta che migliora con l'uso

Il driver Windows aggiorna il template a ogni riconoscimento riuscito. Lo fa
anche `goodixfpd`: un tocco riconosciuto con margine (punteggio ≥ 40, il doppio
della soglia), che però solo pochi frame riconoscono (al più 3, cioè un tocco
al bordo della zona coperta), viene aggiunto all'impronta. Così un tocco
appena più in là la volta dopo trova qualcosa con cui combaciare, e la
copertura cresce con l'uso.

Limiti scelti per sicurezza:
- i frame della registrazione **non vengono mai sostituiti**, quindi l'impronta
  non può "scivolare" lontano dal dito registrato;
- i frame imparati sono al massimo 30 e si rinnovano dal più vecchio;
- si impara solo da verifiche, mai dal tocco di controllo dei duplicati, e
  solo con un margine doppio rispetto alla soglia;
- `GOODIX_LEARN=0` lo disattiva.

Sui dati della sessione l'apprendimento non cambia nessun esito: gli impostori
restano 0/25 anche contro l'impronta arricchita.

## Riproducibilità

Ogni modifica a matcher o pipeline si misura **prima** di toccare il sensore.
I test di `goodixfpd/` (`meson test`) controllano:

- `tls`: il canale TLS-PSK contro un client che fa la parte del sensore
  (handshake, dati, riconnessione, chiave sbagliata rifiutata);
- `template`: formato su disco, archivio, regole d'apprendimento. Con
  `-Ddataset=<cartella>` di catture reali anche pipeline bit per bit, FRR e FAR;
- `dbus`: 16 scenari contro i client veri. Bus privato, polkit finto, sensore
  simulato; `fprintd-list`, `fprintd-verify` e il vero `pam_fprintd.so` (via
  `pam_start_confdir`, senza toccare `/etc/pam.d`).

[`tools/fp_tune/`](../tools/fp_tune) resta il banco usato per la taratura
originale (sessione al sensore con dump grezzi, analisi con lo SIGFM OpenCV).
