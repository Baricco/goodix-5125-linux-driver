#!/usr/bin/env bash
# install.sh — system installation of the goodixfpd daemon (only once).
# Creates the user/group, the udev rule, the PAM client, the systemd service, and deploys to /opt.
# It does NOT touch PAM: that is done by hand, with the safe test of docs/pam-integration.md.
#
# Usage:  sudo lab/daemon/install.sh
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"          # the repository's lab/daemon/ directory
GROUP=goodixfp; USERN=goodixfp

[ "$(id -u)" = 0 ] || { echo "must run as root: sudo lab/daemon/install.sh" >&2; exit 1; }

echo "[install] 1/5 system user and group '$USERN'"
getent group  "$GROUP" >/dev/null || groupadd -r "$GROUP"
NOLOGIN="$(command -v nologin || echo /usr/bin/nologin)"
getent passwd "$USERN" >/dev/null || \
  useradd -r -g "$GROUP" -s "$NOLOGIN" -d /var/lib/goodixfp "$USERN"

echo "[install] 2/5 payload in /opt (deploy)"
"$SRC/deploy.sh"

echo "[install] 3/5 udev rule (sensor access for the group; replaces the old uaccess one)"
install -m644 "$SRC/70-goodixfp.rules" /etc/udev/rules.d/70-goodixfp.rules
rm -f /etc/udev/rules.d/70-goodix-5125.rules
udevadm control --reload
udevadm trigger

echo "[install] 4/5 PAM client in /usr/local/lib/goodixfp"
install -Dm755 "$SRC/goodixfp-verify" /usr/local/lib/goodixfp/goodixfp-verify

echo "[install] 5/5 systemd service"
install -m644 "$SRC/goodixfpd.service" /etc/systemd/system/goodixfpd.service
systemctl daemon-reload
systemctl enable --now goodixfpd
sleep 1
systemctl --no-pager --full status goodixfpd | head -n 8 || true

cat <<'NEXT'

============================================================
Installed. NOW the test, in this order (no PAM until the socket works):

 1) Enroll a fingerprint with the GUI (as the user) and copy it into the system store:
      tools/.venv/bin/python lab/enroll_gui.py        # map densely + Save
      sudo install -Dm640 -o root -g goodixfp \
           state/fingers/<name>.npz /var/lib/goodixfp/$USER/<name>.npz

 2) Socket test (the socket belongs to goodixfp; with sudo root talks to it). Touch the sensor FIRMLY:
      sudo /usr/local/lib/goodixfp/goodixfp-verify $USER; echo "result=$?"
    result=0 = recognized. Try another finger too -> result=1.
    (To test it WITHOUT sudo: add yourself to the group with 'sudo usermod -aG goodixfp $USER' and log in again.)

 3) Only when the socket reliably gives result=0, hook PAM to sudo:
    keep a root shell open (sudo -i) in another terminal, then put AT THE TOP
    of /etc/pam.d/sudo:
      auth  sufficient  pam_exec.so  quiet  /usr/local/lib/goodixfp/goodixfp-verify
    Details and greeter login: docs/pam-integration.md

To uninstall everything:  sudo lab/daemon/uninstall.sh
============================================================
NEXT
