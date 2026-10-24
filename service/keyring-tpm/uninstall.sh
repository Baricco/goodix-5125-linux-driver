#!/usr/bin/env bash
# Rimuove l'auto-sblocco keyring via TPM. NON rimuove l'utente dal gruppo tss
# (potrebbe servire ad altro) ne' cancella la password del keyring.
set -euo pipefail
SHARE="$HOME/.local/share/goodixfp"
UNIT_DIR="$HOME/.config/systemd/user"

systemctl --user disable goodix-keyring-unlock.service 2>/dev/null || true
rm -f "$UNIT_DIR/goodix-keyring-unlock.service"
systemctl --user daemon-reload 2>/dev/null || true
rm -f "$SHARE/keyring-unlock.sh" "$SHARE/gkr_unlock.py" "$SHARE/keyring-tpm.cred"
sudo rm -f /etc/polkit-1/rules.d/49-goodix-keyring-creds.rules
echo "Rimosso. (Il gruppo tss e la password del keyring restano invariati.)"
