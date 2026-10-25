#!/usr/bin/env bash
# Sblocca il keyring GNOME con la password sigillata nel TPM (PCR 7).
# Pensato per login a impronta (pam_fprintd), quando pam_gnome_keyring non riceve
# la password e il keyring resterebbe bloccato. Gira prima della sessione grafica
# e a ogni (ri)avvio di gnome-keyring-daemon (vedi goodix-keyring-unlock.service).
# Fallback: se il TPM rifiuta (PCR cambiato), manca il gruppo tss o il daemon non
# risponde, esce 0 -> il keyring chiede la password come di consueto.
# Esito nel journal: journalctl --user -b -u goodix-keyring-unlock
#
# NB: niente sonde sulla collezione (es. busctl get-property ... Locked).
# gnome-keyring registra i client in modo asincrono e una lettura di proprieta'
# arrivata troppo presto lo fa abortire: il daemon riparte BLOCCATO. L'attesa del
# Secret Service la fa gkr_unlock.py, solo con chiamate di metodo.
CRED="$HOME/.local/share/goodixfp/keyring-tpm.cred"
UNLOCKER="$HOME/.local/share/goodixfp/gkr_unlock.py"
log() { echo "[goodix-keyring] $*" >&2; }
[ -r "$CRED" ] || { log "nessun sigillo in $CRED, salto"; exit 0; }

PW="$(systemd-creds decrypt --name=gkr "$CRED" - 2>/dev/null)" ||
  { log "il TPM non rilascia la password (PCR 7 cambiato? gruppo tss?)"; exit 0; }
[ -n "$PW" ] || exit 0
if printf '%s' "$PW" | python3 "$UNLOCKER"; then
  log "keyring sbloccato"
else
  log "sblocco fallito, il keyring chiedera' la password"
fi
unset PW
exit 0
