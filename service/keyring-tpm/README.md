# keyring-tpm — auto-sblocco del keyring GNOME via TPM (opzionale)

Quando accedi con l'**impronta** invece che con la password, `pam_gnome_keyring`
non riceve la password e il keyring GNOME resta bloccato: al primo programma che
lo usa ti chiede la password. Questo componente lo risolve **come fa Windows con
il TPM**: la password del keyring viene **sigillata nel TPM**, legata allo stato
di **Secure Boot (PCR 7)**, e uno sblocco al login la recupera dal TPM.

È **opzionale** e **indipendente** dal driver impronte: puoi installarlo o no.

## Come funziona

1. La password del keyring (= la tua password di login) viene cifrata con
   `systemd-creds -T --tpm2-pcrs=7` → file `~/.local/share/goodixfp/keyring-tpm.cred`.
   `-T` = solo TPM (nessuna host key), così è decifrabile dall'utente nel gruppo
   `tss`. Legata a PCR 7: se qualcuno avvia un altro OS, il TPM non la rilascia.
2. Al login, un servizio utente systemd (`goodix-keyring-unlock.service`)
   dissigilla la password dal TPM e sblocca il keyring via D-Bus Secret Service
   (`UnlockWithMasterPassword`).
3. Una regola polkit consente la `systemd-creds decrypt` senza prompt anche
   quando la sessione non è ancora "attiva" per polkit.

**Fallback sicuro:** se il TPM rifiuta (es. dopo un cambio di Secure Boot/chiavi
→ PCR 7 diverso), lo sblocco fallisce in silenzio e il keyring chiede la password
come sempre. La password del keyring resta la tua password di login → **mai
lock-out**. Dopo un cambiamento di PCR 7 basta ri-eseguire `seal.sh`.

## Requisiti

- **Secure Boot ATTIVO** (altrimenti PCR 7 non protegge: un live USB con SB off
  riprodurrebbe lo stesso valore). Vedi `../../docs/secure-boot.md`.
- TPM 2.0 (`/dev/tpmrm0`), `systemd-creds`, `gnome-keyring`, `python3`+`dbus`.

## Installazione

```sh
./install.sh      # come utente normale (usa sudo dove serve)
# poi LOGOUT completo + rientro (attiva il gruppo tss)
```

Disinstallazione: `./uninstall.sh`.

## File

- `gkr_unlock.py` — sblocca il keyring via D-Bus (letta la password da stdin).
- `keyring-unlock.sh` — hook di login: dissigilla dal TPM + chiama l'unlocker.
- `goodix-keyring-unlock.service` — servizio utente (WantedBy=default.target).
- `seal.sh` — (ri)crea il sigillo nel TPM.
- `49-goodix-keyring-creds.rules.in` — regola polkit (template, `@USER@`).

## Nota sicurezza

La regola polkit consente all'utente di decifrare credenziali di sistema senza
autenticazione. Su una macchina single-user dove l'utente ha già `sudo` non è
un'escalation reale. Su sistemi multi-utente valutare se restringerla o evitarla.
