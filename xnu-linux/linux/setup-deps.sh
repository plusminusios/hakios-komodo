#!/bin/bash
# Everything the kernel build needs that neither the repository nor Apple's xnu carries.
#
#   linux/setup-deps.sh --stage1   vendor sources, host tools (mig, unifdef, tcsh), the SDK
#   linux/setup-deps.sh --stage2   the firehose library, compiled against the kernel's exported
#                                  headers (needs the xnu tree from apply-xnu.sh first)
#
# Adapted for this repository from ls9875/hackios-umi's xnu-deps/setup.sh (APSL 2.0), which
# proves on ubuntu-latest that XNU can be built on Linux with theos' cctools and a stock clang.
# Changes here: only the firehose library is built (this kernel compiles the pthread provider
# in-tree), and the board define is the one this kernel is configured for.
set -e -o pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh"
mkdir -p "$DEPS"
cd "$DEPS"
JOBS=${JOBS:-$(nproc)}
MACOS_SDK_URL=https://github.com/joseluisq/macosx-sdks/releases/download/13.3/MacOSX13.3.sdk.tar.xz
PATCHES=${PATCHES_DIR:?}
XNU_SRC=${XNU_SRC:?}

log() { echo "setup-deps: $*"; }
die() { echo "setup-deps: ERROR: $*" >&2; exit 1; }

