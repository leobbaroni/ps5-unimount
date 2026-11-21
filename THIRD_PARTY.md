# Third-party components

UniMount is distributed under GPL-3.0 (`LICENSE`). It contains or links the components below. Where a licence is not stated in this repository the entry says "see upstream" rather than guessing. Source files carry their own notices and take precedence over this summary.

## Code and binaries in the tree

| Component | Origin | Licence | Where it lives |
| --- | --- | --- | --- |
| ShadowMountPlus 1.7 (the engine this repository forks) | drakmor, <https://github.com/drakmor/ShadowMountPlus>; built on ShadowMount by VoidWhisper | GPL-3.0 (`LICENSE`) | `src/`, `include/`, config examples and image scripts in the root; `web/index.html` is UniMount's own interface |
| Kraken Mount package reader and index writer | quer3q, <https://github.com/quer3q/kraken-mount>, v0.3.1 | GPL-3.0-or-later (`vendor/kraken/LICENSE`; the vendored crate declares `GPL-3.0-or-later`) | `vendor/kraken/crates/kraken-mount-pkg/`, `vendor/kraken/src/ampr_index.rs`; imported by `tools/sync-upstream.sh kraken`; revision in `vendor/kraken/REVISION` |
| Kraken backend integration (`smp-kraken-bridge`) | Vov4k957, <https://github.com/Vov4k957/shadowmountplus-kraken> | GPL-3.0-or-later (`kraken-bridge/Cargo.toml`) | `kraken-bridge/`, `src/sm_kraken.c`, `tools/kraken-index.patch` |
| ampr-emu-ng v0.5.0, prebuilt `libSceAmpr.sprx` | quer3q, <https://github.com/quer3q/ampr-emu-ng> | Not stated in this tree: see upstream. The binary is embedded in the payload and written into game caches | `vendor/kraken/vendor/ampr-emu-ng/` (with `SHA256SUMS`, `VERSION`); embedded by `src/kraken_assets.S` |
| pgo_stub 0.6, prebuilt `libScePlayGo.sprx` | drakmor, <https://github.com/drakmor/pgo_stub> | GPL-3.0 per `vendor/kraken/THIRD_PARTY_NOTICES`; corresponding source <https://github.com/drakmor/pgo_stub/tree/0.6> | `vendor/pgo_stub/` (with `SHA256SUMS`, `VERSION`); embedded by `src/kraken_assets.S` |
| dlc_emu by drakmor | <https://github.com/drakmor/dlc_emu> | GPL-3.0 per `vendor/kraken/THIRD_PARTY_NOTICES` (that notice covers release 0.3) | Not vendored in this tree. Credited because Kraken Mount's upstream notices list it and the Kraken path stages the `dlc_emu.ini` file that games and backports read |
| sha3 crate 0.12 and its dependencies (cfg-if, crypto-common, digest, hybrid-array, keccak, sponge-cursor, typenum) | crates.io | MIT, per `licenses/kraken-sha3-NOTICES.txt` | Dependency of `kraken-bridge/` (`Cargo.toml`, `Cargo.lock`) |
| Other Rust crates of the Kraken reader (itoa, libc, memchr, serde_core, serde_json, zmij, parts of the Rust standard library) | crates.io, Rust project | MIT options, per `vendor/kraken/THIRD_PARTY_NOTICES`, which also covers `compiler_builtins`. That file was written for the Kraken Mount ELF, so check it against what `kraken-bridge/Cargo.lock` actually links | `vendor/kraken/THIRD_PARTY_NOTICES`, `kraken-bridge/Cargo.lock` |
| websrv-derived SMB context setup | John Törnblom, <https://github.com/ps5-payload-dev/websrv> (`src/smb.c`) | GPL version 3 or later, per `licenses/SMB-NOTICES.txt` | `src/sm_smb.c`; notice in `licenses/SMB-NOTICES.txt` |
| SMB filesystem adapter | derived from owendswang/ps5-web-file-manager, as a comment in the source states | Not stated in this tree: see upstream | `src/sm_vfs.c` |
| Execute-bit repair for folder games | ShadowMountPlus pull request #226 by PhialsBasement | As ShadowMountPlus (GPL-3.0) | `src/sm_exec_permissions.c`, `tests/test_exec_permissions.c` |

## Libraries linked into the payload

These are built for the PS5 from the ps5-payload-dev package recipes (`pacbrew-repo`, see `.github/workflows/ps5.yml`) or by the Makefile, and linked statically. They are not copied into the tree except where noted.

| Library | Licence | Notes |
| --- | --- | --- |
| json-c 0.19 | MIT-style text in `licenses/json-c-COPYING.txt` | Built by `tools/build_ps5_json_c.py` with `patches/json-c-0.19-release-free.patch` |
| libsmb2 | LGPL 2.1 or later for the library, BSD 2-Clause for its examples (`licenses/libsmb2-COPYING.txt`, `licenses/libsmb2-LGPL-2.1.txt`) | Notice in `licenses/SMB-NOTICES.txt` |
| libjpeg-turbo | See `licenses/libjpeg-turbo-LICENSE.md` and `licenses/libjpeg-turbo-README.ijg` | |
| libpng | See upstream | Linked as `libpng16.a` |
| zlib | See upstream | |
| libmicrohttpd | See upstream | HTTP server for the API |
| SQLite | Public domain, per `vendor/kraken/THIRD_PARTY_NOTICES` | Linked with `-lsqlite3` |
| ps5-payload-sdk | See upstream, <https://github.com/ps5-payload-dev/sdk> | Toolchain and headers; not part of the tree |

## Credited projects that are not in the tree

PS5 Payload Manager by itsPLK (the distribution channel), BackPork by BestPig (the fakelib and backport mechanism that ShadowMountPlus integrates), kstuff-lite by EchoStretch (required on the console), MkPFS by PSBrew (referenced in the image-creation notes).
