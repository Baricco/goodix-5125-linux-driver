# Sicurezza

Il login a impronta cambia *come* ti autentichi, non *cosa* protegge i tuoi dati.
Per una postura "come Windows" servono tre livelli, indipendenti tra loro.

## Livello 1 — Autenticazione a impronta (questo progetto)

`pam_fprintd` su `sudo`, greeter e lockscreen. L'impronta è un **gate sì/no**: non
rilascia chiavi. Il resto del sistema resta protetto dalle sue credenziali come
prima. Config con fallback password (`auth sufficient`) → nessun lock-out.

## Livello 2 — Sblocco del keyring via TPM (opzionale)

`service/keyring-tpm/`. Con il login a impronta, `gnome-keyring` non riceve la
password e resterebbe bloccato. La password del keyring viene **sigillata nel
TPM**, legata a **Secure Boot (PCR 7)**, e sbloccata al login. È il modo in cui
Windows evita di richiedere di nuovo le credenziali. **Richiede Secure Boot
attivo** (`docs/secure-boot.md`), altrimenti il sigillo PCR 7 non protegge.
Fallback: se il TPM rifiuta, il keyring chiede la password come sempre.

## Livello 3 — Cifratura del disco (LUKS + TPM) — **consigliata, non inclusa**

Il login a impronta e lo sblocco del keyring **non** proteggono i dati a riposo:
chi ha il portatile può leggere il disco. Per protezione reale serve **LUKS**,
sigillato al TPM come BitLocker. Non è parte di questi script (un installer di
driver non deve cifrare il disco altrui): si fa con strumenti di sistema.

Si può fare **in-place senza reinstallare** con `cryptsetup reencrypt --encrypt`
(solo la partizione root; `/boot` resta in chiaro), e si auto-sblocca all'avvio
con `systemd-cryptenroll --tpm2-device=auto --tpm2-pcrs=7`. Avvertenze:

- **Fai un backup dei dati irrecuperabili prima.** La reencryption riscrive tutta
  la partizione; è resumabile ma non a rischio zero.
- Tieni **sempre** una passphrase oltre al TPM: un update di firmware/Secure Boot
  cambia PCR 7 e il TPM non sblocca più → la passphrase evita il lock-out.
- Usa lo **stesso meccanismo PCR** del Livello 2, quindi va fatto con Secure Boot
  già attivo per avere senso.

## Divulgazione di vulnerabilità

Problemi di sicurezza: aprire una issue privata / contattare il maintainer prima
della divulgazione pubblica.
