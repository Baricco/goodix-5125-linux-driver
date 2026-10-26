#!/usr/bin/env bash
# deploy.sh — syncs ONLY the project runtime into /opt/goodix-driver, recreates the venv there and
# copies the PSK paired with the sensor (without regenerating it). Repeatable: run it again after
# every code change to update the production copy. Development stays in ~/goodix-driver (git).
#
# Usage:  sudo lab/daemon/deploy.sh
set -euo pipefail

SRC="$(cd "$(dirname "$0")/../.." && pwd)"  # repository root (lab/daemon/ lives inside it)
DEST=/opt/goodix-driver
GROUP=goodixfp

[ "$(id -u)" = 0 ] || { echo "must run as root: sudo lab/daemon/deploy.sh" >&2; exit 1; }
getent group "$GROUP" >/dev/null || groupadd -r "$GROUP"

echo "[deploy] copying the runtime to $DEST (only the files needed, no .git/development venv)"
install -d -m755 "$DEST" "$DEST/lab" "$DEST/goodix-fp-dump" "$DEST/lab/daemon" "$DEST/captures"
install -m644 "$SRC"/lab/*.py                    "$DEST"/lab/
install -m644 "$SRC"/goodix-fp-dump/*.py         "$DEST"/goodix-fp-dump/
install -m644 "$SRC"/lab/daemon/goodixfpd.py     "$DEST"/lab/daemon/
install -m755 "$SRC"/lab/daemon/goodixfp-verify  "$DEST"/lab/daemon/
# config90.bin: driver_5125 reads it at import time (MCU configuration block). Required.
install -m644 "$SRC"/captures/config90.bin       "$DEST"/captures/
chown -R root:root "$DEST"

# The venv is recreated only if missing (or with --venv): code-only redeploys stay fast.
if [ ! -x "$DEST/tools/.venv/bin/python" ] || [ "${1:-}" = "--venv" ]; then
  echo "[deploy] (re)creating the production venv in $DEST/tools/.venv (daemon dependencies only)"
  rm -rf "$DEST/tools/.venv"
  python3 -m venv "$DEST/tools/.venv"
  "$DEST/tools/.venv/bin/pip" -q install --upgrade pip
  # All the dependencies of the goodix-fp-dump transport (pyusb, periphery, spidev, crcmod, crccheck,
  # pycryptodome: goodix.py/protocol.py import them at the top) + numpy/opencv for matching.
  # The daemon does NOT use PySide6 (only the enrollment GUI does): it stays out.
  "$DEST/tools/.venv/bin/pip" -q install -r "$SRC/goodix-fp-dump/requirements.txt" numpy opencv-python-headless
else
  echo "[deploy] venv already there, reusing it (use '--venv' to recreate it from scratch)"
fi

echo "[deploy] copying the PSK paired with the sensor (NOT regenerated: it is your sensor's key)"
install -d -o root -g "$GROUP" -m750 "$DEST/state"
if [ -f "$SRC/state/linux_psk.bin" ]; then
  install -o root -g "$GROUP" -m640 "$SRC/state/linux_psk.bin" "$DEST/state/linux_psk.bin"
  echo "       PSK copied ($(stat -c%s "$DEST/state/linux_psk.bin") bytes)."
else
  echo "  !! WARNING: $SRC/state/linux_psk.bin missing. Without the PSK the daemon cannot authenticate" >&2
  echo "     and load_or_create_psk WOULD GENERATE a wrong one. Restore the PSK before use." >&2
fi

echo "[deploy] done. (If the service is running, restart it: sudo systemctl restart goodixfpd)"
