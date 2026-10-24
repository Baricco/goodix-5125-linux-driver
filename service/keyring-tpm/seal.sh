#!/usr/bin/env bash
# Sigilla la password del keyring GNOME nel TPM (solo-TPM, legata a PCR 7).
# Eseguibile via sudo: il file risultante viene assegnato all'utente reale.
# La password viene letta a runtime e non viene mai stampata.
set -euo pipefail

TARGET_USER="${SUDO_USER:-$USER}"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
CRED="$TARGET_HOME/.local/share/goodixfp/keyring-tpm.cred"
mkdir -p "$(dirname "$CRED")"

read -rsp "Password del keyring (= password di login): " PW; echo

# -T = solo TPM (nessuna host key -> decifrabile da utente in gruppo tss),
# legato allo stato di PCR 7 (Secure Boot). Richiede Secure Boot ATTIVO per
# avere valore reale (altrimenti un live USB con SB off riproduce lo stesso PCR7).
printf '%s' "$PW" | systemd-creds encrypt -T --tpm2-pcrs=7 --name=gkr - "$CRED"
chmod 600 "$CRED"
chown "$TARGET_USER:$TARGET_USER" "$CRED" 2>/dev/null || true
chown "$TARGET_USER:$TARGET_USER" "$(dirname "$CRED")" 2>/dev/null || true

# Verifica roundtrip (non stampa la password).
DEC="$(systemd-creds decrypt --name=gkr "$CRED" - 2>/dev/null || true)"
if [ -n "$DEC" ] && [ "$DEC" = "$PW" ]; then
  echo "OK: sigillo creato e verificato ($CRED)"
else
  echo "ERRORE: roundtrip fallito (controllare TPM/PCR7)"; exit 1
fi
unset PW DEC
