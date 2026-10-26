#!/usr/bin/env bash
# Unlocks the GNOME keyring with the password sealed in the TPM (PCR 7).
# Meant for fingerprint login (pam_fprintd), when pam_gnome_keyring does not
# receive the password and the keyring would stay locked. Runs before the
# graphical session and at every (re)start of gnome-keyring-daemon (see
# goodix-keyring-unlock.service).
# Fallback: if the TPM refuses (PCR changed), the tss group is missing or the
# daemon does not answer, it exits 0 -> the keyring asks for the password as usual.
# Result in the journal: journalctl --user -b -u goodix-keyring-unlock
#
# NB: no probes on the collection (e.g. busctl get-property ... Locked).
# gnome-keyring registers clients asynchronously and a property read that
# arrives too early makes it abort: the daemon restarts LOCKED. Waiting for the
# Secret Service is done by gkr_unlock.py, with method calls only.
CRED="$HOME/.local/share/goodixfp/keyring-tpm.cred"
UNLOCKER="$HOME/.local/share/goodixfp/gkr_unlock.py"
log() { echo "[goodix-keyring] $*" >&2; }
[ -r "$CRED" ] || { log "no seal in $CRED, skipping"; exit 0; }

PW="$(systemd-creds decrypt --name=gkr "$CRED" - 2>/dev/null)" ||
  { log "the TPM does not release the password (PCR 7 changed? tss group?)"; exit 0; }
[ -n "$PW" ] || exit 0
if printf '%s' "$PW" | python3 "$UNLOCKER"; then
  log "keyring unlocked"
else
  log "unlock failed, the keyring will ask for the password"
fi
unset PW
exit 0
