#!/usr/bin/env bash
# deploy.sh — sincronizza SOLO il runtime del progetto in /opt/goodix-driver, ricrea il venv lì e
# copia la PSK appaiata al sensore (senza rigenerarla). Ripetibile: rilancialo dopo ogni modifica
# al codice per aggiornare la copia di produzione. Lo sviluppo resta in ~/goodix-driver (git).
#
# Uso:  sudo service/deploy.sh
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"     # radice del repo (service/ sta dentro)
DEST=/opt/goodix-driver
GROUP=goodixfp

[ "$(id -u)" = 0 ] || { echo "deve girare come root: sudo service/deploy.sh" >&2; exit 1; }
getent group "$GROUP" >/dev/null || groupadd -r "$GROUP"

echo "[deploy] copio il runtime in $DEST (solo i file che servono, niente .git/venv di sviluppo)"
install -d -m755 "$DEST" "$DEST/tools" "$DEST/goodix-fp-dump" "$DEST/service" "$DEST/captures"
install -m644 "$SRC"/driver_5125.py              "$DEST"/
install -m644 "$SRC"/tools/*.py                  "$DEST"/tools/
install -m644 "$SRC"/goodix-fp-dump/*.py         "$DEST"/goodix-fp-dump/
install -m644 "$SRC"/service/goodixfpd.py        "$DEST"/service/
install -m755 "$SRC"/service/goodixfp-verify     "$DEST"/service/
# config90.bin: driver_5125 lo legge all'import (blocco di configurazione MCU). Necessario.
install -m644 "$SRC"/captures/config90.bin       "$DEST"/captures/
chown -R root:root "$DEST"

echo "[deploy] (ri)creo il venv di produzione in $DEST/tools/.venv (solo dipendenze del demone)"
rm -rf "$DEST/tools/.venv"
python3 -m venv "$DEST/tools/.venv"
"$DEST/tools/.venv/bin/pip" -q install --upgrade pip
# Tutte le dipendenze del trasporto goodix-fp-dump (pyusb, periphery, spidev, crcmod, crccheck,
# pycryptodome: goodix.py/protocol.py le importano in cima) + numpy/opencv per il matching.
# Il demone NON usa PySide6 (solo la GUI di enroll): quello resta fuori.
"$DEST/tools/.venv/bin/pip" -q install -r "$SRC/goodix-fp-dump/requirements.txt" numpy opencv-python-headless

echo "[deploy] copio la PSK appaiata al sensore (NON la rigenero: è la chiave del tuo sensore)"
install -d -o root -g "$GROUP" -m750 "$DEST/state"
if [ -f "$SRC/state/linux_psk.bin" ]; then
  install -o root -g "$GROUP" -m640 "$SRC/state/linux_psk.bin" "$DEST/state/linux_psk.bin"
  echo "       PSK copiata ($(stat -c%s "$DEST/state/linux_psk.bin") byte)."
else
  echo "  !! ATTENZIONE: $SRC/state/linux_psk.bin assente. Senza PSK il demone non si autentica" >&2
  echo "     e load_or_create_psk NE RIGENEREREBBE una sbagliata. Ripristina la PSK prima di usare." >&2
fi

echo "[deploy] fatto. (Il servizio, se attivo, va riavviato: sudo systemctl restart goodixfpd)"
