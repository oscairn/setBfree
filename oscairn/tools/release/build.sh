#!/bin/sh
# Build one platform's single-file .clap from a clean Release configure.
#
#   build.sh <platform> <outdir>      e.g. build.sh macos-arm64 dist
#
# Writes <outdir>/<NAME>-<platform>.clap; the CMake tree goes under <outdir>/build-<platform>.
# Must run on a machine of the platform's OS (each release job runs on its own runner).
set -eu
cd "$(dirname "$0")/../../.."
. oscairn/tools/release/release.conf

platform=${1:?usage: build.sh <platform> <outdir>}
out=${2:?usage: build.sh <platform> <outdir>}

case $platform in
  macos-*) want=Darwin ;;
  linux-*) want=Linux ;;
  *) echo "build.sh: unknown platform '$platform' (macos-* or linux-*)" >&2; exit 2 ;;
esac
[ "$(uname -s)" = "$want" ] || {
    echo "build.sh: platform '$platform' needs $(uname -s != $want); build each platform on its own runner" >&2
    exit 1; }

mkdir -p "$out"
build=$out/build-$platform
rm -rf "$build"
cmake -S "$CMAKE_SOURCE_DIR" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel
cp "$build/$CLAP_FILE" "$out/$NAME-$platform.clap"
echo "build.sh: built $out/$NAME-$platform.clap"
