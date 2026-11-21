#!/usr/bin/env python3
"""Generate the PS5 Payload Manager (PLDMGR) custom-repository files.

Writes payloads.json and icons.json in the repository root, following
https://github.com/itsPLK/ps5-payload-manager/blob/main/CUSTOM_REPOSITORIES.md

    python3 tools/make_pldmgr_manifest.py --elf build/unimount.elf \
        --base-url https://github.com/OWNER/REPO/releases/download/v0.1.0

The ELF's SHA-256 goes into the manifest, so PLDMGR verifies every download.
PLDMGR compares `version` first, then `last_update`, then `checksum`.
"""
import argparse
import base64
import datetime
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# PLDMGR limits: name/id/author/category <= 127 bytes, description <= 1023,
# url <= 1023, filename <= 255 and ending in .elf or .bin, icon value <= 12288 chars.
ICON_KEY = "unimount"
ICON_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">'
    '<rect width="256" height="256" rx="28" fill="#0b0d10"/>'
    '<rect x="44" y="48" width="168" height="96" rx="6" fill="none" stroke="#fff" stroke-width="12"/>'
    '<rect x="44" y="168" width="44" height="40" rx="4" fill="none" stroke="#fff" stroke-width="10"/>'
    '<rect x="106" y="168" width="44" height="40" rx="4" fill="#fff"/>'
    '<rect x="168" y="168" width="44" height="40" rx="4" fill="none" stroke="#fff" stroke-width="10"/>'
    "</svg>"
)

DESCRIPTION = (
    "One mount payload for PS5 games: extracted folders, exFAT/UFS/PFS images "
    "(.exfat, .ffpkg, .ffpfs, .ffpfsc) and plaintext debug .pkg files mounted "
    "directly, without converting them. Registers titles on the home screen, "
    "reports launch results with the console's own error codes, and replaces "
    "running ShadowMountPlus and Kraken Mount separately. Independent fork; "
    "not an official release of either project. Needs kstuff-lite 1.07 or newer."
)


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elf", required=True, type=pathlib.Path, help="built payload to describe")
    parser.add_argument("--base-url", required=True, help="URL prefix the payload file is downloaded from")
    parser.add_argument("--version", help="release version without the leading v (default: VERSION file)")
    parser.add_argument("--date", help="release date YYYY-MM-DD (default: today, UTC)")
    parser.add_argument("--source", default="", help="project page shown in PLDMGR")
    parser.add_argument("--out-dir", type=pathlib.Path, default=ROOT, help="where payloads.json and icons.json go")
    args = parser.parse_args()

    if not args.base_url.startswith(("http://", "https://")):
        parser.error("--base-url must be http:// or https://")
    version = (args.version or (ROOT / "VERSION").read_text(encoding="utf-8")).strip().lstrip("v")
    date = args.date or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    filename = f"unimount_v{version}.elf"
    if not args.elf.is_file() or args.elf.stat().st_size == 0:
        parser.error(f"{args.elf} is missing or empty")

    payload = {
        "id": "unimount",
        "name": "UniMount",
        "filename": filename,
        "url": f"{args.base_url.rstrip('/')}/{filename}",
        "description": DESCRIPTION,
        "version": f"v{version}",
        "last_update": date,
        "category": "Utilities & Tools",
        "checksum": sha256_of(args.elf),
        "author": "UniMount contributors (based on ShadowMountPlus by drakmor and Kraken Mount by quer3q)",
        "icon_key": ICON_KEY,
    }
    if args.source:
        payload["source"] = args.source
    for key, limit in (("name", 127), ("id", 127), ("author", 127), ("category", 127),
                       ("description", 1023), ("url", 1023), ("filename", 255)):
        if len(payload[key].encode("utf-8")) > limit:
            parser.error(f"{key} exceeds PLDMGR's {limit}-byte limit")

    icon = "data:image/svg+xml;base64," + base64.b64encode(ICON_SVG.encode("utf-8")).decode("ascii")
    if len(icon) > 12288:
        parser.error("icon exceeds PLDMGR's 12288-character limit")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    manifest = {"name": "UniMount", "icons_url": "icons.json", "payloads": [payload]}
    (args.out_dir / "payloads.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    (args.out_dir / "icons.json").write_text(json.dumps({"icons": {ICON_KEY: icon}}, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{filename}  sha256={payload['checksum']}  version={payload['version']}  date={date}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
