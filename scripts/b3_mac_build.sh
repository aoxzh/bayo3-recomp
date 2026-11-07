#!/usr/bin/env bash
# Build the macOS Bayonetta 3 binary (HEAD) under the shared machine lock.
#
#   scripts/b3_mac_build.sh [--dir <build dir>]
#
# The default build directory is private/build-mac; --dir or BUILD may select another.
# Incremental rebuilds only.
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${BUILD:-$ROOT/private/build-mac}
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) BUILD=$2; shift 2 ;;
    *) args+=("$1"); shift ;;
  esac
done

LOCK=$ROOT/private/.b3-mac-lock
mkdir -p "$ROOT/private"
if ! mkdir "$LOCK" 2>/dev/null; then
  echo "waiting for the machine lock ($LOCK)..."
  for _ in $(seq 1 600); do
    sleep 2
    mkdir "$LOCK" 2>/dev/null && break
  done
  [ -d "$LOCK" ] || { echo "error: machine lock is stuck: $LOCK" >&2; exit 1; }
fi
cleanup() { rmdir "$LOCK" 2>/dev/null; }
trap cleanup EXIT

echo "building $BUILD"
cmake --build "$BUILD" --parallel "${JOBS:-8}" ${args[@]+"${args[@]}"}
rc=$?
echo "build exit $rc"
exit $rc
