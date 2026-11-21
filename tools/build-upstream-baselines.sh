#!/usr/bin/env bash
# Baseline builds of the two upstream payloads, unmodified (WSL2 Ubuntu 26.04, root).
# Run tools/wsl-setup-toolchain.sh first.
#
#   wsl -d Ubuntu -u root -- bash /mnt/c/Users/TugaPlayz/Desktop/GITHUB/ps5-unimount/tools/build-upstream-baselines.sh
#
# Clones the committed HEADs of _upstream/{shadowMountPlus,kraken-mount} into /opt/build (LF
# checkouts; _upstream is only read), builds both, runs the host checks and copies the ELFs to
# _upstream/_baseline/. Build only: nothing here talks to a console.
set -euo pipefail

UPSTREAM=${UPSTREAM:-/mnt/c/Users/TugaPlayz/Desktop/GITHUB/ps5-unimount/_upstream}
BUILD=${BUILD:-/opt/build}
SRC=$BUILD/src
BASELINE=$UPSTREAM/_baseline
SMP_SDK=/opt/ps5-payload-sdk
KM_SDK=/opt/ps5-payload-sdk-0.43
KM_LLVM=${KM_LLVM:-19}
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

declare -A RESULTS
ORDER=()
record() { RESULTS[$1]=$2; ORDER+=("$1"); }

########################################################################### ShadowMountPlus
# Recipe: .github/workflows/ps5.yml (job build), after the pacbrew packages (setup script).
log "ShadowMountPlus"
sync_repo shadowMountPlus .deps
cd "$BUILD/shadowMountPlus"
export PS5_PAYLOAD_SDK=$SMP_SDK

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
smp_test() { # name, command...
  local name=$1; shift
  echo "--- $name"
  if "$@"; then record "smp: $name" PASS; else record "smp: $name" "FAIL (rc=$?)"; fi
}
smp_test "test_runtime_config.py --sanitize" python3 tools/test_runtime_config.py --sanitize
smp_test "test_shellcore.py --sanitize" python3 tools/test_shellcore.py --sanitize
smp_test "test_scan_permissions.py" python3 tools/test_scan_permissions.py
smp_test "test_shellcore_launch.py --sanitize" python3 tools/test_shellcore_launch.py --sanitize
smp_test "test_sandbox_cleanup.py" python3 tools/test_sandbox_cleanup.py

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
km_test() {
  local name=$1; shift
  echo "--- $name"
  if "$@"; then record "km: $name" PASS; else record "km: $name" "FAIL (rc=$?)"; fi
}
km_test "cargo check --workspace --locked --target x86_64-unknown-freebsd (-Dwarnings)" \
  env RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN RUSTFLAGS=-Dwarnings CARGO_TARGET_DIR=target/host \
  cargo check --workspace --locked --target x86_64-unknown-freebsd
km_test "node --check src/ui.html script" bash -c '
  sed -n "/^<script>$/,/^<\/script>$/p" src/ui.html | sed "1d;\$d" > /tmp/km-ui.js
  test -s /tmp/km-ui.js && node --check /tmp/km-ui.js'
km_test "cargo test --workspace (host)" \
  env RUSTUP_TOOLCHAIN=$RUST_TOOLCHAIN CARGO_TARGET_DIR=target/host cargo test --workspace --locked

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
  printf '%-80s %s\n' "$k" "${RESULTS[$k]}"
  [[ ${RESULTS[$k]} == PASS ]] || fail=1
done
exit $fail
