#!/usr/bin/env bash
# Build the test-only launch helper (tools/hwtest/hwtest.c) inside WSL/Linux.
# Output: <checkout>/build/unimount-hwtest.elf
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
if [ -z "${LLVM_CONFIG:-}" ] && [ -x /usr/bin/llvm-config-18 ]; then
    export LLVM_CONFIG=/usr/bin/llvm-config-18
fi
OUT="$(mktemp -d)"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -O2 -Wall -Wextra -o "$OUT/unimount-hwtest.elf" "$HERE/hwtest.c" \
    -lSceSystemService -lSceUserService
"$PS5_PAYLOAD_SDK/bin/prospero-strip" --strip-all "$OUT/unimount-hwtest.elf"
mkdir -p "$ROOT/build"
cp -f "$OUT/unimount-hwtest.elf" "$ROOT/build/unimount-hwtest.elf"
rm -rf "$OUT"
ls -l "$ROOT/build/unimount-hwtest.elf"
