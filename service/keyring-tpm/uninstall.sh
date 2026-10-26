#!/usr/bin/env bash
# Removes the keyring auto-unlock via TPM. Does NOT remove the user from the tss
# group (it may be needed for something else) nor delete the keyring password.
set -euo pipefail
SHARE="$HOME/.local/share/goodixfp"
UNIT_DIR="$HOME/.config/systemd/user"

systemctl --user disable goodix-keyring-unlock.service 2>/dev/null || true
rm -f "$UNIT_DIR/goodix-keyring-unlock.service"
systemctl --user daemon-reload 2>/dev/null || true
rm -f "$SHARE/keyring-unlock.sh" "$SHARE/gkr_unlock.py" "$SHARE/keyring-tpm.cred"
sudo rm -f /etc/polkit-1/rules.d/49-goodix-keyring-creds.rules
echo "Removed. (The tss group and the keyring password are unchanged.)"
