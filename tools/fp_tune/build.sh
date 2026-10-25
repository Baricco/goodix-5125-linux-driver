#!/usr/bin/env bash
# Compila l'harness di tuning contro lo stesso libsigfm.a del driver libfprint.
set -e
cd "$(dirname "$0")"
# Clone di Rockytkg con il fork libfprint (SIGFM su OpenCV), in ext/ fuori da git.
FORK="$(cd "$(dirname "$0")/../.." && pwd)/ext/goodix-linux-27c6-5125/libfprint"
SIGFM_DIR="$FORK/libfprint/sigfm"
LIBSIGFM="$FORK/build/libfprint/sigfm/libsigfm.a"
[ -f "$LIBSIGFM" ] || { echo "libsigfm.a non trovato: builda prima libfprint (ninja -C $FORK/build)"; exit 1; }
PC=opencv4; pkg-config --exists opencv4 2>/dev/null || PC=opencv5
CFLAGS=$(pkg-config --cflags "$PC")
LDIRS=$(pkg-config --libs-only-L "$PC")
# Solo i moduli usati da SIGFM (evita opencv_viz -> VTK).
# OpenCV 5: features2d -> features. SIFT sta in opencv_features.
LIBS="-lopencv_features -lopencv_imgproc -lopencv_imgcodecs -lopencv_core -lopencv_flann"
pkg-config --exists opencv4 2>/dev/null && LIBS="-lopencv_features2d -lopencv_imgproc -lopencv_imgcodecs -lopencv_core -lopencv_flann"
g++ -O2 -std=c++17 sigfm_test.cpp -o sigfm_test -I"$SIGFM_DIR" $CFLAGS "$LIBSIGFM" $LDIRS $LIBS
echo "OK -> $(pwd)/sigfm_test"
# Pipeline immagine reale del driver su frame 16-bit (tuning preprocessing).
CORE="$FORK/libfprint/drivers/goodixgf/core"
gcc -O2 -std=gnu11 -Wall imgproc_run.c "$CORE/goodix_imgproc.c" -I"$CORE" -lm -o imgproc_run
echo "OK -> $(pwd)/imgproc_run"
# SIGFM di produzione come .so per l'analisi Python (ratio regolabile via
# sigfm_set_ratio, tutto il resto identico al sorgente del fork).
sed -e 's/^constexpr auto distance_match = 0.75;/static double distance_match = 0.75;\nextern "C" void sigfm_set_ratio(double r) { distance_match = r; }/' \
    "$SIGFM_DIR/sigfm.cpp" > .sigfm_prod.cpp
g++ -O2 -fPIC -shared -std=c++17 .sigfm_prod.cpp -o libsigfm_prod.so -I"$SIGFM_DIR" $CFLAGS $LDIRS $LIBS
rm -f .sigfm_prod.cpp
echo "OK -> $(pwd)/libsigfm_prod.so"
