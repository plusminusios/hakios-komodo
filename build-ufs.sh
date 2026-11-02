#!/bin/bash
# build-ufs.sh MODE_HEX PREFIX ["extra boot-args"]
# Packages a RAM-only UFS test image with the given ufsmode, copies to out/.
set -euo pipefail
ROOT=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
MODE="${1:?mode hex e.g. 0x116}"
PREFIX="${2:?prefix e.g. xnu-ufsP-phy}"
EXTRA="${3:-}"
SHIM="$ROOT"/komodo/shim
OUT="$ROOT"/xnu-linux/out
KERNEL=$OUT/kernel.development.tegu
CMD="debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 panicreset=300 ufscache=1 ufsbanner=1 rd=ufsroot0 ufsroot=userdata ufsmode=$MODE $EXTRA"
cd "$SHIM"
rm -f payload.bin
make -s xnuboot KERNEL="$KERNEL" PREFIX="$PREFIX" CMDLINE="$CMD"
rm -f payload.bin
cp "$PREFIX"-raw.img "$PREFIX"-stocklike.img "$OUT"/
echo ">> built $PREFIX (ufsmode=$MODE) -> $OUT/$PREFIX-stocklike.img"
