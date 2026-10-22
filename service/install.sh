#!/usr/bin/env bash
# install.sh — installazione di sistema del demone goodixfpd (una volta sola).
# Crea l'utente/gruppo, la regola udev, il client PAM, il servizio systemd, e fa il deploy in /opt.
# NON tocca PAM: quello si fa a mano, col collaudo sicuro di docs/pam-integration.md.
#
# Uso:  sudo service/install.sh
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"          # directory service/ del repo
GROUP=goodixfp; USERN=goodixfp

[ "$(id -u)" = 0 ] || { echo "deve girare come root: sudo service/install.sh" >&2; exit 1; }

echo "[install] 1/5 utente e gruppo di sistema '$USERN'"
getent group  "$GROUP" >/dev/null || groupadd -r "$GROUP"
NOLOGIN="$(command -v nologin || echo /usr/bin/nologin)"
getent passwd "$USERN" >/dev/null || \
  useradd -r -g "$GROUP" -s "$NOLOGIN" -d /var/lib/goodixfp "$USERN"

echo "[install] 2/5 payload in /opt (deploy)"
"$SRC/deploy.sh"

echo "[install] 3/5 regola udev (accesso al sensore per il gruppo; sostituisce la vecchia uaccess)"
install -m644 "$SRC/70-goodixfp.rules" /etc/udev/rules.d/70-goodixfp.rules
rm -f /etc/udev/rules.d/70-goodix-5125.rules
udevadm control --reload
udevadm trigger

echo "[install] 4/5 client PAM in /usr/local/lib/goodixfp"
install -Dm755 "$SRC/goodixfp-verify" /usr/local/lib/goodixfp/goodixfp-verify

echo "[install] 5/5 servizio systemd"
install -m644 "$SRC/goodixfpd.service" /etc/systemd/system/goodixfpd.service
systemctl daemon-reload
systemctl enable --now goodixfpd
sleep 1
systemctl --no-pager --full status goodixfpd | head -n 8 || true

cat <<'NEXT'

============================================================
Installato. ORA il collaudo, nell'ordine (niente PAM finché il socket non va):

 1) Registra un'impronta con la GUI (da utente) e copiala nello store di sistema:
      tools/.venv/bin/python tools/enroll_gui.py        # mappa denso + Salva
      sudo install -Dm640 -o root -g goodixfp \
           state/fingers/<nome>.npz /var/lib/goodixfp/$USER/<nome>.npz

 2) Test del socket (il socket è di goodixfp; con sudo ci parla root). Tocca il sensore DECISO:
      sudo /usr/local/lib/goodixfp/goodixfp-verify $USER; echo "esito=$?"
    esito=0 = riconosciuto. Prova anche un altro dito -> esito=1.
    (Per testarlo SENZA sudo: aggiungiti al gruppo con 'sudo usermod -aG goodixfp $USER' e rilogga.)

 3) Solo quando il socket dà esito=0 in modo affidabile, collega PAM a sudo:
    tieni aperta una shell root (sudo -i) in un altro terminale, poi metti IN CIMA
    a /etc/pam.d/sudo:
      auth  sufficient  pam_exec.so  quiet  /usr/local/lib/goodixfp/goodixfp-verify
    Dettagli e login al greeter: docs/pam-integration.md

Per disinstallare tutto:  sudo service/uninstall.sh
============================================================
NEXT
