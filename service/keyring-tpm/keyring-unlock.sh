#!/usr/bin/env bash
# Sblocca il keyring GNOME al login con la password sigillata nel TPM (PCR 7).
# Pensato per login a impronta (pam_fprintd), quando pam_gnome_keyring non riceve
# la password e il keyring resterebbe bloccato.
# Fallback: se il TPM rifiuta (PCR cambiato), manca il gruppo tss o il bus non
# risponde, esce 0 in silenzio -> il keyring chiede la password come di consueto.
# Imposta GOODIX_KEYRING_LOG=1 per log diagnostici su stderr (journal).
CRED="$HOME/.local/share/goodixfp/keyring-tpm.cred"
UNLOCKER="$HOME/.local/share/goodixfp/gkr_unlock.py"
[ -r "$CRED" ] || exit 0
log() { [ -n "$GOODIX_KEYRING_LOG" ] && echo "[goodix-keyring] $*" >&2 || true; }

# Attendi che il Secret Service (e la collezione 'login') siano sul bus, max ~10s.
for _ in $(seq 1 50); do
  busctl --user get-property org.freedesktop.secrets \
    /org/freedesktop/secrets/collection/login \
    org.freedesktop.Secret.Collection Locked >/dev/null 2>&1 && break
  sleep 0.2
done

PW="$(systemd-creds decrypt --name=gkr "$CRED" - 2>/dev/null)" || { log "decrypt fallita"; exit 0; }
[ -n "$PW" ] || exit 0
printf '%s' "$PW" | python3 "$UNLOCKER" >/dev/null 2>&1 || log "unlock fallito"
unset PW
exit 0
