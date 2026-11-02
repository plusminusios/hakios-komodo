#!/bin/bash
# Fetch the macOS SDK the kernel is compiled against (the same SDKROOT=macosx the macOS build
# uses). Only the SDK: the cctools linker comes from theos, and linux/setup-deps.sh builds the
# usr/local overlay inside it (firehose headers and library, availability.pl).
set -e -o pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh"
SDK_URL=${MACOS_SDK_URL:-https://github.com/joseluisq/macosx-sdks/releases/download/13.3/MacOSX13.3.sdk.tar.xz}

log() { echo "setup-sdk: $*"; }

if [ -d "$MACOS_SDK/usr/include" ] && [ -d "$MACOS_SDK/System/Library/Frameworks/Kernel.framework" ]; then
	log "already there: $MACOS_SDK"
	exit 0
fi
mkdir -p "$DEPS"
cd "$DEPS"
log "downloading $SDK_URL"
curl -L --fail --retry 5 -s -o macos-sdk.tar.xz "$SDK_URL" || { echo "setup-sdk: download failed" >&2; exit 1; }
rm -rf MacOSX13.3.sdk
tar -xJf macos-sdk.tar.xz || { echo "setup-sdk: cannot unpack $SDK_URL" >&2; exit 1; }
rm -f macos-sdk.tar.xz
[ -d "$MACOS_SDK/System/Library/Frameworks/Kernel.framework" ] ||
	{ echo "setup-sdk: $MACOS_SDK has no Kernel.framework" >&2; exit 1; }
log "SDK at $MACOS_SDK ($(du -sh "$MACOS_SDK" | cut -f1))"
