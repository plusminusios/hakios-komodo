#!/bin/bash
# run-build.sh — build the komodo XNU kernel on Linux (no macOS) and package it
# for the phone. One command, idempotent, logs in logs/.
#
#   ./run-build.sh            everything
#   ./run-build.sh --from N   resume at step N (1..7)
#
# Steps
#   1 toolchain   LLVM 18 + theos cctools (ld64) + macOS 13.3 SDK, unpacked from dl/
#   2 sources     xnu-8796.141.3 + tegu stage scripts + 81 tegu + 4 komodo patches
#   3 deps-1      mig, unifdef, tcsh, SDK overlay
#   4 headers     make exporthdrs
#   5 deps-2      libfirehose_kernel.a
#   6 kernel      make (+ DriverKit iig stubs from the link errors)
#   7 package     KOMOXNU payload + boot images (shim + embedded payload)
#
# Based on myh120405/ios-sweet-linux (APSL 2.0, see NOTICE.sweet-linux), adapted
# for MACHINE_CONFIG=TEGU (komodo reuses that slot) and a root-less toolchain.
set -uo pipefail
TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# komodo/ (shim, ADT, payload tools, komodo patches). Local: ../komodo;
# inside the komodo tree (komodo/linux-build): ..
if [ -z "${KOMODO:-}" ]; then
	if [ -d "$TOP/../komodo/shim" ]; then KOMODO=$(cd "$TOP/../komodo" && pwd); else KOMODO=$(cd "$TOP/.." && pwd); fi
fi
# ios-tegu xnu/patches (81 patches + 3 stage scripts)
TEGU_PATCHES=${TEGU_PATCHES:-$TOP/../ios-tegu-ref/xnu/patches}
# ios-tegu tools/ (tegu-init.S + mkinit.py for the md0 init image)
TEGU_TOOLS=${TEGU_TOOLS:-$(cd "$TEGU_PATCHES/../.." 2>/dev/null && pwd)/tools}
LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/clang+llvm-18.1.8-x86_64-linux-gnu-ubuntu-18.04.tar.xz
TC_URL=https://github.com/L1ghtmann/llvm-project/releases/download/test-210562a/iOSToolchain-x86_64.tar.xz
SDK_URL=https://github.com/joseluisq/macosx-sdks/releases/download/13.3/MacOSX13.3.sdk.tar.xz
LOGS=$TOP/logs; mkdir -p "$LOGS"
FROM=1; [ "${1:-}" = "--from" ] && FROM=${2:?step number}

say()  { printf '\n\033[1;36m== [%s] %s\033[0m\n' "$1" "$2"; }
die()  { printf '\n\033[1;31mFAILED at step %s: %s\033[0m\nlog: %s\n' "$1" "$2" "${3:-}"; [ -n "${3:-}" ] && tail -40 "$3"; exit 1; }
step() { [ "$1" -ge "$FROM" ]; }

