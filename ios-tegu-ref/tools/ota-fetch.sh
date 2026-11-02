#!/usr/bin/env bash
# Fetch the Pixel 9a (tegu) full OTA and extract the stock boot images +
# device trees for hardware research.
#
# Needs: curl, python3, payload-dumper-go (https://github.com/ssut/payload-dumper-go)
#
# Usage:
#   tools/ota-fetch.sh [URL] [--workdir DIR]
#
# Without a URL, uses the known tegu build CP3A.260905.009 (Android 17,
# September 2026). NOTE: Google's OTA page (developers.google.com/android/ota)
# is JS-rendered; if you need a different build, grab the zip URL from that
# page in a browser (it looks like
# https://dl.google.com/dl/android/aosp/tegu-ota-<build>-<sha8>.zip) and pass
# it as the first argument.
set -euo pipefail

URL=""
WORKDIR="ota"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --workdir) WORKDIR="$2"; shift 2 ;;
    *) URL="$1"; shift ;;
  esac
done
URL="${URL:-https://dl.google.com/dl/android/aosp/tegu-ota-cp3a.260905.009-6750e3df.zip}"

mkdir -p "$WORKDIR"
cd "$WORKDIR"

ZIP="$(basename "$URL")"
if [[ ! -f "$ZIP" ]]; then
  echo "== downloading $URL (~3.4 GB)"
  curl -L -o "$ZIP" "$URL"
fi

command -v payload-dumper-go >/dev/null || {
  echo "payload-dumper-go not found. Install: https://github.com/ssut/payload-dumper-go/releases" >&2
  exit 1
}
echo "== extracting boot images"
payload-dumper-go -partitions boot,vendor_boot,dtbo,init_boot,vendor_kernel_boot -o . "$ZIP"

echo "$URL" > payload-meta.txt
echo "== done:"
ls -la *.img
echo
echo "Next steps:"
echo "  # dump the 10 dtbo overlays (mostly PMIC rails):"
echo "  python3 tools/unpack_bootv4.py $WORKDIR/boot.img $WORKDIR/vendor_boot.img $WORKDIR/dtbo.img dtb/"
echo "  # the 4 full base DTBs are FDT blobs inside vendor_kernel_boot.img; dump + read them:"
echo "  python3 tools/fdt-read.py <extracted dtb> --find gic"
