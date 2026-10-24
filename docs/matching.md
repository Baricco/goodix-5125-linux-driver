# Matching con SIGFM: stato reale

Il riconoscimento usa **SIGFM** ("SIFT Is Good For Matching"), l'algoritmo di
goodix-fp-linux-dev pensato proprio per sensori 64x80. Port fedele in `lab/sigfm.py`
(da `cpp/match.cpp`/`compute.cpp` del repo upstream), template multi-frame in `lab/template.py`.

## Perche' la prima versione (RANSAC) falliva

Il primo tentativo (`lab/match.py`) usava SIFT + `findHomography(RANSAC)`: fit di UNA
trasformazione globale, score = n. inlier. Su 64x64 con pochi match rumorosi RANSAC non fitta
nulla di stabile -> score 0 sia per genuino sia per impostore, nessuna soglia separa.

SIGFM invece fa un **voto geometrico a coppie, invariante a rotazione e scala**:
1. SIFT + ratio test di Lowe (0.75) -> match tentativi.
2. per ogni coppia di match, se la distanza tra i keypoint e' conservata entro il 5%
   (LENGTH_MATCH) registra un "angolo" (sin/cos della rotazione relativa).
3. per ogni coppia di angoli, se sin e cos coincidono entro il 5% (ANGLE_MATCH) +1 allo score.

Lo score e' il conteggio combinatorio di coppie coerenti: enorme per lo stesso dito con
sovrapposizione, ~0 per dita diverse. Degrada con grazia (anche overlap parziale lascia un
cluster coerente), al contrario del fit a omografia singola.

## Preprocessing

`lab/preprocessor.py` (sottrazione baseline clear + normalizzazione per colonna + CLAHE)
batte nettamente il min-max grezzo del demo upstream. Misurato (template-max, dati reali):

| probe                     | min-max grezzo | column-norm + CLAHE |
|---------------------------|---------------:|--------------------:|
| A2 (genuino, con overlap) |         36 585 |         **251 661** |
| B1 (impostore)            |            228 |              **49** |

CLAHE rafforza il genuino ~7x e abbatte il leakage impostore ~5x.

## Risultati misurati (hardware reale: dito A genuino, dito B impostore)

Template = raffica Aenr_00..09; verify = max score sul template, soglia 300:

- A2 (genuino, cade su una zona coperta) -> **ACCETTATO**, score 251661.
- B1, Bver (impostore) -> **RIFIUTATO**, score 49 e 0.
- A1, Aver (genuino, ma zone NON coperte dalla raffica) -> RIFIUTATO, score 0.

Velocita': ~1.35 ms medi per confronto (max ~80 ms nel caso combinatorio di immagini
quasi identiche). Verify con template da 10-15 frame resta ben sotto i 100 ms.

## Il vincolo vero: COPERTURA in enroll

I falsi rifiuti (A1, Aver) non sono un difetto del matcher: SIGFM trova (giustamente) overlap
quando c'e' e 0 quando non c'e'. Gli Aenr sono una raffica ravvicinata su una sola zona del
dito; i tocchi indipendenti che cadono altrove non hanno nulla con cui sovrapporsi.

La soluzione e' l'**enrollment guidato per copertura** (stile Windows Hello): 12-15 tocchi
distribuiti su tutto il polpastrello (punta, sinistra, destra, base, centro), cosi' che ogni
tocco futuro si sovrapponga ad almeno un frame registrato. Con quel dataset:
1. ri-costruire il template (`lab/template.py enroll`),
2. tarare la soglia sulla separazione genuino/impostore reale (ora provvisoria a 300).

## Prossimo passo operativo

Sessione di cattura su hardware (la lancia l'utente): `lab/driver_5125.py --enroll 15 --tag A`
muovendo il dito per coprire tutta la superficie, poi alcuni tocchi di verifica genuini + un
paio con un altro dito. Il template e la taratura si fanno offline dai `*_plain.bin` salvati.

Il resto della catena (protocollo, provisioning PSK, TLS, acquisizione, decodifica) e' gia'
verificato e funzionante.

## Mosaico incrementale (riduzione falsi negativi)

Su mappatura densa reale (~113 zone): falsi positivi 0 (SIGFM selettivo), ma restano falsi
negativi quando il tocco di verifica cade in un BUCO di copertura. Le zone adiacenti pero' SI
REGISTRANO tra loro (roto-traslazione, offset reali 15-50px, rotazioni <6deg): un mosaico di 8
zone connesse si cuce correttamente con le creste continue.

Il limite non e' il dito ma l'enroll a punti: `SAME_SPOT` crea una zona nuova solo quando
l'overlap e' gia' perso, quindi butta i frame intermedi che servirebbero a cucire (componente
connessa massima ~8-15 su 113). Soluzione: enroll a MOSAICO INCREMENTALE (`lab/mosaic.py`,
`lab/mosaic_session.py`): si tiene il flusso denso, ogni frame si registra sulla mappa corrente
e la estende solo se si aggancia; i buchi (aree dentro l'inviluppo non coperte) emergono dalla
copertura e guidano la mappatura. La GUI (`lab/enroll_gui.py`) mostra il mosaico live coi buchi
in rosso. La verifica resta sullo score SIGFM (0 falsi positivi) piu' gli inlier di registrazione.
