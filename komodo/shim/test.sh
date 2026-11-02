#!/bin/bash
# Offline checks for the komodo shim: host renders + logic tests + the real
# freestanding aarch64 Image link (no ABS64 relocs). No phone needed.
set -e
cd "$(dirname "$0")"
TMP="$(mktemp -d)"
CF="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"

echo "== 1. console host render =="
gcc -O1 -Wall -Wextra -o "$TMP/con" console.c host_test.c
"$TMP/con" "$TMP/console.png" | sed 's/^/   /'

echo "== 2. shim integration on real komodo-base.dtb =="
gcc -O1 -Wall -Wextra -DSHIM_HOST -DLOADER_HOST -o "$TMP/shim" \
    shim.c fb.c fdt.c console.c loader.c exceptions.c wdt.c host_shim_test.c
"$TMP/shim" komodo-base.dtb "$TMP/shim.png" | sed 's/^/   /'

echo "== 3. MMU descriptor/attribute logic =="
gcc -O1 -Wall -Wextra -DMMU_HOST -o "$TMP/mmu" mmu.c host_mmu_test.c
"$TMP/mmu" | sed 's/^/   /'

echo "== 4. loader: KOMOXNU payload -> segments + boot_args =="
gcc -O1 -Wall -Wextra -DLOADER_HOST -o "$TMP/loader" loader.c fdt.c console.c host_loader_test.c
"$TMP/loader" | sed 's/^/   /'

echo "== 5. freestanding aarch64 build + link -> Image =="
for f in start.S shim.c fb.c fdt.c console.c mmu.c loader.c vectors.S exceptions.c wdt.c; do
  aarch64-linux-gnu-gcc $CF -c "$f" -o "$TMP/$(basename ${f%.*}).o"
done
abs=$(aarch64-linux-gnu-readelf -r "$TMP"/shim.o "$TMP"/fb.o "$TMP"/fdt.o "$TMP"/console.o "$TMP"/mmu.o "$TMP"/loader.o "$TMP"/exceptions.o "$TMP"/wdt.o 2>/dev/null | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "   ERROR: $abs ABS64 relocations in C objects"; exit 1; }
echo "   ABS64 relocations: 0 (position-independent image OK)"
aarch64-linux-gnu-gcc $CF -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
  "$TMP"/start.o "$TMP"/shim.o "$TMP"/fb.o "$TMP"/fdt.o "$TMP"/console.o "$TMP"/mmu.o "$TMP"/loader.o "$TMP"/vectors.o "$TMP"/exceptions.o "$TMP"/wdt.o -o "$TMP/shim.elf"
aarch64-linux-gnu-objcopy -O binary "$TMP/shim.elf" "$TMP/Image"
magic=$(xxd -s 0x38 -l 4 -p "$TMP/Image")
[ "$magic" = "41524d64" ] || { echo "   ERROR: bad Image magic $magic"; exit 1; }
echo "   linked Image: $(stat -c%s "$TMP/Image") bytes, arm64 magic OK"

echo "ALL OFFLINE SHIM CHECKS PASSED ($TMP)"
