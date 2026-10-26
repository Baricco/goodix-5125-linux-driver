#!/usr/bin/env bash
# uninstall.sh — removes the system installation of the daemon. Reversible and safe.
# Leaves /var/lib/goodixfp (your fingerprints) and the goodixfp user, unless --purge.
#
# Usage:  sudo lab/daemon/uninstall.sh [--purge]
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "must run as root: sudo lab/daemon/uninstall.sh" >&2; exit 1; }
PURGE="${1:-}"

echo "[uninstall] stopping and removing the systemd service"
systemctl disable --now goodixfpd 2>/dev/null || true
rm -f /etc/systemd/system/goodixfpd.service
systemctl daemon-reload

echo "[uninstall] removing the udev rule and the PAM client"
rm -f /etc/udev/rules.d/70-goodixfp.rules
udevadm control --reload 2>/dev/null || true
udevadm trigger 2>/dev/null || true
rm -rf /usr/local/lib/goodixfp

echo "[uninstall] removing /opt/goodix-driver"
rm -rf /opt/goodix-driver

echo
echo "  !! REMEMBER: remove the 'pam_exec.so .../goodixfp-verify' line from /etc/pam.d/sudo by hand"
echo "     (and from any other PAM file where you put it)."

if [ "$PURGE" = "--purge" ]; then
  echo "[uninstall] --purge: also removing fingerprints and the system user"
  userdel goodixfp 2>/dev/null || true
  rm -rf /var/lib/goodixfp
else
  echo "  (Left in place: /var/lib/goodixfp with the fingerprints, and the goodixfp user."
  echo "   To remove them: sudo lab/daemon/uninstall.sh --purge)"
fi
