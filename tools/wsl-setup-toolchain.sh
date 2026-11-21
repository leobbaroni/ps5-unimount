#!/usr/bin/env bash
# Toolchain setup for upstream baseline builds (WSL2 Ubuntu 26.04, run as root).
#
#   wsl -d Ubuntu -u root -- bash /mnt/c/Users/TugaPlayz/Desktop/GITHUB/ps5-unimount/tools/wsl-setup-toolchain.sh
#
# Installs:
#   - apt build deps (ShadowMountPlus CI list; clang/lld/llvm 21 instead of 18, see SMP_LLVM below)
#   - ps5-payload-sdk (git HEAD, pacbrew-repo/sdk PKGBUILD) at /opt/ps5-payload-sdk, owned by pacman
#   - pacbrew libs into /opt/ps5-payload-sdk/target/user/homebrew via makepkg + pacman, as SMP CI does:
#     openlibm zlib libpng libmicrohttpd libjpeg-turbo sqlite libsmb2
#   - Kraken Mount toolchain (ps5/Dockerfile): pinned SDK v0.43 zip at /opt/ps5-payload-sdk-0.43,
#     SQLite amalgamation 3.46.1 at /opt/sqlite, rustup 1.99.0 + rust-src (patched by
#     ps5/prepare-rust-std.py), clang-19/lld-19/llvm-19 (KM_LLVM below)
#
# Idempotent: already-installed pieces are skipped. REBUILD_PACBREW=1 rebuilds all pacbrew packages
# (the sdk package always tracks git HEAD at build time).
set -euo pipefail

UPSTREAM=${UPSTREAM:-/mnt/c/Users/TugaPlayz/Desktop/GITHUB/ps5-unimount/_upstream}
SRC=${SRC:-/opt/build/src}
PACBREW_REPO_URL=https://github.com/ps5-payload-dev/pacbrew-repo
PACBREW_REPO_REF=${PACBREW_REPO_REF:-}   # empty = HEAD (as SMP CI); set a commit to pin
PACBREW_USER=pacbrew                     # makepkg refuses to run as root
SMP_SDK=/opt/ps5-payload-sdk
KM_SDK=/opt/ps5-payload-sdk-0.43
KM_SDK_URL=https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip
KM_SDK_SHA256=a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb
SQLITE_URL=https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
SQLITE_SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
RUST_TOOLCHAIN=1.99.0

[[ $(id -u) -eq 0 ]] || { echo "run as root" >&2; exit 1; }
log() { printf '\n=== %s\n' "$*"; }

log "apt packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# SMP CI list (clang-18/lld-18 replaced by the distro's clang 21) + makepkg's fakeroot,
# + clang-19/lld-19/llvm-19 for Kraken Mount (its Dockerfile), + unzip/wget/ca-certificates.
# tcl: pacbrew sqlite's `make sqlite3.h libsqlite3.la` needs tclsh (preinstalled on GitHub runners).
# nodejs: Kraken Mount CI's `node --check` of the UI script.
apt-get install -y -qq --no-install-recommends \
  autoconf automake build-essential clang-21 lld-21 llvm-21 cmake curl git libarchive-tools \
  libtool makepkg meson nasm pacman-package-manager patch pkg-config python3 python3-yaml xxd zip \
  unzip wget ca-certificates fakeroot file tcl nodejs \
  clang-19 lld-19 llvm-19 >/dev/null

git config --global core.autocrlf false
git config --global --get-all safe.directory | grep -qx '\*' || git config --global --add safe.directory '*'

log "pacman configuration (as SMP CI)"
install -d -m 755 /var/lib/pacman/local /var/cache/pacman/pkg
cat > /etc/pacman.conf <<'EOF'
[options]
Architecture = auto
SigLevel = Required DatabaseOptional
LocalFileSigLevel = Optional
EOF
id -u "$PACBREW_USER" >/dev/null 2>&1 || useradd -m -s /bin/bash "$PACBREW_USER"

log "pacbrew-repo"
mkdir -p "$SRC"
if [[ ! -d $SRC/pacbrew-repo/.git ]]; then
  git clone -q "$PACBREW_REPO_URL" "$SRC/pacbrew-repo"
fi
if [[ -n $PACBREW_REPO_REF ]]; then
  git -C "$SRC/pacbrew-repo" fetch -q origin
  git -C "$SRC/pacbrew-repo" checkout -q "$PACBREW_REPO_REF"
fi
chown -R "$PACBREW_USER:$PACBREW_USER" "$SRC/pacbrew-repo"
echo "pacbrew-repo $(git -C "$SRC/pacbrew-repo" rev-parse HEAD)"

