#!/usr/bin/env bash
# Cattura un lotto di frame ETICHETTATI per il tuning offline del matcher.
# Uso:  ./capture_dataset.sh <label> <n>
#   label = template | genuine | impostor
#   template/genuine -> dito REGISTRATO ; impostor -> dito/i DIVERSO/I
#
# Richiede GOODIX_DUMP_IMGPROC attivo su fprintd (lo imposta setup_dump.sh).
# Ogni fprintd-verify cattura e salva un frame (l'esito match/no-match non conta).
# Usa sudo per spostare i dump (sono in /var/lib/fprint/goodix, root-owned).
set -u
DUMP=/var/lib/fprint/goodix
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LABEL="${1:?uso: capture_dataset.sh <template|genuine|impostor> <n>}"
N="${2:?manca il numero di tocchi}"
OUT="$REPO/captures/fp-dataset/$LABEL"   # captures/ e' fuori da git
mkdir -p "$OUT"

echo ">>> Pulisco i dump precedenti dal driver"
sudo rm -f "$DUMP"/imgproc-*.pgm

echo ">>> Fase '$LABEL': $N tocchi."
case "$LABEL" in
  impostor) echo "    USA UN DITO DIVERSO (meglio variarne 2-3) da quello registrato." ;;
  *)        echo "    USA IL DITO REGISTRATO (right-index). Copri un po' tutta la zona." ;;
esac
for i in $(seq "$N"); do
  echo "---- $LABEL $i/$N : appoggia il dito ----"
  fprintd-verify >/dev/null 2>&1 || true
done

echo ">>> Sposto i frame in $OUT"
sudo bash -c "mv $DUMP/imgproc-*-enh8.pgm $DUMP/imgproc-*-final8.pgm '$OUT'/ 2>/dev/null; chown -R $(id -un):$(id -gn) '$OUT'"
n_enh=$(ls "$OUT"/*-enh8.pgm 2>/dev/null | wc -l)
n_fin=$(ls "$OUT"/*-final8.pgm 2>/dev/null | wc -l)
echo "Salvati in $OUT: $n_enh frame enh8, $n_fin final8."
