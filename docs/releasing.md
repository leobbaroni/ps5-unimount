# Releasing

A release is a git tag `v<version>`, a GitHub release carrying the ELF as `unimount_v<version>.elf`, and a refreshed PS5 Payload Manager manifest (`payloads.json`, `icons.json`) committed to the repository root. `payloads.json` and `icons.json` are not release assets: PLDMGR reads them from the repository, and the manifest points at the release asset.

1. Bump the version in the root `VERSION` file (for example `0.1.1`). Commit it.
2. Build: `tools/wsl-build.sh`. It produces `build/unimount.elf`.
3. Run the tests: `tools/wsl-build.sh test` (cargo tests and every `tools/test_*.py`). CI runs the host tests on every push as well.
4. Generate the manifest from the built ELF, with `<version>` replaced by the contents of `VERSION`:

   ```sh
   python tools/make_pldmgr_manifest.py --elf build/unimount.elf \
     --base-url https://github.com/leobbaroni/ps5-unimount/releases/download/v<version> \
     --source https://github.com/leobbaroni/ps5-unimount
   ```

   The ELF's SHA-256 goes into `payloads.json`, so PLDMGR verifies every download against it.
5. Commit `payloads.json` and `icons.json`.
6. Tag `v<version>` (the tag must be `v` plus the contents of `VERSION`) and push the branch and the tag. The tag starts `.github/workflows/ps5.yml`, which builds in CI and creates a GitHub release with `UniMount_<version tag>.zip` and `unimount_v<version>.elf` attached.
7. Make the release asset match the manifest. The CI build is a separate build and may not be byte-identical to your local one, so its checksum can differ from the one in `payloads.json`. Upload the ELF you hashed, replacing the CI asset of the same name:

   ```sh
   gh release upload v<version> build/unimount.elf#unimount_v<version>.elf --clobber
   ```

   If the release does not exist yet, create it instead: `gh release create v<version> build/unimount.elf#unimount_v<version>.elf --title "UniMount v<version>"`.
8. Check: the `url` in `payloads.json` downloads, its SHA-256 equals `checksum`, and PS5 Payload Manager (with "Multiple sources" enabled) lists the new version.

The source URL users add in PLDMGR is `https://raw.githubusercontent.com/leobbaroni/ps5-unimount/main/payloads.json`, so the manifest commit has to be on `main`. Until the first release has been cut, that file does not exist in the repository.
