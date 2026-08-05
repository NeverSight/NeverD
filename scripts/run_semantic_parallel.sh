#!/usr/bin/env bash
# Parallel runner for NeverDSemanticTests using GTest sharding.
# Usage: run_semantic_parallel.sh [num_shards] [gtest_filter]
#
# By default the test binary is rebuilt before running.  This is deliberate: an
# incremental `make` does NOT pick up newly added test .cpp files until CMake is
# re-configured, so running the existing binary can silently execute a stale or
# incomplete test set and report misleading pass/fail counts.  Reconfiguring then
# building guarantees the binary matches the current sources.  Set ND_NO_BUILD=1
# to skip the build and run whatever binary already exists.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
BIN="$BUILD_DIR/bin/NeverDSemanticTests"
SHARDS="${1:-8}"
FILTER="${2:-*}"

if [ "${ND_NO_BUILD:-0}" != "1" ]; then
  if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "no configured build at $BUILD_DIR; configure it once, or set ND_NO_BUILD=1" >&2
    exit 2
  fi
  NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  echo "building NeverDSemanticTests (set ND_NO_BUILD=1 to skip)..."
  # Re-configure so CMake adds any newly listed test sources, then build.
  if ! cmake -S "$ROOT" -B "$BUILD_DIR" >/tmp/nd_build_reconf.log 2>&1; then
    echo "cmake reconfigure failed; see /tmp/nd_build_reconf.log" >&2
    exit 2
  fi
  if ! make -C "$BUILD_DIR" NeverDSemanticTests -j"$NCPU" >/tmp/nd_build.log 2>&1; then
    echo "build failed; see /tmp/nd_build.log" >&2
    exit 2
  fi
fi

if [ ! -x "$BIN" ]; then
  echo "test binary not found: $BIN (build it, or unset ND_NO_BUILD)" >&2
  exit 2
fi

OUTDIR="$(mktemp -d /tmp/nd_shard.XXXXXX)"
echo "shards=$SHARDS filter=$FILTER outdir=$OUTDIR"

pids=()
for i in $(seq 0 $((SHARDS-1))); do
  GTEST_TOTAL_SHARDS=$SHARDS GTEST_SHARD_INDEX=$i \
    "$BIN" --gtest_filter="$FILTER" --gtest_brief=1 \
    >"$OUTDIR/shard_$i.log" 2>&1 &
  pids+=($!)
done

fail=0
declare -a RC
for i in $(seq 0 $((SHARDS-1))); do
  if wait "${pids[$i]}"; then RC[$i]=0; else RC[$i]=$?; fail=1; fi
done

echo "==== SUMMARY ===="
grep -hE '\[==========\].*ran' "$OUTDIR"/shard_*.log | awk '{s+=$2} END{print "total_run="s}'
grep -hE '\[  PASSED  \]' "$OUTDIR"/shard_*.log | awk '{s+=$4} END{print "total_passed="s}'

# Flag shards that did NOT finish (killed by a signal, OOM, or otherwise cut
# short) so a partial run can never masquerade as a clean one: a killed shard
# silently drops its whole slice from the totals above, making far fewer tests
# look like a near-complete pass.  A finished shard always prints gtest's final
# "[==========] N tests ... ran" line; its absence means the shard was cut off.
incomplete=""
for i in $(seq 0 $((SHARDS-1))); do
  rc=${RC[$i]:-0}
  if ! grep -qE '\[==========\].*ran' "$OUTDIR/shard_$i.log"; then
    if [ "$rc" -gt 128 ]; then
      incomplete="$incomplete shard_$i(killed:sig=$((rc-128)))"
    else
      incomplete="$incomplete shard_$i(incomplete:rc=$rc)"
    fi
  fi
done
if [ -n "$incomplete" ]; then
  echo "!!! INCOMPLETE/KILLED shards — totals UNDER-REPORTED, run is NOT clean:$incomplete"
  fail=1
fi

echo "--- FAILED lines (if any) ---"
grep -hE '\[  FAILED  \]' "$OUTDIR"/shard_*.log | grep -v 'listed below' | sort -u
grep -hE 'FAILED|error:|Segmentation' "$OUTDIR"/shard_*.log | grep -vE '\[  FAILED  \] [0-9]+ test' | head -50
echo "logs in $OUTDIR"
exit $fail
