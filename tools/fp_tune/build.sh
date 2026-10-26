#!/usr/bin/env bash
# Builds the tuning harness against the same libsigfm.a as the libfprint driver.
set -e
cd "$(dirname "$0")"
# Rockytkg clone with the libfprint fork (SIGFM on OpenCV), in ext/ outside git.
FORK="$(cd "$(dirname "$0")/../.." && pwd)/ext/goodix-linux-27c6-5125/libfprint"
SIGFM_DIR="$FORK/libfprint/sigfm"
LIBSIGFM="$FORK/build/libfprint/sigfm/libsigfm.a"
[ -f "$LIBSIGFM" ] || { echo "libsigfm.a not found: build libfprint first (ninja -C $FORK/build)"; exit 1; }
PC=opencv4; pkg-config --exists opencv4 2>/dev/null || PC=opencv5
CFLAGS=$(pkg-config --cflags "$PC")
LDIRS=$(pkg-config --libs-only-L "$PC")
# Only the modules SIGFM uses (avoids opencv_viz -> VTK).
# OpenCV 5: features2d -> features. SIFT lives in opencv_features.
LIBS="-lopencv_features -lopencv_imgproc -lopencv_imgcodecs -lopencv_core -lopencv_flann"
pkg-config --exists opencv4 2>/dev/null && LIBS="-lopencv_features2d -lopencv_imgproc -lopencv_imgcodecs -lopencv_core -lopencv_flann"
g++ -O2 -std=c++17 sigfm_test.cpp -o sigfm_test -I"$SIGFM_DIR" $CFLAGS "$LIBSIGFM" $LDIRS $LIBS
echo "OK -> $(pwd)/sigfm_test"
# The driver's real image pipeline on 16-bit frames (preprocessing tuning).
CORE="$FORK/libfprint/drivers/goodixgf/core"
gcc -O2 -std=gnu11 -Wall imgproc_run.c "$CORE/goodix_imgproc.c" -I"$CORE" -lm -o imgproc_run
echo "OK -> $(pwd)/imgproc_run"
# Production SIGFM as a .so for the Python analysis (ratio adjustable through
# sigfm_set_ratio, everything else identical to the fork's source).
sed -e 's/^constexpr auto distance_match = 0.75;/static double distance_match = 0.75;\nextern "C" void sigfm_set_ratio(double r) { distance_match = r; }/' \
    "$SIGFM_DIR/sigfm.cpp" > .sigfm_prod.cpp
g++ -O2 -fPIC -shared -std=c++17 .sigfm_prod.cpp -o libsigfm_prod.so -I"$SIGFM_DIR" $CFLAGS $LDIRS $LIBS
rm -f .sigfm_prod.cpp
echo "OK -> $(pwd)/libsigfm_prod.so"
