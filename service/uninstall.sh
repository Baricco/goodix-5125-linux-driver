#!/usr/bin/env bash
# uninstall.sh — rimuove l'installazione di sistema del demone. Reversibile e sicuro.
# Lascia /var/lib/goodixfp (le tue impronte) e l'utente goodixfp, salvo --purge.
#
# Uso:  sudo service/uninstall.sh [--purge]
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "deve girare come root: sudo service/uninstall.sh" >&2; exit 1; }
PURGE="${1:-}"

echo "[uninstall] fermo e rimuovo il servizio systemd"
systemctl disable --now goodixfpd 2>/dev/null || true
rm -f /etc/systemd/system/goodixfpd.service
systemctl daemon-reload

echo "[uninstall] rimuovo regola udev e client PAM"
rm -f /etc/udev/rules.d/70-goodixfp.rules
udevadm control --reload 2>/dev/null || true
udevadm trigger 2>/dev/null || true
rm -rf /usr/local/lib/goodixfp

echo "[uninstall] rimuovo /opt/goodix-driver"
rm -rf /opt/goodix-driver

echo
echo "  !! RICORDA: togli a mano la riga 'pam_exec.so .../goodixfp-verify' da /etc/pam.d/sudo"
echo "     (e da eventuali altri file PAM in cui l'hai messa)."

if [ "$PURGE" = "--purge" ]; then
  echo "[uninstall] --purge: rimuovo anche impronte e utente di sistema"
  userdel goodixfp 2>/dev/null || true
  rm -rf /var/lib/goodixfp
else
  echo "  (Lasciati: /var/lib/goodixfp con le impronte, e l'utente goodixfp."
  echo "   Per toglierli: sudo service/uninstall.sh --purge)"
fi