pacbrew() { # pkgdir-name, pacman package name, [pacman -U extra args]
  local dir=$1 name=$2; shift 2
  if [[ ${REBUILD_PACBREW:-0} != 1 ]] && pacman -Q "$name" >/dev/null 2>&1; then
    echo "$(pacman -Q "$name") already installed"; return
  fi
  log "makepkg $dir"
  rm -f "$SRC/pacbrew-repo/$dir"/ps5-payload-*.pkg.tar.gz
  # PS5_PAYLOAD_SDK is the PKGBUILDs' own default; set explicitly so every makepkg stage sees it.
  runuser -u "$PACBREW_USER" -- env -C "$SRC/pacbrew-repo/$dir" PS5_PAYLOAD_SDK="$SMP_SDK" \
    makepkg -c -f --noconfirm
  pacman --noconfirm -U "$@" "$SRC/pacbrew-repo/$dir"/ps5-payload-*.pkg.tar.gz
}
# A hand-installed SDK at the same path is replaced by the package (same sdk git HEAD).
pacbrew sdk ps5-payload-sdk --overwrite '*'
pacbrew openlibm ps5-payload-openlibm
pacbrew zlib ps5-payload-zlib
pacbrew libpng ps5-payload-libpng
pacbrew libmicrohttpd ps5-payload-libmicrohttpd
pacbrew libjpeg-turbo ps5-payload-libjpeg-turbo
pacbrew sqlite ps5-payload-sqlite
pacbrew libsmb2 ps5-payload-libsmb2
test -f $SMP_SDK/target/user/homebrew/include/smb2/smb2.h
test -f $SMP_SDK/target/user/homebrew/include/smb2/libsmb2-raw.h
test -s $SMP_SDK/target/user/homebrew/lib/libsmb2.a

# SMP's Makefile needs the sdk checkout's sce_stubs (CI: PS5_SCE_STUBS_DIR=<sdk checkout>/sce_stubs).
# Clone it from makepkg's bare mirror, so it is the exact commit the sdk package was built from.
SDK_MIRROR=$SRC/pacbrew-repo/sdk/sdk
SDK_COMMIT=$(git -C "$SDK_MIRROR" rev-parse HEAD)
if [[ $(git -C "$SRC/ps5-sdk" rev-parse HEAD 2>/dev/null) != "$SDK_COMMIT" ]]; then
  rm -rf "$SRC/ps5-sdk"
  git clone -q "$SDK_MIRROR" "$SRC/ps5-sdk"
fi
test -f "$SRC/ps5-sdk/sce_stubs/libkernel_sys.c"
echo "sdk checkout $(git -C "$SRC/ps5-sdk" rev-parse HEAD) ($(git -C "$SRC/ps5-sdk" describe --tags --always))"

log "Kraken Mount: SDK v0.43 at $KM_SDK"
fetch_verify() { # url sha256 dest
  curl -fsSL --proto =https --retry 3 "$1" -o "$3"
  echo "$2  $3" | sha256sum -c -
}
if [[ ! -x $KM_SDK/bin/prospero-clang ]]; then
  tmp=$(mktemp -d)
  fetch_verify "$KM_SDK_URL" "$KM_SDK_SHA256" "$tmp/sdk.zip"
  unzip -q "$tmp/sdk.zip" -d "$tmp/x"
  rm -rf "$KM_SDK"
  mv "$tmp/x/ps5-payload-sdk" "$KM_SDK"
  rm -rf "$tmp"
fi
test -x "$KM_SDK/bin/prospero-clang"

log "Kraken Mount: SQLite amalgamation 3.46.1 at /opt/sqlite"
if [[ ! -f /opt/sqlite/sqlite3.c || ! -f /opt/sqlite/sqlite3.h ]]; then
  tmp=$(mktemp -d)
  fetch_verify "$SQLITE_URL" "$SQLITE_SHA256" "$tmp/sqlite.zip"
  unzip -q "$tmp/sqlite.zip" -d "$tmp"
  rm -rf /opt/sqlite
  mv "$tmp/sqlite-amalgamation-3460100" /opt/sqlite
  rm -rf "$tmp"
fi
test -f /opt/sqlite/sqlite3.c && test -f /opt/sqlite/sqlite3.h

log "Kraken Mount: rustup $RUST_TOOLCHAIN + rust-src"
export PATH=$HOME/.cargo/bin:$PATH
if ! command -v rustup >/dev/null; then
  curl -fsSL --proto =https https://sh.rustup.rs | sh -s -- -y -q --profile minimal --default-toolchain none --no-modify-path
fi
rustup toolchain install "$RUST_TOOLCHAIN" --profile minimal \
  --component rust-src --target x86_64-unknown-freebsd
# The FreeBSD 11 d_ino patch of std's rust-src (idempotent: the script skips if applied).
RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN python3 "$UPSTREAM/kraken-mount/ps5/prepare-rust-std.py"

log "versions"
clang-21 --version | head -1
clang-19 --version | head -1
pacman -Q
RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN rustc -V
RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN cargo -V
echo "done"
