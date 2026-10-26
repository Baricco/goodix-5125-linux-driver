#!/usr/bin/env bash
# Captures a batch of LABELLED frames for offline matcher tuning.
# Usage:  ./capture_dataset.sh <label> <n>
#   label = template | genuine | impostor
#   template/genuine -> ENROLLED finger ; impostor -> DIFFERENT finger(s)
#
# Needs GOODIX_DUMP_IMGPROC enabled in fprintd (set by setup_dump.sh).
# Every fprintd-verify captures and saves one frame (the match/no-match outcome does not matter).
# Uses sudo to move the dumps (they are in /var/lib/fprint/goodix, owned by root).
set -u
DUMP=/var/lib/fprint/goodix
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LABEL="${1:?usage: capture_dataset.sh <template|genuine|impostor> <n>}"
N="${2:?the number of touches is missing}"
OUT="$REPO/captures/fp-dataset/$LABEL"   # captures/ is outside git
mkdir -p "$OUT"

echo ">>> Cleaning the driver's previous dumps"
sudo rm -f "$DUMP"/imgproc-*.pgm

echo ">>> Phase '$LABEL': $N touches."
case "$LABEL" in
  impostor) echo "    USE A FINGER DIFFERENT from the enrolled one (better to vary 2-3)." ;;
  *)        echo "    USE THE ENROLLED FINGER (right index). Cover the whole area a bit." ;;
esac
for i in $(seq "$N"); do
  echo "---- $LABEL $i/$N : place the finger ----"
  fprintd-verify >/dev/null 2>&1 || true
done

echo ">>> Moving the frames to $OUT"
sudo bash -c "mv $DUMP/imgproc-*-enh8.pgm $DUMP/imgproc-*-final8.pgm '$OUT'/ 2>/dev/null; chown -R $(id -un):$(id -gn) '$OUT'"
n_enh=$(ls "$OUT"/*-enh8.pgm 2>/dev/null | wc -l)
n_fin=$(ls "$OUT"/*-final8.pgm 2>/dev/null | wc -l)
echo "Saved in $OUT: $n_enh enh8 frames, $n_fin final8."
