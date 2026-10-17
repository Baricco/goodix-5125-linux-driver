# Matching: stato onesto (NON funzionante)

Il riconoscimento NON e' risolto in questo prototipo. Documentato qui per onesta' e per
indicare la via giusta.

## Cosa e' stato provato

- Preprocessing: sottrazione baseline (frame clear) + normalizzazione per colonna + CLAHE
  (`tools/preprocessor.py`); tentativo di esaltazione Gabor (`tools/enhance.py`, peggiorava).
- Matcher stile SIGFM: SIFT + ratio test di Lowe + verifica geometrica RANSAC affine
  (`tools/match.py`, `tools/match_template.py`), con template multi-frame (enroll a 10 tocchi).

## Risultati misurati (hardware reale, un dito A, un dito B)

- Cross-match tra frame di enroll quasi sovrapposti: 35-97 inlier (alto).
- Verifiche INDIPENDENTI contro il template (soglia tentata 15):
  - genuino Aver = 0, A2 = 2
  - impostore Bver = 0, B1 = 0
- Genuino e impostore collassano entrambi verso 0: **nessuna soglia separa**.

## Perche'

Sensore piccolo (80x64): ogni tocco e' una zona minuscola. Due pressioni indipendenti dello
stesso dito hanno poca sovrapposizione; i descrittori SIFT sulla texture di creste parallele
non sono abbastanza distintivi/stabili tra pressioni (pressione, umidita', rotazione). I
punteggi alti ottenuti erano tra frame quasi identici, non tra tocchi reali ripetuti.

## Via corretta (lavoro futuro)

1. Esaltazione delle creste basata sull'orientamento (Gabor allineato ben tarato) o un
   estrattore di minuzie, per descrittori stabili.
2. La libreria SIGFM reale (quella richiamata da grold), non una reimplementazione SIFT.
3. Enrollment guidato per copertura con feedback (come Windows Hello, 15-20 tocchi) e taratura
   della soglia su un dataset reale.
4. In libfprint: `nr_enroll_stages` alto e matcher dedicato per sensori piccoli.

Il resto della catena (protocollo, provisioning PSK, TLS, acquisizione, decodifica) e'
invece verificato e funzionante.