# ---------------------------------------------------------------------------
if step 1; then
	say 1 "toolchain"
	cd "$TOP"; mkdir -p dl
	[ -s dl/llvm18.tar.xz ] || curl -fL --retry 5 -s -o dl/llvm18.tar.xz "$LLVM_URL" || die 1 "download LLVM"
	[ -s dl/iOSToolchain-x86_64.tar.xz ] || curl -fL --retry 5 -s -o dl/iOSToolchain-x86_64.tar.xz "$TC_URL" || die 1 "download cctools"
	[ -s dl/MacOSX13.3.sdk.tar.xz ] || curl -fL --retry 5 -s -o dl/MacOSX13.3.sdk.tar.xz "$SDK_URL" || die 1 "download SDK"
	# patch series: tegu's 81 + stage scripts, then komodo's 008x on top
	if [ ! -d patches-src/xnu/patches ] || [ -z "$(ls patches-src/xnu/patches/*.patch 2>/dev/null)" ]; then
		[ -d "$TEGU_PATCHES" ] || die 1 "no tegu patches at $TEGU_PATCHES (set TEGU_PATCHES)"
		mkdir -p patches-src/xnu/patches
		cp "$TEGU_PATCHES"/*.patch "$TEGU_PATCHES"/*.sh patches-src/xnu/patches/
	fi
	cp "$KOMODO"/ci/008*.patch patches-src/xnu/patches/   # always the current komodo set
	[ -x deps/llvm/bin/clang ] || { mkdir -p deps/llvm && tar -xJf dl/llvm18.tar.xz -C deps/llvm --strip-components=1; } || die 1 "unpack LLVM"
	# the LLVM 18 release links libtinfo.so.5 (ncurses 5 ABI); Arch only has 6
	[ -f deps/llvm/lib/libtinfo.so.5 ] && LC_ALL=C readelf -V deps/llvm/lib/libtinfo.so.5 | grep -q NCURSES_TINFO_5 ||
		gcc -shared -fPIC -O2 -Wl,--version-script=linux/tinfo5/tinfo5.map -Wl,-soname,libtinfo.so.5 \
			-o deps/llvm/lib/libtinfo.so.5 linux/tinfo5/tinfo5_stub.c || die 1 "libtinfo.so.5 stub"
	[ -x theos/toolchain/linux/iphone/bin/ld ] || { mkdir -p theos/toolchain && tar -xJf dl/iOSToolchain-x86_64.tar.xz -C theos/toolchain/; } || die 1 "unpack cctools"
	[ -d deps/MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework ] || tar -xJf dl/MacOSX13.3.sdk.tar.xz -C deps || die 1 "unpack SDK"
	deps/llvm/bin/clang --version | head -1 || die 1 "clang does not run (libtinfo? see linux/tinfo5)"
	{ theos/toolchain/linux/iphone/bin/ld -v 2>&1 || true; } | grep -m1 "ld64" || die 1 "ld64 does not run"
fi

source "$TOP/linux/env.sh"     # PATH: linux/bin (xnu-clang, xcrun...), deps/host/bin, LLVM

# ---------------------------------------------------------------------------
if step 2; then
	say 2 "xnu sources + patch series ($(ls "$PATCHES_DIR"/*.patch | wc -l) patches)"
	bash "$TOP/linux/apply-xnu.sh" > "$LOGS/2-apply.log" 2>&1 || die 2 "apply-xnu" "$LOGS/2-apply.log"
	grep -E "^apply-xnu: (applied=|overlay)" "$LOGS/2-apply.log"
fi
if step 3; then
	say 3 "host tools: mig, unifdef, tcsh, SDK overlay"
	bash "$TOP/linux/setup-deps.sh" --stage1 > "$LOGS/3-deps1.log" 2>&1 || die 3 "setup-deps stage1" "$LOGS/3-deps1.log"
fi
if step 4; then
	say 4 "export kernel headers"
	bash "$TOP/linux/build.sh" exporthdrs > "$LOGS/4-exporthdrs.log" 2>&1 || die 4 "exporthdrs" "$LOGS/4-exporthdrs.log"
fi
if step 5; then
	say 5 "libfirehose_kernel"
	bash "$TOP/linux/setup-deps.sh" --stage2 > "$LOGS/5-deps2.log" 2>&1 || die 5 "setup-deps stage2" "$LOGS/5-deps2.log"
fi
K=$XNU_SRC/BUILD/obj/${KERNEL_CONFIG}_ARM64_${MACHINE_CONFIG}/kernel.$(echo "$KERNEL_CONFIG" | tr A-Z a-z).$(echo "$MACHINE_CONFIG" | tr A-Z a-z)
if step 6; then
	say 6 "kernel (DEVELOPMENT / ARM64 / $MACHINE_CONFIG) — the long one"
	rm -f "$K" "$K.unstripped"   # never package a stale kernel after a failed compile
	(cd "$TOP" && bash linux/build-with-iig.sh) > "$LOGS/6-kernel.log" 2>&1
	[ -s "$K" ] || die 6 "no kernel produced (first errors below)" "$LOGS/6-kernel.log"
	ls -la "$K"; file "$K" 2>/dev/null
fi
# ---------------------------------------------------------------------------
if step 7; then
	say 7 "package for komodo"
	[ -s "$K" ] || die 7 "no kernel at $K (run step 6)"
	mkdir -p "$TOP/out"; cp "$K" "$TOP/out/"
	cd "$KOMODO/shim" || die 7 "no komodo/shim"
	python3 -I ../mkadt_komodo.py ../komodo.adt > "$LOGS/7-package.log" 2>&1 || die 7 "ADT" "$LOGS/7-package.log"
	rm -f payload.bin
	make -s xnuboot KERNEL="$K" >> "$LOGS/7-package.log" 2>&1 || die 7 "make xnuboot" "$LOGS/7-package.log"
	cp xnu-boot-raw.img xnu-boot-stocklike.img "$TOP/out/"
	# second set: a static init as md0 (mockfs root) -> "init running as PID 1"
	# without touching the phone's flash
	if [ -f "$TEGU_TOOLS/tegu-init.S" ] && [ -f "$TEGU_TOOLS/mkinit.py" ]; then
		clang -target arm64-apple-ios16.0 -c "$TEGU_TOOLS/tegu-init.S" -o "$TOP/out/tegu-init.o" >> "$LOGS/7-package.log" 2>&1 &&
		python3 "$TEGU_TOOLS/mkinit.py" "$TOP/out/tegu-init.o" "$TOP/out/tegu-init" >> "$LOGS/7-package.log" 2>&1 &&
		rm -f payload.bin &&
		make -s xnuboot KERNEL="$K" RAMDISK="$TOP/out/tegu-init" PREFIX=xnu-init \
			CMDLINE="debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 rd=md0" \
			>> "$LOGS/7-package.log" 2>&1 &&
		cp xnu-init-raw.img xnu-init-stocklike.img "$TOP/out/" ||
		echo "note: init ramdisk variant failed (see $LOGS/7-package.log); the plain images are fine"
	else
		echo "note: no tegu tools at $TEGU_TOOLS; skipping the init ramdisk variant"
	fi
	rm -f payload.bin
	grep -E "chained fixups|ramdisk:|payload.bin|xnu-(boot|init)" "$LOGS/7-package.log"
	say 7 "DONE"
	cat <<EOF

Kernel:      $TOP/out/$(basename "$K")
Boot images: $TOP/out/xnu-boot-raw.img        kernel only (ends in "no root" panic)
             $TOP/out/xnu-boot-stocklike.img
             $TOP/out/xnu-init-raw.img        + init on md0 (goal: PID 1 on screen)
             $TOP/out/xnu-init-stocklike.img
Diagnostics: $KOMODO/diag/out/v0..v4_*.img

Phone in fastboot (RAM-only, nothing is flashed):
  fastboot boot <image>
See $KOMODO/УТРО.md for the order and what each screen means.
EOF
fi
