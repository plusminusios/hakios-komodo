#!/bin/bash
# Generate the DriverKit implementation stubs the kernel link needs.
#
# Apple's iig is closed source: it turns iokit/DriverKit/<Class>.iig into the RPC marshalling
# that is compiled into the kernel. There is no open implementation, so the wrappers are
# reconstructed from the linker's own list of undefined symbols:
#   * a method the kernel already implements (a *_Impl symbol that exists) is forwarded to it;
#   * everything else (RPC dispatch, user-space-only methods) returns kIOReturnUnsupported.
# Only DriverKit user-space RPC is affected; the platform, display, timer and touch code does
# not go through these wrappers.
#
# usage: linux/iig-stubs.sh <build.log> [kernel.syms]
#   kernel.syms is `nm -n` of a kernel that linked: with it, the wrappers that can be
#   forwarded are. Without it every wrapper is a stub (the first pass).
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh"
LOG=${1:?usage: iig-stubs.sh <build.log> [kernel.syms]}
SYMS=${2:-}
OUT=$TOP/linux/iig-impl
mkdir -p "$OUT"

python3 - "$LOG" "$OUT/undefined.txt" <<'PY'
import re, sys
log, out = sys.argv[1], sys.argv[2]
lines = open(log, errors='replace').read().splitlines()
# Only the *last* "Undefined symbols" block counts: the log is appended to across stub rounds,
# and a symbol that an earlier round defined must not be stubbed again (that is how the
# "__ZTV16IODispatchSource is already defined" error happened).
last = None
for i, s in enumerate(lines):
    if s.startswith('Undefined symbols'):
        last = i
syms = []
if last is not None:
    for s in lines[last + 1:]:
        m = re.match(r'\s+"([^"]+)"', s)
        if m:
            syms.append(m.group(1))
            continue
        if not s.strip() or s.startswith('ld:'):
            break
syms = list(dict.fromkeys(syms))
open(out, 'w').write('\n'.join(syms) + '\n')
print('iig-stubs: %d undefined symbols in the last link of %s' % (len(syms), log))
PY

before=$(sha256sum "$OUT/OSObject.iig.cpp" 2>/dev/null | cut -d' ' -f1 || true)
# Accumulate: every round adds the symbols the previous stub file does not cover yet. The kernel
# needs all of them at once (the 16 other iig classes are empty translation units), so the set
# only grows. undefined-all.txt must exist on the first round, otherwise `cat` fails under
# `set -e` and the whole build script stops after the (expected) first link failure.
: >>"$OUT/undefined-all.txt"
cat "$OUT/undefined.txt" "$OUT/undefined-all.txt" | sed '/^$/d' | sort -u >"$OUT/undefined-all.txt.tmp"
mv "$OUT/undefined-all.txt.tmp" "$OUT/undefined-all.txt"
echo "iig-stubs: $(wc -l <"$OUT/undefined-all.txt") symbols in the accumulated set"
python3 "$TOP/linux/gen-iig-stubs.py" "$OUT/undefined-all.txt" "$OUT" $SYMS
after=$(sha256sum "$OUT/OSObject.iig.cpp" | cut -d' ' -f1)
if [ "$before" = "$after" ]; then echo "iig-stubs: no change"; else echo "iig-stubs: regenerated $OUT/OSObject.iig.cpp"; fi
