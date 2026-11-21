#!/usr/bin/env bash
# Baseline builds of the two upstream payloads, unmodified (WSL2 Ubuntu 26.04, root).
# Run tools/wsl-setup-toolchain.sh first.
#
#   wsl -d Ubuntu -u root -- bash <checkout>/tools/build-upstream-baselines.sh
#
# Clones the committed HEADs of _upstream/{shadowMountPlus,kraken-mount} into /opt/build (LF
# checkouts; _upstream is only read), builds both, runs the host checks and copies the ELFs to
# _upstream/_baseline/. Build only: nothing here talks to a console.
set -euo pipefail

UPSTREAM=${UPSTREAM:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/_upstream}
BUILD=${BUILD:-/opt/build}
SRC=$BUILD/src
BASELINE=$UPSTREAM/_baseline
SMP_SDK=/opt/ps5-payload-sdk
KM_SDK=/opt/ps5-payload-sdk-0.43
# Compilers: SMP CI uses clang-18, the Kraken Dockerfile clang-19; the distro's clang 21 builds both
# (both SDKs' prospero-clang let clang >= 20 add its own crt objects, which puts _start at 0x0, so
# e_entry is 0x0; with 18/19 the SDK passes crt1.o and e_entry is _start's address further in).
# SMP_LLVM=18 / KM_LLVM=19 need setup with INSTALL_UPSTREAM_CLANGS=1.
SMP_LLVM=${SMP_LLVM:-21}
KM_LLVM=${KM_LLVM:-21}
RUST_TOOLCHAIN=1.99.0
export PATH=$HOME/.cargo/bin:$PATH

[[ $(id -u) -eq 0 ]] || { echo "run as root" >&2; exit 1; }
log() { printf '\n=== %s\n' "$*"; }
git config --global core.autocrlf false

# Fresh LF working copy of the upstream repo's committed HEAD; keeps listed cache dirs.
sync_repo() { # name, [paths to keep]
  local name=$1 dest=$BUILD/$1; shift
  if [[ ! -d $dest/.git ]]; then
    git clone -q "$UPSTREAM/$name" "$dest"
  else
    git -C "$dest" fetch -q --tags origin
    git -C "$dest" checkout -q -f --detach "$(git -C "$UPSTREAM/$name" rev-parse HEAD)"
    local keep=() p
    for p in "$@"; do keep+=(-e "$p"); done
    git -C "$dest" clean -q -fdx "${keep[@]}"
  fi
  echo "$name $(git -C "$dest" rev-parse HEAD) ($(git -C "$dest" describe --abbrev=6 --dirty --always --tags))"
  if git -C "$dest" ls-files --eol | grep -q 'w/crlf'; then
    echo "note: $name has CRLF files committed (as upstream):"
    git -C "$dest" ls-files --eol | grep 'w/crlf' | sed 's/^/  /'
  fi
}

declare -A RESULTS INFO
ORDER=()
record() { RESULTS[$1]=$2; ORDER+=("$1"); }
# check NAME [--info] -- COMMAND...: --info results are reported but don't fail the script.
check() {
  local name=$1 info=0; shift
  [[ $1 == --info ]] && { info=1; shift; }
  [[ $1 == -- ]] && shift
  echo "--- $name"
  INFO[$name]=$info
  if "$@"; then record "$name" PASS; else record "$name" "FAIL (rc=$?)"; fi
}

########################################################################### ShadowMountPlus
# Recipe: .github/workflows/ps5.yml (job build), after the pacbrew packages (setup script).
log "ShadowMountPlus"
sync_repo shadowMountPlus .deps
cd "$BUILD/shadowMountPlus"
export PS5_PAYLOAD_SDK=$SMP_SDK
export LLVM_CONFIG=/usr/bin/llvm-config-$SMP_LLVM   # read by the SDK's prospero-llvm-config

make json-c

# CI: "Check json-c release memory ownership"
python3 tools/build_ps5_json_c.py --host --sanitize --prefix /tmp/smp-json-release
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I/tmp/smp-json-release/include tools/check_json_c_release.c \
  /tmp/smp-json-release/lib/libjson-c.a -lm -o /tmp/smp-json-release-test
ASAN_OPTIONS=detect_leaks=1 /tmp/smp-json-release-test

# CI: "Check API tools and manifest"
python3 -m py_compile tools/api_test.py
python3 -c 'import pathlib, yaml; yaml.safe_load(pathlib.Path("docs/openapi.yaml").read_text(encoding="utf-8"))'

# CI: "Build payload"
make clean
make -j4 all PS5_SCE_STUBS_DIR="$SRC/ps5-sdk/sce_stubs"
test -s shadowmountplus.elf

