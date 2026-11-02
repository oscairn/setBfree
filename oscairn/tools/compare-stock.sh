#!/bin/sh
# The stock-vs-fork gate (doc/oscairn-plan.md, M0): renders the same performance with
# upstream's engine sources and with this tree's, and requires identical bytes.
#   oscairn/tools/compare-stock.sh WORKDIR [UPSTREAM_REF]
# UPSTREAM_REF defaults to the merge base with upstream's master (fetched if missing).
# The fork is built with SETBFREE_LIBC_RAND, as upstream draws its key-click and wave-noise
# randomness from libc's rand(); the fork's default generator is compared for information.
# The fork's key attack and release take 2.9 ms at every rate, one fragment at 44.1 kHz as
# upstream's do: at 44.1 kHz it is compared as it is, at other rates with osc.envelope.rate=0
# (one fragment at every rate, as upstream), and as it is for information.
set -eu
work=$1
ref=${2:-}
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$work"
work=$(cd "$work" && pwd)

if [ -z "$ref" ]; then
    git -C "$root" remote get-url upstream >/dev/null 2>&1 ||
        git -C "$root" remote add upstream https://github.com/pantherb/setBfree.git
    git -C "$root" fetch -q upstream master
    ref=$(git -C "$root" merge-base HEAD upstream/master)
fi
rm -rf "$work/stock-src"
mkdir -p "$work/stock-src"
git -C "$root" archive "$ref" src b_whirl b_overdrive b_reverb | tar -x -C "$work/stock-src"

build() { # name source-dir extra-cmake-args...
    name=$1 src=$2
    shift 2
    cmake -S "$root/oscairn" -B "$work/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DOSCAIRN_ORGAN_TESTS=OFF -DSETBFREE_SOURCE_DIR="$src" "$@" >/dev/null
    cmake --build "$work/$name" --target setbfree-gate-render >/dev/null
}
build stock "$work/stock-src" -DSETBFREE_STOCK=ON
build fork "$root" -DSETBFREE_LIBC_RAND=ON
build fork-default "$root"

status=0
for rate in 44100 48000 96000; do
    for config in noconfig config; do
        more=
        [ $rate = 44100 ] || more="osc.envelope.rate=0"
        "$work/stock/setbfree-gate-render" $rate "$work/stock-$rate-$config.raw" $config
        "$work/fork/setbfree-gate-render" $rate "$work/fork-$rate-$config.raw" $config "$more"
        "$work/fork-default/setbfree-gate-render" $rate "$work/fork-default-$rate-$config.raw" $config
        if cmp -s "$work/stock-$rate-$config.raw" "$work/fork-$rate-$config.raw"; then
            echo "identical: $rate Hz $config${more:+ ($more)}"
        else
            echo "DIFFERENT: $rate Hz $config${more:+ ($more)}"
            status=1
        fi
        if [ -n "$more" ]; then
            "$work/fork/setbfree-gate-render" $rate "$work/fork-rate-$rate-$config.raw" $config
            if cmp -s "$work/stock-$rate-$config.raw" "$work/fork-rate-$rate-$config.raw"; then
                echo "  (fork's 2.9 ms attack and release: identical too)"
            else
                echo "  (fork's 2.9 ms attack and release: differs, as expected at this rate)"
            fi
        fi
        if cmp -s "$work/stock-$rate-$config.raw" "$work/fork-default-$rate-$config.raw"; then
            echo "  (fork's own generator: identical too)"
        else
            echo "  (fork's own generator: differs, as expected from other key-click randomness)"
        fi
    done
done
echo "upstream $ref"
exit $status
