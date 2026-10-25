#!/usr/bin/env bash
# Installa lo sblocco automatico del keyring GNOME via TPM (PCR 7) per l'utente
# corrente. Pensato per login a impronta: dopo il login il keyring si sblocca da
# solo, senza chiedere la password. OPZIONALE e indipendente dal driver impronte.
#
# Prerequisiti: Secure Boot ATTIVO (altrimenti PCR7 non protegge), TPM2,
# gnome-keyring, systemd-creds, python3 con dbus-python.
# Eseguire come UTENTE normale (non root): usa sudo dove serve.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USER_NAME="$(id -un)"
SHARE="$HOME/.local/share/goodixfp"
UNIT_DIR="$HOME/.config/systemd/user"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[!]\033[0m %s\n' "$*"; }
die() { printf '\033[31m[errore]\033[0m %s\n' "$*" >&2; exit 1; }

say "Controlli preliminari"
[ -e /dev/tpmrm0 ] || die "TPM2 non trovato (/dev/tpmrm0). Serve un TPM 2.0."
command -v systemd-creds >/dev/null || die "systemd-creds mancante."
command -v gnome-keyring-daemon >/dev/null || die "gnome-keyring mancante."
python3 -c 'import dbus' 2>/dev/null || die "python3 dbus (python-dbus) mancante."

SB="?"
sbvar="$(ls /sys/firmware/efi/efivars/SecureBoot-* 2>/dev/null | head -1)"
if [ -n "$sbvar" ]; then
  last="$(od -An -t u1 "$sbvar" 2>/dev/null | awk '{print $NF}')"
  [ "$last" = "1" ] && SB=on || SB=off
fi
if [ "$SB" != "on" ]; then
  warn "Secure Boot NON risulta attivo (stato: $SB)."
  warn "Senza Secure Boot il sigillo su PCR 7 protegge poco (un live USB lo"
  warn "riprodurrebbe). L'auto-sblocco funzionera' comunque, ma per sicurezza"
  warn "reale abilita Secure Boot. Continuo tra 3s (Ctrl-C per annullare)..."
  sleep 3
fi

say "Aggiungo $USER_NAME al gruppo tss (accesso TPM senza root)"
sudo gpasswd -a "$USER_NAME" tss

say "Installo gli script utente in $SHARE"
mkdir -p "$SHARE" "$UNIT_DIR"
install -Dm755 "$HERE/gkr_unlock.py"    "$SHARE/gkr_unlock.py"
install -Dm755 "$HERE/keyring-unlock.sh" "$SHARE/keyring-unlock.sh"
install -Dm644 "$HERE/goodix-keyring-unlock.service" \
               "$UNIT_DIR/goodix-keyring-unlock.service"

say "Installo la regola polkit (decrypt credenziali senza prompt per $USER_NAME)"
tmp="$(mktemp)"
sed "s/@USER@/$USER_NAME/g" "$HERE/49-goodix-keyring-creds.rules.in" > "$tmp"
sudo install -Dm644 "$tmp" /etc/polkit-1/rules.d/49-goodix-keyring-creds.rules
rm -f "$tmp"

say "Sigillo la password del keyring nel TPM (PCR 7)"
echo "    (digita la password del keyring = la tua password di login)"
sudo bash "$HERE/seal.sh"

say "Abilito il servizio utente"
systemctl --user daemon-reload
# reenable: ricrea i link di [Install] anche aggiornando una versione precedente.
systemctl --user reenable goodix-keyring-unlock.service

cat <<EOF

\033[32mFatto.\033[0m Esegui un LOGOUT completo e rientra (anche con l'impronta):
il gruppo tss diventa attivo e al login il keyring si sblocca da solo.
Verifica:  journalctl --user -b -u goodix-keyring-unlock   # atteso: "keyring sbloccato"
Disinstalla con ./uninstall.sh
EOF
