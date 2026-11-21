#!/usr/bin/env bash
# Build the payload inside WSL/Linux from this checkout.
#
# The checkout may live on a slow Windows mount (/mnt/c/...), so the tree is
# mirrored to a native Linux directory first and built there. The finished ELF
# is copied back to <checkout>/build/.
#
#   tools/wsl-build.sh            # incremental build
#   tools/wsl-build.sh clean      # make clean, then build
#   tools/wsl-build.sh test       # host regression tests + cargo test, no PS5 build
#
# Toolchain: see tools/wsl-setup-toolchain.sh.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="${UNIMOUNT_BUILD_DIR:-/opt/build/unimount}"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
SCE_STUBS="${PS5_SCE_STUBS_DIR:-/opt/src/sdk/sce_stubs}"
MODE="${1:-build}"

# shellcheck disable=SC1091
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"

VERSION_TAG="$(git -c safe.directory='*' -C "$SRC" describe --abbrev=6 --dirty --always --tags 2>/dev/null || echo unknown)"

mkdir -p "$DST"
rsync -a --delete \
    --exclude '/_upstream/' --exclude '/.git/' --exclude '/build/' \
    --exclude '/.deps/' --exclude '/kraken-bridge/target/' --exclude '/.impeccable/' \
    --exclude '*.o' --exclude '*.elf' --exclude '/src/*_asset.c' --exclude '/src/libkernel_sys_ext.so' \
    "$SRC"/ "$DST"/
# pkg_helper_asset.c is tracked upstream, so the *_asset.c exclude must not drop it.
cp -f "$SRC/src/pkg_helper_asset.c" "$DST/src/pkg_helper_asset.c" 2>/dev/null || true

cd "$DST"

if [ "$MODE" = test ]; then
    cargo test --locked --manifest-path kraken-bridge/Cargo.toml
    for t in tools/test_*.py; do
        echo "== $t"
        python3 "$t"
    done
    exit 0
fi

if [ "$MODE" = clean ]; then
    make clean
fi

make -j"$(nproc)" all PS5_SCE_STUBS_DIR="$SCE_STUBS" VERSION_TAG="$VERSION_TAG"

mkdir -p "$SRC/build"
cp -f shadowmountplus.elf "$SRC/build/unimount.elf"
( cd "$SRC/build" && sha256sum unimount.elf > unimount.elf.sha256 )
ls -l "$SRC/build/unimount.elf"
echo "version: $VERSION_TAG"
