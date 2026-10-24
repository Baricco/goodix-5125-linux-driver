# goodix-5125 — impronta digitale su Linux per Goodix 27c6:5125

Login con l'impronta su Linux per il lettore **Goodix `27c6:5125`** (e
`27c6:5135`) integrato nel tasto di accensione dei portatili Huawei MateBook /
Honor MagicBook. Si integra nello stack standard: **fprintd + pam_fprintd**,
quindi funziona con `sudo`, greeter e blocco schermo, con la password sempre
disponibile come alternativa.

**Stato:** funzionante e misurato sul sensore reale. 0/25 dita sbagliate
accettate, 22/25 tocchi del dito registrato riconosciuti al primo colpo. Vedi
[come funziona e perché è sicuro](docs/how-it-works.md).

## Cosa contiene

| | |
|---|---|
| [`packaging/`](packaging) | Pacchetto Arch **`libfprint-goodixgf`**: libfprint ufficiale + matcher SIGFM + driver goodixgf, con aggiornamento guidato a ogni nuova libfprint |
| [`gui/`](gui) | **Gestore impronte** (PySide6): registra, prova, cancella le impronte, per dito e per utente |
| [`service/keyring-tpm/`](service/keyring-tpm) | Facoltativo: sblocco del portachiavi GNOME al login con impronta, password sigillata nel TPM |
| [`docs/`](docs) | Guida d'installazione, BIOS, Secure Boot, recupero del sensore, funzionamento, protocollo |
| [`tools/fp_tune/`](tools/fp_tune) | Banco di taratura: sessione al sensore con dump grezzi e analisi offline col codice reale del driver |
| [`lab/`](lab) | Il laboratorio di reverse engineering (prototipo Python) da cui è nato il progetto |

## Installazione rapida (Arch / CachyOS)

```sh
cd packaging/libfprint-goodixgf && makepkg -si     # sostituisce libfprint
sudo systemctl enable goodixgf-warmup.service
fprintd-enroll -f right-index-finger               # oppure: python gui/fingerprint_manager.py
```

Poi aggiungi `auth sufficient pam_fprintd.so max-tries=10 timeout=20` in cima
ai file PAM che ti interessano. La procedura completa, con le regole
anti-lockout e la risoluzione dei problemi, è nella
**[guida passo passo](docs/INSTALL.md)**.

## Documentazione

- [Guida d'installazione e d'uso](docs/INSTALL.md) — dal pacchetto al login, aggiornamenti, problemi
- [Come funziona](docs/how-it-works.md) — catena, matcher, il problema dei falsi positivi e le difese, risultati
- [BIOS](docs/bios.md) — impostazioni rilevanti (Secure Boot, TPM, virtualizzazione)
- [Secure Boot su CachyOS + limine](docs/secure-boot.md)
- [Recupero del sensore bloccato](docs/recovery.md)
- [Sicurezza complessiva](SECURITY.md) — impronta, keyring, cifratura del disco
- [Protocollo del sensore](docs/protocol.md) e [storia del matching](docs/matching.md)

## Sicurezza del sensore

Il driver scrive nel sensore solo la propria chiave per il canale cifrato
(area PSK, reversibile). **Non contiene e non scrive mai firmware.** Il
firmware Goodix, le DLL Windows, le catture USB e le chiavi restano fuori dal
repository.

## Crediti

- [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) e la merge
  request SIGFM !530 (Matthieu Charette, Natasha England-Elbro, Timur Mangliev).
- [Rockytkg/goodix-linux-27c6-5125](https://github.com/Rockytkg/goodix-linux-27c6-5125)
  — driver goodixgf, base delle nostre modifiche.
- [goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev) — goodix-fp-dump, SIGFM.
- sorguido / grold (goodix-27c6-5125, LGPL-2.1) — algoritmo di provisioning PSK.

## Licenza

[GPL-2.0-or-later](LICENSE). Le patch a libfprint mantengono le licenze dei
file su cui si applicano (LGPL-2.1-or-later per libfprint e SIGFM,
GPL-2.0-or-later per il core goodixgf). Reverse engineering a fini di
interoperabilità su hardware proprio.
