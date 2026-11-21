#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

usage() {
    echo "Usage: $0 smp | kraken <release-tag>" >&2
    exit 2
}

if [[ $# -eq 0 ]]; then
    usage
fi
if [[ -n $(git status --porcelain) ]]; then
    echo 'Commit or stash local changes before merging an upstream update.' >&2
    exit 1
fi

case "$1" in
    smp)
        [[ $# -eq 1 ]] || usage
        git fetch --no-tags https://github.com/drakmor/ShadowMountPlus.git \
            refs/heads/1.7:refs/remotes/smp-upstream/1.7
        git merge --no-edit refs/remotes/smp-upstream/1.7
        ;;
    kraken)
        [[ $# -eq 2 && $2 == v* ]] || usage
        tag="refs/tags/$2"
        git check-ref-format "$tag"
        git fetch --no-tags https://github.com/quer3q/kraken-mount.git "$tag"
        revision=$(git rev-parse 'FETCH_HEAD^{commit}')
        work=$(mktemp -d vendor/.kraken-sync.XXXXXX)
        trap 'if [[ ! -d "$work/previous" ]]; then rm -rf -- "$work"; fi' EXIT
        stage="$work/new"
        mkdir "$stage"
        git archive "$revision" LICENSE THIRD_PARTY_NOTICES crates/kraken-mount-pkg \
            src/ampr_index.rs vendor/ampr-emu-ng/libSceAmpr.sprx \
            vendor/ampr-emu-ng/VERSION vendor/ampr-emu-ng/SHA256SUMS |
            tar -xf - -C "$stage"
        (
            cd "$stage/vendor/ampr-emu-ng"
            awk '$2 == "libSceAmpr.sprx" {print; ++matches} END {exit matches != 1}' SHA256SUMS |
                sha256sum --check --strict -
        )
        patch --batch --forward -p1 -d "$stage" < tools/kraken-index.patch
        printf '%s' "$revision" > "$stage/REVISION"

        mv vendor/kraken "$work/previous"
        if ! mv "$stage" vendor/kraken; then
            mv "$work/previous" vendor/kraken || printf 'Previous Kraken files: %s/previous\n' "$work" >&2
            exit 1
        fi
        rm -rf -- "$work/previous"
        printf 'Imported Kraken Mount %s (%s). Review the diff and run the PS5 build before committing.\n' "$2" "$revision"
        ;;
    *) usage ;;
esac
