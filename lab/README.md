# lab — laboratorio di reverse engineering

Il prototipo in Python con cui è stato decifrato il sensore, prima del driver
libfprint. **Non serve per usare l'impronta** (per quello: `packaging/` e
`docs/INSTALL.md`). Resta utile per studiare il protocollo e per fare debug a
basso livello.

| File | Cosa fa |
|---|---|
| `driver_5125.py` | provisioning PSK (E0), sessione TLS-PSK, acquisizione immagine |
| `pairing.py` | busta PSK `0xbb010003` (algoritmo pubblico di sorguido/grold) |
| `sensor.py`, `sensor_recover.py`, `usb_reset.py` | accesso al sensore, self-healing, reset |
| `preprocessor.py`, `sigfm.py`, `template.py`, `coverage.py` | preprocessing e matcher SIGFM in Python |
| `fragment_mapper.py`, `mosaic*.py`, `posegraph.py`, `enroll_gui.py` | enroll per copertura con mappa a mosaico |
| `daemon/` | il primo autenticatore (demone `goodixfpd` + `pam_exec`), superato da fprintd |
| `docs/` | note storiche: integrazione PAM del demone, porting C, migrazione a fprintd |

I file in `docs/` descrivono strade poi abbandonate e possono citare percorsi
vecchi.

Dipendenze: `goodix-fp-dump/` nella radice del repo (non incluso, va clonato da
goodix-fp-linux-dev) e `pip install -r lab/requirements.txt`. I dati locali
(`state/`, `captures/`) restano nella radice e non vengono mai pubblicati.

Attenzione: `driver_5125.py --write` riscrive la PSK del sensore. Dopo averlo
usato, il driver libfprint la riscrive di nuovo da solo al primo avvio.
