#!/usr/bin/env bash
# Matcher calibration session (one-off, ~10 minutes of touches).
#
# Assumes goodixfpd is installed (packaging/goodixfpd). During the session it
# turns on raw 16-bit dumps and debug logs in fprintd; at the end of the
# session it turns them off by itself and deletes the dumps from the system
# (they stay only in captures/fp-dataset2, outside git).
#
#  1. enrolls the finger again (if the image pipeline changes, the old template
#     can no longer be compared)
#  2. N verifications with the enrolled finger (genuine) and N with other
#     fingers (impostors): measures FRR/FAR live and saves ALL the raw frames
#     for offline tuning (tools/fp_tune/analyze_session.py)
#
# Usage:  tools/fp_tune/calib_session.sh [n_genuine] [n_impostors]
# Output: captures/fp-dataset2/<phase>/ (imgproc-* dumps), captures/fp-dataset2/session.log
set -u
DUMP=/var/lib/fprint/goodix
DEBUG_CONF=/etc/systemd/system/fprintd.service.d/debug.conf
OUT="$(cd "$(dirname "$0")/../.." && pwd)/captures/fp-dataset2"
FINGER=right-index-finger
NG="${1:-25}"
NI="${2:-25}"
LOG="$OUT/session.log"

mkdir -p "$OUT"
: > "$LOG"
say() { echo -e "\n\033[1;36m>>> $*\033[0m"; echo ">>> $*" >> "$LOG"; }

say "sudo authentication (only once; it stays active for the whole session)"
sudo -v || exit 1
( while true; do sudo -n -v 2>/dev/null; sleep 50; done ) &
KEEP=$!

cleanup() {
  kill $KEEP 2>/dev/null
  echo ">>> Turning fprintd dumps and debug off, deleting the system dumps"
  sudo rm -f "$DEBUG_CONF"
  sudo bash -c "rm -f $DUMP/imgproc-*"
  sudo systemctl daemon-reload
  sudo systemctl restart fprintd
}
trap cleanup EXIT

say "Turning on raw dumps and debug log in fprintd (for this session only)"
sudo install -Dm644 /dev/stdin "$DEBUG_CONF" <<'EOF'
[Service]
Environment=GOODIX_DUMP_IMGPROC=1
Environment=G_MESSAGES_DEBUG=all
EOF
sudo systemctl daemon-reload
sudo systemctl restart fprintd
sleep 1
T0=$(date '+%Y-%m-%d %H:%M:%S')
echo "start $T0" >> "$LOG"

grab() {   # moves the driver's dumps into the phase folder
  local dst="$OUT/$1"
  mkdir -p "$dst"
  sudo bash -c "mv $DUMP/imgproc-* '$dst'/ 2>/dev/null; cp $DUMP/goodix.dat '$dst'/ 2>/dev/null; chown -R $(id -un):$(id -gn) '$dst'"
  echo "  saved $(ls "$dst" | grep -c raw16) frames in $dst"
}

verify_loop() {   # $1=phase  $2=n  $3=instructions
  local phase=$1 n=$2 ok=0 i res
  sudo bash -c "rm -f $DUMP/imgproc-*"
  for i in $(seq "$n"); do
    echo -e "\n---- $phase $i/$n : $3"
    echo "     (wait for 'Place your finger', place it, then LIFT it)"
    res=$(fprintd-verify -f "$FINGER" 2>&1 | tee /dev/stderr | grep -o 'verify-[a-z-]*' | tail -1)
    echo "$phase $i $res" >> "$LOG"
    [ "$res" = "verify-match" ] && ok=$((ok + 1))
  done
  echo "$phase: $ok/$n verify-match" | tee -a "$LOG"
  grab "$phase"
}

say "PHASE 1/3 - new enrollment of the finger ($FINGER)"
echo "    NATURAL touches as when unlocking, moving the finger a little every time"
echo "    (higher, lower, left, right, slightly rotated)."
sudo bash -c "rm -f $DUMP/imgproc-*"
fprintd-delete "$USER" >/dev/null 2>&1
fprintd-enroll -f "$FINGER" 2>&1 | tee -a "$LOG"
grab enroll

say "PHASE 2/3 - $NG verifications with the SAME finger (genuine), natural unlock touch"
verify_loop genuine "$NG" "ENROLLED finger ($FINGER)"

say "PHASE 3/3 - $NI verifications with OTHER fingers (impostors)"
echo "    Alternate: middle, ring, thumb, index of the other hand (change every ~6 touches)."
verify_loop impostor "$NI" "DIFFERENT finger"

journalctl -u fprintd --since "$T0" --no-pager -o short-precise > "$OUT/fprintd.journal" 2>/dev/null
say "Done. Summary:"
grep -E '^(genuine|impostor):' "$LOG"
echo "Data in $OUT (journal in $OUT/fprintd.journal)."