# fetch DIR URL COMMIT: one commit of a repository, without its history
fetch() {
	[ -d "$1" ] && git -C "$1" rev-parse -q --verify HEAD >/dev/null 2>&1 &&
		[ "$(git -C "$1" rev-parse --show-toplevel)" = "$(cd "$1" && pwd -P)" ] && return 0
	log "fetching $1"
	rm -rf "$1"
	git init -q "$1"
	git -C "$1" remote add origin "$2"
	git -C "$1" fetch -q --depth 1 origin "$3" || die "cannot fetch $3 from $2"
	git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

link() { [ "$(readlink "$2" 2>/dev/null)" = "$1" ] || ln -sfn "$1" "$2"; }

update() {
	cat > "$1.new"
	if cmp -s "$1.new" "$1"; then rm "$1.new"; else mv "$1.new" "$1"; fi
}

stage1() {
	local missing= tool
	for tool in git curl tar xz make cc clang clang++ flex bison m4 perl python3 autoreconf automake; do
		command -v $tool >/dev/null || missing="$missing $tool"
	done
	[ -z "$missing" ] || die "not installed:$missing"
	[ -x "$CCTOOLS/ld" ] || die "no cctools in $CCTOOLS: install theos with its Linux toolchain"
	[ -d "$MACOS_SDK/usr/include" ] || bash "$TOP/linux/setup-sdk.sh" || die "no macOS SDK in $MACOS_SDK"

	# --- sources ---------------------------------------------------------------------------
	fetch AvailabilityVersions https://github.com/apple-oss-distributions/AvailabilityVersions.git \
		5d9baee345b3bc72b26c71adc65f94f2d5e9ced2        # AvailabilityVersions-115.4
	fetch libdispatch https://github.com/apple-oss-distributions/libdispatch.git \
		a641558f447659d90ad002ef0ee6f0212273fbfc        # libdispatch-1415.100.11
	fetch mm_bootstrap_cmds https://github.com/markmentovai/bootstrap_cmds \
		f337e600741638896fb6056ce87c6479f74b89a3        # cross_platform/bootstrap_cmds-138
	fetch tcsh https://github.com/tcsh-org/tcsh.git b7db07931546178c4cb2542b5fc850c483628ed5
	fetch unifdef https://github.com/fanf2/unifdef.git 08d12cd3059fe072929ea001ad9833f5487fd355

	# The macOS SDK brings the DriverKit headers Apple's closed iig would generate, and the
	# Kernel.framework private headers (AppleKeyStore, cp_*) that HFS and the kext code use.
	[ -d "$MACOS_SDK/System/Library/Frameworks/Kernel.framework" ] ||
		bash "$TOP/linux/setup-sdk.sh" || die "no macOS SDK in $MACOS_SDK"

	# --- host tools -------------------------------------------------------------------------
	mkdir -p host/bin
	# XNU's host tools include <mach-o/...>; the five headers they need (three from the xnu
	# tree, arch.h and swap.h written here) are in linux/hostinc, which HOST_CC points at.
	for f in loader.h nlist.h fat.h arch.h swap.h; do
		[ -f "$TOP/linux/hostinc/mach-o/$f" ] || die "missing linux/hostinc/mach-o/$f"
	done
	# mig: Apple's own does not build on glibc; this is a port that does
	if [ ! -x host/bin/migcom ] || [ ! -x host/bin/mig ]; then
		log "building mig"
		(cd mm_bootstrap_cmds && autoreconf --install && sh configure --prefix="$DEPS/host" &&
			make -j"$JOBS" && make install) > mig-build.log 2>&1 ||
			die "mig did not build, see deps/mig-build.log"
	fi
	if [ ! -x host/bin/unifdef ]; then
		log "building unifdef"
		[ -f unifdef/version.h ] || printf '"@(#) $Version: unifdef-2.12 $\\n"\n"@(#) $Date: 2020-11-06 11:22:51 +0000 $\\n"\n' > unifdef/version.h
		cc -std=gnu11 -O2 -w -o host/bin/unifdef unifdef/unifdef.c
	fi
	# csh: XNU's SETUP/config/doconf is a csh script
	if [ ! -x host/bin/tcsh ]; then
		log "building tcsh"
		(cd tcsh && ./configure --prefix="$DEPS/host" && make -j"$JOBS") > tcsh-build.log 2>&1 ||
			die "tcsh did not build, see deps/tcsh-build.log"
		cp tcsh/tcsh host/bin/tcsh
	fi
	link tcsh host/bin/csh
	# the Mach-O linker and its companions come with theos
	for tool in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
		link "$CCTOOLS/$tool" host/bin/$tool
	done
	link "$CCTOOLS/ld" host/bin/ld64
	link "$CCTOOLS/ld" "$TOP/linux/bin/ld64"
	link xnu-clang "$TOP/linux/bin/xnu-clang++"

	# --- the SDK ----------------------------------------------------------------------------
	# XNU's makefiles want a directory that looks like an SDK: the macOS SDK, as symlinks, with
	# the pieces Apple's internal SDK has and the public one lacks put under usr/local.
	mkdir -p "$DEPS/sdk"
	if [ ! -d "$XNU_SDK_PATH/usr/include" ]; then
		log "making the SDK: links to $MACOS_SDK"
		rm -rf "$XNU_SDK_PATH"
		cp -rs "$(cd "$MACOS_SDK" && pwd)" "$XNU_SDK_PATH"
	fi
	local sdk_local=$XNU_SDK_PATH/usr/local
	mkdir -p "$sdk_local/include/kernel/os" "$sdk_local/include/os" "$sdk_local/lib/kernel" "$sdk_local/libexec"
	update "$sdk_local/libexec/availability.pl" < AvailabilityVersions/availability.pl
	[ -x "$sdk_local/libexec/availability.pl" ] || chmod +x "$sdk_local/libexec/availability.pl"
	# the kernel's half of a private libdispatch header (unifdef exits 1 when it removed something)
	{ host/bin/unifdef -DOS_FIREHOSE_SPI=1 -DKERNEL=1 libdispatch/os/firehose_buffer_private.h || [ $? -eq 1 ]; } |
		update "$sdk_local/include/kernel/os/firehose_buffer_private.h"
	update "$sdk_local/include/kernel/os/firehose_types.defs" < libdispatch/src/firehose/firehose_types.defs
	log "stage 1 done"
}

# The firehose (os_log) buffer code of libdispatch is open source but not part of XNU: Apple
# links it from its internal SDK. Here it becomes a static library in the SDK, compiled against
# the headers the kernel build exports -- so the export step has to come first.
stage2() {
	[ -f "$XNU_SRC/makedefs/MakeInc.def" ] || die "no kernel tree in $XNU_SRC (run linux/apply-xnu.sh)"
	[ -x host/bin/migcom ] && [ -d "$XNU_SDK_PATH/usr/local/lib/kernel" ] || die "stage 1 has not been done"
	local lib=$XNU_SDK_PATH/usr/local/lib/kernel hdrs=$XNU_SRC/BUILD/obj/EXPORT_HDRS
	if [ -f "$lib/libfirehose_kernel.a" ]; then
		log "stage 2: nothing to do"
		return 0
	fi
	log "exporting the kernel's headers"
	"$TOP/linux/build.sh" -j"$JOBS" exporthdrs > exporthdrs.log 2>&1 && [ -d "$hdrs/osfmk" ] ||
		die "the kernel headers were not exported, see deps/exporthdrs.log"
	log "building libfirehose_kernel.a"
	mkdir -p build/firehose
	xnu-clang -arch arm64 -c -Os -mkernel -nostdinc -fno-builtin \
		-DKERNEL=1 -DKERNEL_PRIVATE=1 -DXNU_KERNEL_PRIVATE=0 -DPRIVATE=1 -D__DARWIN_64_BIT_INO_T=1 \
		-DDISPATCH_USE_DTRACE=0 -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 \
		-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0 -DOS_FIREHOSE_SPI=1 -DARM64_BOARD_CONFIG_${MACHINE_CONFIG} \
		-Ilibdispatch -Ilibdispatch/src -Ilibdispatch/src/firehose \
		-I$hdrs/osfmk -I$hdrs/bsd -I$hdrs/libkern -I$hdrs/pexpert -I$hdrs/iokit -I$hdrs/security \
		-I$XNU_SRC/libkern/firehose -I$hdrs/libkern/os -I$XNU_SRC/EXTERNAL_HEADERS \
		-I$XNU_SDK_PATH/usr/local/include/kernel \
		-o build/firehose/firehose_buffer.o libdispatch/src/firehose/firehose_buffer.c
	libtool -static -o "$lib/libfirehose_kernel.a" build/firehose/firehose_buffer.o
	ls -la "$lib/libfirehose_kernel.a"
	log "stage 2 done"
}

case "${1:-}" in
--stage1) stage1 ;;
--stage2) stage2 ;;
"") stage1; [ -f "$XNU_SRC/makedefs/MakeInc.def" ] && stage2 ;;
*) sed -n '2,12p' "$0"; exit 1 ;;
esac
