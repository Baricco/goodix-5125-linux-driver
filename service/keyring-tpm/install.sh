#!/usr/bin/env bash
# Installs automatic unlocking of the GNOME keyring via TPM (PCR 7) for the
# current user. Meant for fingerprint login: after login the keyring unlocks by
# itself, without asking for the password. OPTIONAL and independent of the
# fingerprint driver.
#
# Prerequisites: Secure Boot ENABLED (otherwise PCR 7 protects nothing), TPM2,
# gnome-keyring, systemd-creds, python3 with dbus-python.
# Run as a NORMAL user (not root): it uses sudo where needed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USER_NAME="$(id -un)"
SHARE="$HOME/.local/share/goodixfp"
UNIT_DIR="$HOME/.config/systemd/user"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[!]\033[0m %s\n' "$*"; }
die() { printf '\033[31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

say "Preliminary checks"
[ -e /dev/tpmrm0 ] || die "TPM2 not found (/dev/tpmrm0). A TPM 2.0 is required."
command -v systemd-creds >/dev/null || die "systemd-creds missing."
command -v gnome-keyring-daemon >/dev/null || die "gnome-keyring missing."
python3 -c 'import dbus' 2>/dev/null || die "python3 dbus (python-dbus) missing."

SB="?"
sbvar="$(ls /sys/firmware/efi/efivars/SecureBoot-* 2>/dev/null | head -1)"
if [ -n "$sbvar" ]; then
  last="$(od -An -t u1 "$sbvar" 2>/dev/null | awk '{print $NF}')"
  [ "$last" = "1" ] && SB=on || SB=off
fi
if [ "$SB" != "on" ]; then
  warn "Secure Boot does NOT appear to be enabled (state: $SB)."
  warn "Without Secure Boot the PCR 7 seal protects little (a live USB would"
  warn "reproduce it). Auto-unlock will work anyway, but for real security"
  warn "enable Secure Boot. Continuing in 3s (Ctrl-C to cancel)..."
  sleep 3
fi

say "Adding $USER_NAME to the tss group (TPM access without root)"
sudo gpasswd -a "$USER_NAME" tss

say "Installing the user scripts in $SHARE"
mkdir -p "$SHARE" "$UNIT_DIR"
install -Dm755 "$HERE/gkr_unlock.py"    "$SHARE/gkr_unlock.py"
install -Dm755 "$HERE/keyring-unlock.sh" "$SHARE/keyring-unlock.sh"
install -Dm644 "$HERE/goodix-keyring-unlock.service" \
               "$UNIT_DIR/goodix-keyring-unlock.service"

say "Installing the polkit rule (credential decryption without a prompt for $USER_NAME)"
tmp="$(mktemp)"
sed "s/@USER@/$USER_NAME/g" "$HERE/49-goodix-keyring-creds.rules.in" > "$tmp"
sudo install -Dm644 "$tmp" /etc/polkit-1/rules.d/49-goodix-keyring-creds.rules
rm -f "$tmp"

say "Sealing the keyring password in the TPM (PCR 7)"
echo "    (type the keyring password = your login password)"
sudo bash "$HERE/seal.sh"

say "Enabling the user service"
systemctl --user daemon-reload
# reenable: recreates the [Install] links even when updating an older version.
systemctl --user reenable goodix-keyring-unlock.service

printf '\n\033[32mDone.\033[0m '
cat <<EOF
Log out completely and log in again (also with the fingerprint):
the tss group becomes active and at login the keyring unlocks by itself.
Check:  journalctl --user -b -u goodix-keyring-unlock   # expected: "keyring unlocked"
Uninstall with ./uninstall.sh
EOF
