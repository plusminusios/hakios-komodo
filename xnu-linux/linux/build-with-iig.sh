#!/bin/bash
# make the kernel, and if the link is short of DriverKit symbols, let linux/iig-stubs.sh
# reconstruct them from the linker's list and link again.
#
# make's exit status is not the answer: config/Makefile's export check runs after the link and
# fails for any kernel, so the build is run with -k and judged by the kernel file and XNU's
# "built kernel at" line. Two rounds of stubs are possible:
#   1. link fails with undefined symbols -> generate every wrapper as kIOReturnUnsupported;
#   2. the kernel links -> regenerate, forwarding the wrappers whose _Impl the kernel has,
#      and relink. That is the shape the iig-generated code would have.
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh"
LOG=$TOP/build.log
K=$XNU_SRC/BUILD/obj/${KERNEL_CONFIG}_ARM64_${MACHINE_CONFIG}/kernel.$(echo "$KERNEL_CONFIG" | tr 'A-Z' 'a-z').$(echo "$MACHINE_CONFIG" | tr 'A-Z' 'a-z')
: >"$LOG"

run() {
	bash "$TOP/linux/build.sh" -k 2>&1 | tee -a "$LOG"
}

# iokit/DriverKit/Makefile's rule is
#     %.h : %.iig
#         $(IIG) --def $< --header $@ --impl $(patsubst %.h,%.iig.cpp,$@) ...
# so the implementation is written next to whatever .h it generated. In this build the generated
# headers end up in BUILD/obj/<config>/iokit/<KERNEL_CONFIG>/DriverKit/, one level below the
# object (BUILD/obj/<config>/iokit/<KERNEL_CONFIG>/OSObject.iig.cpo). Steps, in this order:
#   1. write the stub next to each generated OSObject.h and in the source tree, and touch it;
#   2. remove every DriverKit object in that layer so make compiles them again.
# The empty implementations of the other classes must be left alone: they are side effects of
# their .h rule, and with the .h in place make has no rule to recreate them ("No rule to make
# target 'DriverKit/OSAction.iig.cpp'"). They are empty because all the symbols they would carry
# are defined once, in OSObject.iig.cpp.
install_stub() {
	local stub=$TOP/linux/iig-impl/OSObject.iig.cpp hdr d
	cp "$stub" "$XNU_SRC/iokit/DriverKit/OSObject.iig.cpp"
	touch "$XNU_SRC/iokit/DriverKit/OSObject.iig.cpp"
	while IFS= read -r hdr; do
		d=$(dirname "$hdr")
		cp "$stub" "$d/OSObject.iig.cpp"
		touch "$d/OSObject.iig.cpp"
		echo "build-with-iig: stub -> $d/OSObject.iig.cpp (next to its generated header)"
	done < <(find "$XNU_SRC/BUILD/obj" -name 'OSObject.h' 2>/dev/null)
	local objroot=$XNU_SRC/BUILD/obj/${KERNEL_CONFIG}_ARM64_${MACHINE_CONFIG} gd
	for gd in "$objroot/iokit/${KERNEL_CONFIG}" "$objroot/iokit"; do
		[ -d "$gd" ] || continue
		rm -f "$gd"/*.iig.cpo
		echo "build-with-iig: removed $gd/*.iig.cpo"
	done
	echo "build-with-iig: tree file now $(wc -c <"$XNU_SRC/iokit/DriverKit/OSObject.iig.cpp") bytes, $(grep -c '__iig_stub_' "$XNU_SRC/iokit/DriverKit/OSObject.iig.cpp") stubs"
}

# After a rebuild, say whether the objects that got compiled really carry the stubs.
show_stub_objects() {
	local f n missing=0
	for f in $(find "$XNU_SRC/BUILD/obj" -name 'OSObject.iig.cpp' 2>/dev/null); do
		n=$(grep -c 'kIOReturnUnsupported\|_Impl' "$f" || true)
		[ "$n" -gt 0 ] || { echo "STUB MISSING: $f"; missing=$((missing + 1)); }
	done
	echo "build-with-iig: OSObject.iig.cpp copies with stubs: $(find "$XNU_SRC/BUILD/obj" -name 'OSObject.iig.cpp' -exec grep -l 'kIOReturnUnsupported\|_Impl' {} + 2>/dev/null | wc -l), missing: $missing"
	for f in "$XNU_SRC"/BUILD/obj/${KERNEL_CONFIG}_ARM64_${MACHINE_CONFIG}/iokit/*/OSObject.iig.cpo \
		"$XNU_SRC"/BUILD/obj/${KERNEL_CONFIG}_ARM64_${MACHINE_CONFIG}/iokit/OSObject.iig.cpo; do
		[ -f "$f" ] || continue
		n=$(deps/host/bin/nm -g "$f" 2>/dev/null | wc -l)
		echo "build-with-iig: object $f: $n global symbols, $(deps/host/bin/nm "$f" 2>/dev/null | grep -c 'IOEventLink') IOEventLink, $(deps/host/bin/nm "$f" 2>/dev/null | grep -c 'IOUserServer') IOUserServer"
	done
}

echo "build-with-iig: kernel will be $K"
run
for attempt in 1 2 3 4; do
	[ -s "$K" ] && break
	if ! grep -q 'Undefined symbols' "$LOG"; then
		echo "build-with-iig: no kernel and the linker did not report undefined symbols; see $LOG"
		exit 1
	fi
	echo "build-with-iig: generating DriverKit stubs from $LOG (attempt $attempt)"
	bash "$TOP/linux/iig-stubs.sh" "$LOG" || { echo "build-with-iig: stub generation failed"; exit 1; }
	install_stub
	# this link is expected to fail; its failure must not stop the loop (build.sh's status is ignored)
	run
	show_stub_objects
done
if [ ! -s "$K" ]; then
	echo "build-with-iig: still no kernel after $attempt stub rounds; last undefined symbols:"
	sed -n '/Undefined symbols/,$p' "$LOG" | grep -E '^\s+"' | sed 's/^/  /' | sort -u | head -80
	exit 1
fi

# second pass: with the kernel's own symbols known, forward the wrappers that have an _Impl
if deps/host/bin/nm -n "$K.unstripped" >"$TOP/kernel.syms" 2>/dev/null && [ -s "$TOP/kernel.syms" ]; then
	before=$(sha256sum "$TOP/linux/iig-impl/OSObject.iig.cpp" 2>/dev/null | cut -d' ' -f1)
	bash "$TOP/linux/iig-stubs.sh" "$LOG" "$TOP/kernel.syms" || true
	after=$(sha256sum "$TOP/linux/iig-impl/OSObject.iig.cpp" 2>/dev/null | cut -d' ' -f1)
	if [ "$before" != "$after" ]; then
		echo "build-with-iig: relinking with the wrappers that forward to their _Impl"
		install_stub
		run
	fi
fi

grep -q 'built kernel at' "$LOG" || { echo "build-with-iig: XNU never printed 'built kernel at'"; exit 1; }
grep 'built kernel at' "$LOG" | tail -1
ls -la "$K"
