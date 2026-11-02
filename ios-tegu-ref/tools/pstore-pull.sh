#!/usr/bin/env bash
# Pull pstore/ramoops logs after a failed test boot.
#
# Tensor Pixels keep the last kernel console output in pstore. After a crash
# or a forced reboot following `fastboot boot`, boot back into Android and run
# this script (adb, no root needed for /sys/fs/pstore):
#
#   tools/pstore-pull.sh [outdir]
set -euo pipefail
OUT="${1:-pstore-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

adb wait-for-device
FILES=$(adb shell 'ls /sys/fs/pstore 2>/dev/null' | tr -d '\r')
if [[ -z "$FILES" ]]; then
  echo "no pstore files found (either no crash since last boot, or ramoops unsupported)"
  exit 0
fi
for f in $FILES; do
  adb pull "/sys/fs/pstore/$f" "$OUT/" >/dev/null
  echo "== $OUT/$f"
done
echo "Zip the directory and share it; also useful: 'adb shell cat /proc/last_kmsg' (older path)."
