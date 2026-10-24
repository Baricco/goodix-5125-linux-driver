# packaging — libfprint-goodixgf

Pacchetto Arch che **sostituisce** `libfprint`: sorgente ufficiale della
versione dei repository (oggi **1.94.100**) più una serie di patch:

| Patch | Origine | Cosa fa |
|---|---|---|
| `0001`–`0019` | merge request upstream [libfprint !530](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/530) (ribasata il 10/10/2026) | aggiunge SIGFM, il matcher SIFT + voto geometrico per sensori piccoli (escluso il commit che converte il driver `elan`) |
| `0020` | questo progetto | SIGFM: scarta le corrispondenze ancorate ai pixel del sensore; ordinamento corretto dei match |
| `0021` | questo progetto, sul driver goodixgf di [Rockytkg](https://github.com/Rockytkg/goodix-linux-27c6-5125) | driver per 27c6:5125/5135: TLS-PSK, finger-detect 0x80/0x82, enroll fino a 30 frame, pipeline "ridge", baseline fresca, controllo qualità; **nessun firmware incorporato** (la versione si confronta soltanto, non si flasha mai) |

Il pacchetto installa anche:

- `goodixgf-warmup.service` — cancella la baseline al boot, così il driver la
  ricampiona a freddo;
- un drop-in di fprintd con `--no-timeout`, per tenere fprintd residente;
- un hook di pacman che avvisa quando nei repo esce una libfprint diversa da
  quella a cui il pacchetto è allineato (nuova versione o nuovo pkgrel, cioè
  correzioni della distribuzione) e quando la libreria non si carica più.

`goodixgf-update` segue il pacchetto libfprint della distribuzione: sulla
versione upstream (`_upstream`) applica prima le correzioni del PKGBUILD
ufficiale Arch per quel pkgrel (`_distro_patches`, `_distro_cherrypicks`,
lette dal testo del PKGBUILD senza eseguirlo), poi le nostre patch.
`_distrover` registra la versione della distribuzione di riferimento ed è
installata in `/usr/share/libfprint-goodixgf/base-version`.

## Uso

```sh
cd libfprint-goodixgf && makepkg -si     # prima installazione
../goodixgf-update                        # dopo un nuovo rilascio di libfprint
```

## Rigenerare le patch

Le patch si esportano da un ramo git sopra il tag ufficiale:

```sh
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git && cd libfprint
git checkout -b goodixgf-1.94.100 v1.94.100
git fetch origin merge-requests/530/head:mr530
git cherry-pick <commit SIGFM di mr530, escluso "elan: switch to sigfm">
git am <patch 0020 e 0021 di questo repo>
# ... modifiche ...
git format-patch v1.94.100.. -o <repo>/packaging/libfprint-goodixgf/
```

Poi aggiorna l'elenco `source` e i checksum (`makepkg -g`) nel PKGBUILD.

Quando arriva una nuova libfprint, `goodixgf-update` prova ad applicare le patch
così come sono. Se falliscono, il ramo va ribasato sul nuovo tag
(`git rebase --onto v<nuova> v1.94.100`) e le patch riesportate.

## Note tecniche sul driver

- Dati di calibrazione: un dito pieno abbassa il segnale di circa 1000–1450
  conteggi (`dc`); il contrasto delle creste dopo il passa-banda (`ridge`) è
  98–219. Un frame vuoto ha `dc` ~ −45 e `ridge` ~ 2.
- La baseline salvata in `/var/lib/fprint/goodix/goodix.dat` coincide col frame
  vuoto fresco entro ±6 conteggi a sensore stabile.
- Variabili d'ambiente per la taratura (drop-in di fprintd): `GOODIX_MIN_RIDGE`,
  `GOODIX_IMGPROC_MODE=legacy|ridge`, `GOODIX_IMGPROC_FRESH_BASE`,
  `GOODIX_IMGPROC_DEGHOST`, `GOODIX_IMGPROC_DOG_S1/_S2`, `GOODIX_IMGPROC_CLAHE`,
  `GOODIX_FRESH_MAXDC`, `GOODIX_DUMP_IMGPROC=1` (dump grezzi, solo per taratura).