log "ShadowMountPlus host regression tests (tests/README.md)"
# ASan initialised fine under this WSL kernel, so no `setarch x86_64 -R` prefix is needed.
check "smp: test_runtime_config.py --sanitize" -- python3 tools/test_runtime_config.py --sanitize
check "smp: test_shellcore.py --sanitize" -- python3 tools/test_shellcore.py --sanitize
check "smp: test_scan_permissions.py" -- python3 tools/test_scan_permissions.py
check "smp: test_shellcore_launch.py --sanitize" -- python3 tools/test_shellcore_launch.py --sanitize
check "smp: test_sandbox_cleanup.py" -- python3 tools/test_sandbox_cleanup.py
# Not in tests/README.md. Fails to compile with Ubuntu 26.04's gcc (-Werror=format-truncation in
# tests/test_storage_paths.c:77): informational.
check "smp: test_storage_paths.py --sanitize (not in README)" --info -- \
  python3 tools/test_storage_paths.py --sanitize

########################################################################### Kraken Mount
# Recipe: ps5/Dockerfile + `ps5/build.sh --inside`. build.sh/linker.sh hard-code /work, so /work
# is a symlink to the build copy (cargo resolves it to the real path; linker.sh only reads /work).
log "Kraken Mount"
sync_repo kraken-mount target/ps5/cargo
ln -sfn "$BUILD/kraken-mount" /work
cd /work
(
  export PS5_PAYLOAD_SDK=$KM_SDK
  export LLVM_CONFIG=/usr/bin/llvm-config-$KM_LLVM
  export PATH=/usr/lib/llvm-$KM_LLVM/bin:$PATH
  export RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN RUSTUP_AUTO_INSTALL=0
  export CARGO_HOME=/work/target/ps5/cargo
  /work/ps5/build.sh --inside
)
test -s target/ps5/kraken-mount.elf

log "Kraken Mount host checks (CI job 'check'; AGENTS.md: no cargo test)"
check "km: cargo check --workspace --locked --target x86_64-unknown-freebsd (-Dwarnings)" -- \
  env RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN RUSTFLAGS=-Dwarnings CARGO_TARGET_DIR=target/host \
  cargo check --workspace --locked --target x86_64-unknown-freebsd
check "km: node --check src/ui.html script" -- bash -c '
  sed -n "/^<script>$/,/^<\/script>$/p" src/ui.html | sed "1d;\$d" > /tmp/km-ui.js
  test -s /tmp/km-ui.js && node --check /tmp/km-ui.js'
check "km: cargo test -p kraken-mount-pkg (host)" -- \
  env RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN CARGO_TARGET_DIR=target/host cargo test -p kraken-mount-pkg --locked
# The bin crate only compiles for FreeBSD/PS5 (libc statfs fields etc.; AGENTS.md: "no cargo
# test"): informational, output kept out of the log.
check "km: cargo test --workspace (host; bin crate is FreeBSD-only)" --info -- bash -c '
  env RUSTUP_TOOLCHAIN='"$RUST_TOOLCHAIN"' CARGO_TARGET_DIR=target/host cargo test --workspace --locked \
    >/tmp/km-cargo-test-workspace.log 2>&1 ||
  { echo "$(grep -c "^error" /tmp/km-cargo-test-workspace.log) error lines: /tmp/km-cargo-test-workspace.log"; false; }'

########################################################################### Outputs
log "copy to $BASELINE"
mkdir -p "$BASELINE"
cp "$BUILD/shadowMountPlus/shadowmountplus.elf" "$BASELINE/shadowmountplus-upstream.elf"
cp "$BUILD/kraken-mount/target/ps5/kraken-mount.elf" "$BASELINE/kraken-mount-upstream.elf"
cd "$BASELINE"
ls -l shadowmountplus-upstream.elf kraken-mount-upstream.elf
sha256sum shadowmountplus-upstream.elf kraken-mount-upstream.elf
for f in shadowmountplus-upstream.elf kraken-mount-upstream.elf; do
  echo "--- $f"; llvm-readelf-21 -h -d "$f" | grep -E 'Type:|Entry point|NEEDED|SONAME|FLAGS'
done

log "results"
fail=0
for k in "${ORDER[@]}"; do
  tag=; [[ ${INFO[$k]} == 1 ]] && tag=" [informational]"
  printf '%-80s %s%s\n' "$k" "${RESULTS[$k]}" "$tag"
  [[ ${RESULTS[$k]} == PASS || ${INFO[$k]} == 1 ]] || fail=1
done
exit $fail
