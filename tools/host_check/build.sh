#!/bin/sh
# Builds and runs the host-compiled equivalence harness (see README.md).
#
#   ./build.sh            build + run, print to stdout
#   ./build.sh --golden   build + run, capture stdout as golden.txt
#   ./build.sh --diff     build + run, diff against golden.txt
set -e
cd "$(dirname "$0")"
ROOT=../..
OUT=out
mkdir -p "$OUT"

CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter -I stub -I $ROOT/src/ui -I $ROOT/include"

$CXX $CXXFLAGS -c "$ROOT/src/ui/theme.cpp"          -o "$OUT/theme.o"
$CXX $CXXFLAGS -c "$ROOT/src/ui/gfx.cpp"            -o "$OUT/gfx.o"
$CXX $CXXFLAGS -c "$ROOT/src/ui/icons.cpp"          -o "$OUT/icons.o"
$CXX $CXXFLAGS -c "$ROOT/src/ui/widgets.cpp"        -o "$OUT/widgets.o"
$CXX $CXXFLAGS -c "$ROOT/src/ui/screen_scenes.cpp"   -o "$OUT/screen_scenes.o"
$CXX $CXXFLAGS -c "$ROOT/src/ui/screen_settings.cpp" -o "$OUT/screen_settings.o"
# driver.cpp #includes screen.cpp and screen_devices.cpp directly (see its
# own comment for why) — do NOT also compile those two here, or their
# externally-linked symbols (drawDeviceCard, RowSnap snap[], ...) get defined
# twice and the link fails.
$CXX $CXXFLAGS -c driver.cpp                        -o "$OUT/driver.o"
$CXX "$OUT"/*.o -o "$OUT/host_check"

case "$1" in
  --golden)
    "$OUT/host_check" > golden.txt
    echo "wrote golden.txt ($(wc -l < golden.txt) lines)"
    ;;
  --diff)
    "$OUT/host_check" > "$OUT/current.txt"
    if diff -u golden.txt "$OUT/current.txt"; then
      echo "OK: matches golden.txt"
    else
      echo "MISMATCH against golden.txt" >&2
      exit 1
    fi
    ;;
  *)
    "$OUT/host_check"
    ;;
esac
