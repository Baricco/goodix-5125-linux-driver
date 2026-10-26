#!/usr/bin/env bash
# Seals the GNOME keyring password in the TPM (TPM only, bound to PCR 7).
# Can be run through sudo: the resulting file is given to the real user.
# The password is read at run time and never printed.
set -euo pipefail

TARGET_USER="${SUDO_USER:-$USER}"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
CRED="$TARGET_HOME/.local/share/goodixfp/keyring-tpm.cred"
mkdir -p "$(dirname "$CRED")"

read -rsp "Keyring password (= login password): " PW; echo

# -T = TPM only (no host key -> a user in the tss group can decrypt it),
# bound to the state of PCR 7 (Secure Boot). Needs Secure Boot ENABLED to be
# worth anything (otherwise a live USB with SB off reproduces the same PCR 7).
printf '%s' "$PW" | systemd-creds encrypt -T --tpm2-pcrs=7 --name=gkr - "$CRED"
chmod 600 "$CRED"
chown "$TARGET_USER:$TARGET_USER" "$CRED" 2>/dev/null || true
chown "$TARGET_USER:$TARGET_USER" "$(dirname "$CRED")" 2>/dev/null || true

# Round-trip check (does not print the password).
DEC="$(systemd-creds decrypt --name=gkr "$CRED" - 2>/dev/null || true)"
if [ -n "$DEC" ] && [ "$DEC" = "$PW" ]; then
  echo "OK: seal created and verified ($CRED)"
else
  echo "ERROR: round trip failed (check the TPM/PCR 7)"; exit 1
fi
unset PW DEC
