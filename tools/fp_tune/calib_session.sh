#!/usr/bin/env bash
# Sessione di calibrazione del matcher (una tantum, ~10 minuti di tocchi).
#
# Presuppone libfprint-goodixgf installata (packaging/). Durante la sessione
# attiva in fprintd i dump grezzi 16-bit e i log di debug; a fine sessione li
# spegne da sola e cancella i dump dal sistema (restano solo in ~/fp-dataset2).
#
#  1. ri-registra il dito (se la pipeline immagine cambia, il vecchio template
#     non e' piu' confrontabile)
#  2. N verify col dito registrato (genuini) e N con altre dita (impostori):
#     misura FRR/FAR dal vivo e salva TUTTI i frame grezzi per il tuning offline
#     (tools/fp_tune/analyze_session.py)
#
# Uso:  tools/fp_tune/calib_session.sh [n_genuini] [n_impostori]
# Output: ~/fp-dataset2/<fase>/ (dump imgproc-*), ~/fp-dataset2/session.log
set -u
DUMP=/var/lib/fprint/goodix
DEBUG_CONF=/etc/systemd/system/fprintd.service.d/debug.conf
OUT="$HOME/fp-dataset2"
FINGER=right-index-finger
NG="${1:-25}"
NI="${2:-25}"
LOG="$OUT/session.log"

mkdir -p "$OUT"
: > "$LOG"
say() { echo -e "\n\033[1;36m>>> $*\033[0m"; echo ">>> $*" >> "$LOG"; }

say "Autenticazione sudo (una volta sola; resta attiva per tutta la sessione)"
sudo -v || exit 1
( while true; do sudo -n -v 2>/dev/null; sleep 50; done ) &
KEEP=$!

cleanup() {
  kill $KEEP 2>/dev/null
  echo ">>> Spengo dump e debug di fprintd, cancello i dump di sistema"
  sudo rm -f "$DEBUG_CONF"
  sudo bash -c "rm -f $DUMP/imgproc-*"
  sudo systemctl daemon-reload
  sudo systemctl restart fprintd
}
trap cleanup EXIT

say "Attivo dump grezzi e log di debug in fprintd (solo per questa sessione)"
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

grab() {   # sposta i dump del driver nella cartella della fase
  local dst="$OUT/$1"
  mkdir -p "$dst"
  sudo bash -c "mv $DUMP/imgproc-* '$dst'/ 2>/dev/null; cp $DUMP/goodix.dat '$dst'/ 2>/dev/null; chown -R $(id -un):$(id -gn) '$dst'"
  echo "  salvati $(ls "$dst" | grep -c raw16) frame in $dst"
}

verify_loop() {   # $1=fase  $2=n  $3=istruzioni
  local phase=$1 n=$2 ok=0 i res
  sudo bash -c "rm -f $DUMP/imgproc-*"
  for i in $(seq "$n"); do
    echo -e "\n---- $phase $i/$n : $3"
    echo "     (aspetta 'Place your finger', appoggia, poi SOLLEVA)"
    res=$(fprintd-verify -f "$FINGER" 2>&1 | tee /dev/stderr | grep -o 'verify-[a-z-]*' | tail -1)
    echo "$phase $i $res" >> "$LOG"
    [ "$res" = "verify-match" ] && ok=$((ok + 1))
  done
  echo "$phase: $ok/$n verify-match" | tee -a "$LOG"
  grab "$phase"
}

say "FASE 1/3 - registrazione nuova del dito ($FINGER)"
echo "    Tocchi NATURALI come quando sblocchi, spostando un poco il dito ogni volta"
echo "    (piu' in alto, in basso, a sinistra, a destra, leggermente ruotato)."
sudo bash -c "rm -f $DUMP/imgproc-*"
fprintd-delete "$USER" >/dev/null 2>&1
fprintd-enroll -f "$FINGER" 2>&1 | tee -a "$LOG"
grab enroll

say "FASE 2/3 - $NG verify con lo STESSO dito (genuini), tocco naturale da sblocco"
verify_loop genuine "$NG" "dito REGISTRATO ($FINGER)"

say "FASE 3/3 - $NI verify con ALTRE dita (impostori)"
echo "    Alterna: medio, anulare, pollice, indice dell'altra mano (cambia ogni ~6 tocchi)."
verify_loop impostor "$NI" "dito DIVERSO"

journalctl -u fprintd --since "$T0" --no-pager -o short-precise > "$OUT/fprintd.journal" 2>/dev/null
say "Fatto. Riepilogo:"
grep -E '^(genuine|impostor):' "$LOG"
echo "Dati in $OUT (journal in $OUT/fprintd.journal)."
