#!/bin/bash
# Put the kernel work on top of Apple's source release, on Linux.
#
#   1. fetch exactly one tag of apple-oss-distributions/xnu (shallow) and check its SHA;
#   2. run the patches/*.sh stage scripts from the tree root, as darwin-xnu-build does;
#   3. apply every patches/*.patch in name order;
#   4. make the one build-system adjustment Linux needs (see below).
#
# Nothing is skipped silently. A patch that does not apply is printed with the files it
# touches and the hunks that failed; then:
#   * STRICT_PATCHES=1  - it is a failure (the patches-audit job: this is the report);
#   * otherwise         - it is a failure unless it is one of the nine names in
#                         linux/known-skips.txt, which the macOS build skips too.
# The applied + known-skip counts must add up to the number of patch files, so a patch that
# disappears (empty glob, wrong directory) fails the run.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/env.sh"
REPO=${XNU_REPO:-https://github.com/apple-oss-distributions/xnu.git}
: "${XNU_TAG:?XNU_TAG is not set}"
: "${XNU_TAG_SHA:?XNU_TAG_SHA is not set}"
: "${XNU_SRC:?XNU_SRC is not set}"
: "${PATCHES_DIR:?PATCHES_DIR is not set}"
: "${MACHINE_CONFIG:?MACHINE_CONFIG is not set}"
: "${MACOS_VERSION:?MACOS_VERSION is not set}"
STRICT_PATCHES=${STRICT_PATCHES:-0}

log() { echo "apply-xnu: $*"; }
die() { echo "apply-xnu: ERROR: $*" >&2; exit 1; }

[ -d "$PATCHES_DIR" ] || die "no patch directory: $PATCHES_DIR"
shopt -s nullglob
patch_files=("$PATCHES_DIR"/*.patch)
stage_files=("$PATCHES_DIR"/*.sh)
[ "${#patch_files[@]}" -gt 0 ] || die "no *.patch in $PATCHES_DIR"

# The patches are applied byte for byte as the repository stores them (the Linux checkout of
# the patch repository is LF; a Windows checkout with core.autocrlf=true is not, which is why
# the workflow sets it to false). No normalisation here.

# known-skips.txt: one file name per line, #-comments and blank lines ignored
declare -A known=()
if [ -f "$HERE/known-skips.txt" ]; then
	while read -r line; do
		case "$line" in ''|'#'*) continue;; esac
		known["${line%%[[:space:]]*}"]=1
	done <"$HERE/known-skips.txt"
fi

# The stage scripts are dxb's extension point: they add vendor sources (libpthread, HFS,
# BearSSL) to the tree before the patches that use them are applied. They ask xcrun for the
# SDK; before linux/setup-deps.sh has built the overlay, the theos SDK is that SDK.
if [ ! -d "$XNU_SDK_PATH/usr/include" ] && [ -d "$MACOS_SDK/usr/include" ]; then
	export XNU_SDK_PATH=$MACOS_SDK
fi

rm -rf "$XNU_SRC"
mkdir -p "$(dirname "$XNU_SRC")"
log "fetching $REPO $XNU_TAG"
git clone -q --depth 1 --branch "$XNU_TAG" --single-branch "$REPO" "$XNU_SRC"
got=$(git -C "$XNU_SRC" rev-parse HEAD)
[ "$got" = "$XNU_TAG_SHA" ] || die "$XNU_TAG is $got, expected $XNU_TAG_SHA"
log "xnu tree at $got"

for s in "${stage_files[@]}"; do
	[ -x "$s" ] || chmod +x "$s"
	log "stage script $(basename "$s")"
	# dxb runs them from the xnu source root; they create bsd/pthread, bsd/sweet_hfs...
	(cd "$XNU_SRC" && bash "$s") || die "stage script $(basename "$s") failed"
done

# report_failure PATCH: everything needed to locate the problem in the patch and the tree
report_failure() {
	local p=$1 name
	name=$(basename "$p")
	echo "::error::patch $name does not apply to $XNU_TAG"
	echo "----- git apply said:"
	sed 's/^/  /' "$XNU_SRC/.apply.err"
	echo "----- files in the patch:"
	grep -E '^diff --git|^--- a/|^\+\+\+ b/' "$p" | sed 's/^/  /' || echo "  (no file headers in the patch)"
	echo "----- patch -p1 --dry-run (hunk-level detail: which hunk, at which line, offset):"
	(cd "$XNU_SRC" && patch -p1 --dry-run --forward <"$p" 2>&1 || true) | sed 's/^/  /'
}

applied=0
skipped=()
failures=()
for p in "${patch_files[@]}"; do
	name=$(basename "$p")
	if git -C "$XNU_SRC" apply --whitespace=nowarn "$p" 2>"$XNU_SRC/.apply.err"; then
		applied=$((applied + 1))
		echo "apply-xnu: ok   $name"
		continue
	fi
	if [ "$STRICT_PATCHES" != 1 ] && [ -n "${known[$name]:-}" ]; then
		skipped+=("$name")
		echo "apply-xnu: known-skip $name (macOS skips it too; see linux/known-skips.txt)"
		sed 's/^/    /' "$XNU_SRC/.apply.err"
		continue
	fi
	report_failure "$p"
	failures+=("$name")
	[ "$STRICT_PATCHES" = 1 ] || die "patch $name failed; nothing after it was applied (applied $applied of ${#patch_files[@]})"
done

rm -f "$XNU_SRC/.apply.err"
total=${#patch_files[@]}
log "applied=$applied known-skip=${#skipped[@]} failed=${#failures[@]} total=$total"
[ $((applied + ${#skipped[@]} + ${#failures[@]})) -eq "$total" ] ||
	die "counts do not add up to $total patches"

# a known skip that starts applying is good news the list should record
for p in "${patch_files[@]}"; do
	name=$(basename "$p")
	if [ -n "${known[$name]:-}" ]; then
		case " ${skipped[*]} " in
		*" $name "*) ;;
		*) log "note: $name is listed in known-skips.txt but applied; the list is stale" ;;
		esac
	fi
done

if [ "$STRICT_PATCHES" = 1 ] && [ "${#failures[@]}" -gt 0 ]; then
	die "${#failures[@]} patch(es) do not apply: ${failures[*]}"
fi

# Fixes for the newest patches that do not compile with any compiler yet (the macOS build has
# not run since they were added). They are patches of their own, not edits to the series, so
# the series stays byte-identical to myh982516/ios-sweet-umi. See linux/fixes/ and README.md.
for f in "$HERE"/fixes/*.patch; do
	[ -f "$f" ] || continue
	if ! git -C "$XNU_SRC" apply -v --whitespace=nowarn "$f"; then
		# A diagnostic overlay must never stop the build; say what failed and
		# print the source the series left, so the patch can be fixed against
		# the real text instead of a reconstruction.
		log "WARNING: overlay fix $(basename "$f") does not apply (skipped)"
		echo "----- source as the series leaves it (fix the overlay against this):"
		sed -n '80,145p' "$XNU_SRC/iokit/Kernel/arm/SweetFramebuffer.cpp" || true
		echo "----- 200,250"
		sed -n '200,250p' "$XNU_SRC/iokit/Kernel/arm/SweetFramebuffer.cpp" || true
		echo "----- 300,330"
		sed -n '300,330p' "$XNU_SRC/iokit/Kernel/arm/SweetFramebuffer.cpp" || true
		overlay_skipped=1
		continue
	fi
	log "overlay fix $(basename "$f")"
done

# Linux build-system adjustment, the same kind darwin-xnu-build makes on macOS. XNU's
# makedefs/MakeInc.cmd hard-codes /usr/bin/xcrun and /usr/bin/codesign, and uses
# /usr/bin/plutil; none of the three exist on Linux, and the first two are what the host
# tools are built with. Point them at PATH, where linux/bin has the stubs and linux/env.sh
# has put the cctools. Nothing else is rewritten: the SDKROOT the makefiles resolve through
# xcrun is the overlay linux/setup-deps.sh builds, and the firehose path in
# makedefs/MakeInc.def is already relative to SDKROOT.
sed -i \
	-e 's|XCRUN = /usr/bin/xcrun|XCRUN = xcrun|' \
	-e 's|HOST_CODESIGN\([[:space:]]*\):=[[:space:]]*/usr/bin/codesign|HOST_CODESIGN\1:= codesign|' \
	-e 's|PLUTIL = /usr/bin/plutil|PLUTIL = plutil|' \
	"$XNU_SRC/makedefs/MakeInc.cmd"
grep -q 'XCRUN = xcrun' "$XNU_SRC/makedefs/MakeInc.cmd" ||
	die "could not point XCRUN at the xcrun on PATH"
grep -q 'HOST_CODESIGN[[:space:]]*:=[[:space:]]*codesign' "$XNU_SRC/makedefs/MakeInc.cmd" ||
	die "could not point HOST_CODESIGN at the codesign stub"
grep -q 'PLUTIL = plutil' "$XNU_SRC/makedefs/MakeInc.cmd" ||
	die "could not point PLUTIL at the plutil stub"
log "makedefs/MakeInc.cmd: XCRUN, HOST_CODESIGN and PLUTIL now resolve from PATH"

# SETUP/config/doconf, the script that turns config/MASTER.* into per-directory Makefiles, is
# the one csh script in the build and starts with #!/bin/csh -f. Linux has no /bin/csh (the
# tcsh that setup-deps builds is on PATH as `csh`); the shebang is the only thing to change.
# Linux passes the rest of the shebang as one argument, hence env -S, which splits it.
sed -i 's|^#!/bin/csh -f$|#!/usr/bin/env -S csh -f|' "$XNU_SRC/SETUP/config/doconf"
grep -q '^#!/usr/bin/env -S csh -f$' "$XNU_SRC/SETUP/config/doconf" ||
	die "could not point SETUP/config/doconf at the csh on PATH"
log "SETUP/config/doconf: runs under env -S csh -f"
