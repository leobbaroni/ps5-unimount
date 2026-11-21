#!/usr/bin/env python3
"""Dependency-free mock of the UniMount HTTP/JSON API for web UI development.

Serves web/index.html at / and answers /api/v1/* from synthetic fixtures in
tools/dev/fixtures/default (invented titles, ids and paths). Artwork is
generated on the fly as SVG gradients with simple shapes; no real game art is
used or shipped.

Usage:
    python tools/dev/mock_api.py [--port 10101] [--scenario default|empty|sixty|legacy|offline]
                                 [--fixtures DIR]

Scenarios:
    default  14 synthetic games covering every state, plus the new endpoints
    empty    no games, no sources
    sixty    60 synthetic games
    idle     default library with no storage job running
    readonly idle, but /settings lacks the [compat]/[kraken]/[diagnostics]/[api] keys
    legacy   same library, but /system, /ownership, /diagnostics/*, /games/art
             and game last_launch objects are absent (older engine)
    offline  every API call fails with HTTP 503 (API unreachable)

Switch at runtime with GET /__scenario/<name>.

--fixtures DIR points at captured responses (games.json, settings.json,
storage.json, version.json, packages_root.json, ... and icon_<TITLE>_cover.png).
Any file present there overrides the synthetic answer for that route. Those
captures are private: keep them out of git.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "web" / "index.html"
DEFAULT_FIXTURES = Path(__file__).resolve().parent / "fixtures" / "default"

LOCK = threading.Lock()
STATE: dict = {}
OPTIONS: dict = {"scenario": "default", "fixtures": None}

NEW_ROUTES = {"/system", "/ownership", "/diagnostics/launches", "/diagnostics/errors"}
GET_ONLY = {"/system", "/diagnostics/launches", "/diagnostics/errors"}
NO_ART = {"PPSA90009", "CUSA90006"}  # these show the icon whole, not cropped
CONFIRM_SECONDS = 6.0


# ---------------------------------------------------------------- artwork --

PALETTES = [
    ("#120a08", "#5a1f10", "#f06a2a", "#ffd08a"),
    ("#06121c", "#0f3550", "#3aa0c8", "#d8f2ff"),
    ("#0b1208", "#21401c", "#79b84a", "#eaffc8"),
    ("#140818", "#3d1650", "#b04ad0", "#f6d8ff"),
    ("#1a1206", "#5a3f12", "#e0a83a", "#fff1c8"),
    ("#071414", "#124040", "#36c0a8", "#d0fff4"),
    ("#180808", "#501616", "#d84040", "#ffd0d0"),
    ("#0a0c18", "#1c2450", "#5068d8", "#dde3ff"),
]


def _seed(text: str) -> int:
    return int(hashlib.sha256(text.encode()).hexdigest()[:8], 16)


def art_svg(title_id: str, width: int, height: int) -> str:
    seed = _seed(title_id)
    deep, mid, accent, light = PALETTES[seed % len(PALETTES)]
    motif = (seed >> 4) % 6
    w, h = width, height
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">',
        "<defs>",
        f'<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{deep}"/>'
        f'<stop offset=".62" stop-color="{mid}"/><stop offset="1" stop-color="{accent}"/></linearGradient>',
        f'<radialGradient id="glow" cx=".68" cy=".42" r=".5"><stop offset="0" stop-color="{light}" stop-opacity=".55"/>'
        f'<stop offset="1" stop-color="{light}" stop-opacity="0"/></radialGradient>',
        "</defs>",
        f'<rect width="{w}" height="{h}" fill="url(#sky)"/>',
        f'<rect width="{w}" height="{h}" fill="url(#glow)"/>',
    ]
    rnd = seed
    def r() -> float:
        nonlocal rnd
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        return rnd / 0x7FFFFFFF
    if motif == 0:  # mountains
        for layer, shade in enumerate((mid, deep, "#000")):
            base = h * (0.55 + layer * 0.14)
            pts = [f"0,{h}"]
            x = 0.0
            while x <= w:
                pts.append(f"{x:.0f},{base - r() * h * 0.28:.0f}")
                x += w / (5 + layer * 3)
            pts.append(f"{w},{h}")
            parts.append(f'<polygon points="{" ".join(pts)}" fill="{shade}" opacity="{0.85 - layer * 0.1:.2f}"/>')
    elif motif == 1:  # planet with ring
        cx, cy, rad = w * 0.66, h * 0.44, h * 0.26
        parts.append(f'<circle cx="{cx:.0f}" cy="{cy:.0f}" r="{rad:.0f}" fill="{accent}"/>')
        parts.append(f'<circle cx="{cx - rad * .25:.0f}" cy="{cy - rad * .2:.0f}" r="{rad * .9:.0f}" fill="{light}" opacity=".18"/>')
        parts.append(f'<ellipse cx="{cx:.0f}" cy="{cy:.0f}" rx="{rad * 1.9:.0f}" ry="{rad * .38:.0f}" fill="none" stroke="{light}" stroke-width="{h * .012:.0f}" opacity=".8" transform="rotate(-14 {cx:.0f} {cy:.0f})"/>')
        for _ in range(40):
            parts.append(f'<rect x="{r() * w:.0f}" y="{r() * h * .7:.0f}" width="3" height="3" fill="{light}" opacity="{.3 + r() * .6:.2f}"/>')
    elif motif == 2:  # columns
        for i in range(7):
            x = w * (0.32 + i * 0.1)
            cw = w * 0.035
            top = h * (0.12 + r() * 0.1)
            parts.append(f'<rect x="{x:.0f}" y="{top:.0f}" width="{cw:.0f}" height="{h - top:.0f}" fill="{deep}" opacity=".9"/>')
            parts.append(f'<rect x="{x - cw * .3:.0f}" y="{top:.0f}" width="{cw * 1.6:.0f}" height="{h * .03:.0f}" fill="{deep}"/>')
        parts.append(f'<rect x="0" y="{h * .78:.0f}" width="{w}" height="{h * .22:.0f}" fill="#000" opacity=".6"/>')
    elif motif == 3:  # waves
        for i in range(6):
            y = h * (0.45 + i * 0.1)
            amp = h * 0.04
            d = f"M0 {y:.0f} " + " ".join(
                f"Q {w * (k + .5) / 6:.0f} {y + (amp if k % 2 else -amp):.0f} {w * (k + 1) / 6:.0f} {y:.0f}" for k in range(6))
            parts.append(f'<path d="{d} L{w} {h} L0 {h} Z" fill="{deep}" opacity="{0.25 + i * 0.12:.2f}"/>')
        parts.append(f'<circle cx="{w * .7:.0f}" cy="{h * .3:.0f}" r="{h * .1:.0f}" fill="{light}" opacity=".85"/>')
    elif motif == 4:  # city blocks
        x = w * 0.25
        while x < w:
            bw = w * (0.03 + r() * 0.05)
            bh = h * (0.2 + r() * 0.5)
            parts.append(f'<rect x="{x:.0f}" y="{h - bh:.0f}" width="{bw:.0f}" height="{bh:.0f}" fill="{deep}"/>')
            for _ in range(int(bh / (h * .06))):
                if r() > .55:
                    parts.append(f'<rect x="{x + bw * .3:.0f}" y="{h - bh + r() * bh * .9:.0f}" width="{bw * .2:.0f}" height="{h * .015:.0f}" fill="{light}" opacity=".7"/>')
            x += bw + w * 0.008
    else:  # forest
        for i in range(26):
            x = w * (0.2 + r() * 0.85)
            tw = w * (0.04 + r() * 0.05)
            th = h * (0.3 + r() * 0.45)
            parts.append(f'<polygon points="{x:.0f},{h - th:.0f} {x - tw:.0f},{h} {x + tw:.0f},{h}" fill="{deep}" opacity="{.6 + r() * .4:.2f}"/>')
    parts.append("</svg>")
    return "".join(parts)


# --------------------------------------------------------------- fixtures --

def load_json(name: str):
    override = OPTIONS.get("fixtures")
    if override:
        path = Path(override) / name
        if path.is_file():
            return json.loads(path.read_text(encoding="utf-8"))
    path = DEFAULT_FIXTURES / name
    if path.is_file():
        return json.loads(path.read_text(encoding="utf-8"))
    return None


def full_game(seed: dict) -> dict:
    game = {
        "path": "", "runtime_path": "", "backport_path": "", "source_type": "folder", "image_type": "",
        "platform": "ps5", "title_id": "", "content_id": "", "title_name": "", "version": "",
        "last_access_time": "2026-11-20 21:02:42.473", "install_time": "2026-11-19 22:04:50.703",
        "icon_url": "", "app_db_size_bytes": 102236160, "installed": True, "managed": True, "mounted": False,
        "image_backed": False, "source_available": True, "installed_pkg": False, "can_uninstall": True,
        "kraken_patch_applied": False, "can_manage_source": True, "can_toggle_fakelib": True,
        "ignore_global_fakelib": False, "fakelib_enabled": True, "fakelib_mode": "full",
        "fakelib_default_mode": "full", "fakelib_mode_override": "default", "fakelib_effective_enabled": True,
        "patch_info_status": 0, "has_patch": False, "dlc_info_status": 0, "dlc_count": 0,
    }
    seed = {k: v for k, v in seed.items() if k != "kraken"}
    game.update(seed)
    tid = game["title_id"]
    if not game["content_id"]:
        game["content_id"] = f"UP0000-{tid}_00-SYNTHETIC{tid[-5:]}000"
    if game["source_type"] == "image":
        game["image_backed"] = True
        game["runtime_path"] = f"/mnt/shadowmnt/{tid}_0000{tid[-4:]}"
    if game["platform"] == "ps4":
        game["can_toggle_fakelib"] = False
    game["icon_url"] = f"/api/v1/games/icon?title_id={tid}&size=thumb&v=1-{_seed(tid):08x}"
    if tid not in NO_ART:
        game["art_url"] = f"/api/v1/games/art?title_id={tid}&v=1-{_seed(tid):08x}"
    return game


def synthetic_games(count: int | None = None) -> list[dict]:
    data = load_json("games.json") or {"games": []}
    seeds = data["games"]
    if OPTIONS.get("fixtures") and seeds and "icon_url" in seeds[0]:
        return copy.deepcopy(seeds)
    games = [full_game(s) for s in seeds]
    if count:
        names = ["Amber", "Basalt", "Cobalt", "Driftwood", "Echo", "Fable", "Granite", "Harbor", "Ivory", "Juniper",
                 "Kestrel", "Lantern", "Meridian", "Nimbus", "Obsidian", "Pinion", "Quarry", "Rampart", "Sable", "Thistle"]
        suffix = ["Run", "Tides", "Garden", "Protocol", "Archive", "Signal"]
        out = []
        for i in range(count):
            base = copy.deepcopy(seeds[i % len(seeds)])
            base.pop("last_launch", None)
            if i < len(seeds):
                base = copy.deepcopy(seeds[i])
            else:
                base["title_id"] = f"PPSA9{1000 + i:04d}"
                base["title_name"] = f"{names[i % len(names)]} {suffix[i % len(suffix)]}"
                base["path"] = f"/mnt/usb0/games/{base['title_name']}.exfat"
                base["source_type"] = "image"
                base["image_type"] = "exfatfs"
                base.pop("installed_pkg", None)
                base["mounted"] = i % 3 == 0
                base["source_available"] = True
            out.append(full_game(base))
        games = out
    return games


def initial_state(scenario: str) -> dict:
    count = 60 if scenario == "sixty" else None
    games = [] if scenario == "empty" else synthetic_games(count)
    for game in games:
        if scenario == "legacy":
            game.pop("last_launch", None)
            game.pop("art_url", None)
        else:
            game.setdefault("last_launch", None)
    kraken = {g["title_id"] for g in (load_json("games.json") or {"games": []})["games"] if g.get("kraken")}
    now = time.time()
    job = None
    if scenario in ("default", "sixty", "legacy") and OPTIONS.get("job", True):
        job = {"operation": "install", "source_type": "folder", "title_id": "PPSA90008", "title_name": "Moss and Mortar",
               "source": "/mnt/usb0/incoming/PPSA90008-app", "destination": "/data/homebrew", "started": now,
               "duration": 240.0, "total": 18253611008, "job_id": 7, "cancel": False}
    return {
        "scenario": scenario,
        "games": games,
        "kraken": kraken,
        "manual": [] if scenario == "empty" else ["/mnt/usb0/incoming/PPSA90008-app", "/data/homebrew/PPSA90003-app"],
        "settings": {
            "scan_paths": [] if scenario == "empty" else ["/mnt/usb0/games", "/mnt/usb0/packages"],
            "api_enabled": True, "allow_lan_access": True, "debug": True, "quiet_mode": False,
            "update_emulators": True, "fakelib_default_mode": "full", "auto_update_ampr": False,
            "auto_remove_missing_games": False, "auto_remove_missing_delay_seconds": 300,
            "fan_target_temperature": 0,
            **({} if scenario == "legacy" or OPTIONS.get("readonly") else {
                "compat_kraken_api": True, "compat_kraken_api_port": 8097, "kraken_auto_mount": True,
                "kraken_inject_ampr": True, "diagnostics_enabled": True, "diagnostics_running_confirm_seconds": 20,
                "diagnostics_error_window_seconds": 120, "api_allow_launch": True}),
        },
        "running": "PPSA90001" if scenario != "empty" else None,
        "launches": {},
        "smb": [{"name": "NAS", "path": "smb://nas.home.example/homebrew", "username": "", "active": True,
                 "read_only": False, "password_set": False}] if scenario != "empty" else [],
        "job": job,
        "last_job": None,
        "next_job": 8,
        "queue": [],
        "queue_running": False,
    }


def state() -> dict:
    if not STATE:
        STATE.update(initial_state(OPTIONS["scenario"]))
    return STATE


# ------------------------------------------------------------ job status --

def job_snapshot(job: dict | None) -> dict:
    if not job:
        return {"status": 0, "job_id": 0, "active": False, "state": "idle", "operation": "", "progress_percent": 0}
    elapsed = time.time() - job["started"]
    fraction = min(1.0, elapsed / job["duration"])
    done = fraction >= 1.0 or job.get("cancel")
    total = job["total"]
    processed = int(total * fraction)
    state_name = "cancelled" if job.get("cancel") else ("completed" if done else (
        "measuring" if elapsed < 2 else ("installing" if job["operation"] == "pkg" else "transferring")))
    snap = {
        "status": 0, "job_id": job["job_id"], "active": not done, "state": state_name,
        "operation": job["operation"], "source_type": job.get("source_type", ""),
        "title_id": job.get("title_id", ""), "title_name": job.get("title_name", ""),
        "source": job["source"], "destination": job.get("destination", ""),
        "progress_percent": round(fraction * 100, 1), "processed_bytes": processed, "total_bytes": total,
        "processed_files": int(fraction * 1200), "total_files": 1200,
        "speed_bytes_per_second": int(total / job["duration"]) if not done else 0,
        "elapsed_ms": int(elapsed * 1000), "cancellable": not done, "cancel_requested": bool(job.get("cancel")),
        "result_status": 0, "result_error": "", "result_error_reason": "",
        "queue_count": 0, "queue_completed": 0, "queue_total": 0, "queue_running": False,
    }
    if job["operation"] == "mount_package":
        snap["kraken_phase"] = min(3, int(fraction * 4))
    return snap


def current_job() -> dict:
    s = state()
    snap = job_snapshot(s["job"])
    if s["job"] and not snap["active"]:
        if s["job"]["operation"] == "install" and s["job"].get("title_id"):
            for g in s["games"]:
                if g["title_id"] == s["job"]["title_id"]:
                    g["installed"] = True
        s["last_job"] = snap
        s["job"] = None
    if not s["job"] and s["last_job"]:
        return s["last_job"]
    return snap


def start_job(operation: str, **fields) -> dict:
    s = state()
    if s["job"] and job_snapshot(s["job"])["active"]:
        raise ApiError(409, 16, "a storage operation is already running", "storage_busy")
    job = {"operation": operation, "started": time.time(), "duration": fields.pop("duration", 25.0),
           "total": fields.pop("total", 4294967296), "job_id": s["next_job"], "cancel": False, **fields}
    s["next_job"] += 1
    s["job"] = job
    return job_snapshot(job)


class ApiError(Exception):
    def __init__(self, http: int, status: int, message: str, reason: str = ""):
        super().__init__(message)
        self.http, self.status, self.reason = http, status, reason


# --------------------------------------------------------------- routes --

def find_game(title_id: str) -> dict:
    for game in state()["games"]:
        if game["title_id"] == title_id:
            return game
    raise ApiError(404, 2, "unknown title")


def settle_launches() -> None:
    s = state()
    now = time.time()
    for tid, started in list(s["launches"].items()):
        game = next((g for g in s["games"] if g["title_id"] == tid), None)
        if not game:
            continue
        elapsed = now - started
        ll = game["last_launch"]
        if elapsed >= 2.0:
            s["running"] = tid
            game["mounted"] = True
        if elapsed >= CONFIRM_SECONDS:
            ll["outcome"] = "running"
            del s["launches"][tid]


def list_games(body: dict) -> dict:
    settle_launches()
    s = state()
    games = []
    legacy = s["scenario"] == "legacy"
    for game in s["games"]:
        item = copy.deepcopy(game)
        if legacy:
            item.pop("last_launch", None)
        if not body.get("include_size") and item["source_type"] == "folder":
            item.pop("size_bytes", None)
        elif "size_bytes" in item:
            item["size_status"] = 0
        elif body.get("include_size"):
            item["size_status"] = 0
            item["size_bytes"] = 3221225472 + _seed(item["title_id"]) % 2147483648
        item.pop("playgo_stub_enabled", None)
        games.append(item)
    return {"count": len(games), "size_included": bool(body.get("include_size")), "games": games}


def game_info(body: dict) -> dict:
    game = copy.deepcopy(find_game(body.get("title_id", "")))
    if state()["scenario"] == "legacy":
        game.pop("last_launch", None)
    game.setdefault("size_bytes", 3221225472 + _seed(game["title_id"]) % 2147483648)
    game.update({
        "size_status": 0, "save_data_size_status": 0, "save_data_size_bytes": 67108864,
        "download_size_status": 0, "download_size_bytes": 0,
        "patch_size_status": 0, "patch_size_bytes": 1610612736 if game.get("has_patch") else 0,
        "dlc_size_status": 0, "dlc_size_bytes": 805306368 * game.get("dlc_count", 0),
        "system_content_size_status": 0, "system_content_size_bytes": game.get("size_bytes", 0),
        "patch": {"installed": bool(game.get("has_patch")), "version": "01.010.000" if game.get("has_patch") else "",
                  "size_bytes": 1610612736 if game.get("has_patch") else 0},
        "base_version": "01.000.000" if game.get("has_patch") else "",
        "dlc": [{"content_id": f"UP0000-{game['title_id']}_00-DLCPACK{i:04d}00000", "title_name": f"Expansion {i + 1}",
                 "version": "01.000.000", "size_status": 0, "size_bytes": 805306368} for i in range(game.get("dlc_count", 0))],
        "dlc_info_truncated": False,
    })
    return game


def storage_listing() -> dict:
    mounts = [
        {"source": "ssd0.user", "mount_point": "/user", "filesystem": "bfs", "total_bytes": 670692474880,
         "free_bytes": 622197080064, "available_bytes": 621697080064, "used_bytes": 48995394816, "read_only": False, "ejectable": False},
        {"source": "/dev/da1s1", "mount_point": "/mnt/usb0", "filesystem": "exfatfs", "total_bytes": 2000398934016,
         "free_bytes": 1284195344384, "available_bytes": 1284195344384, "used_bytes": 716203589632, "read_only": False,
         "ejectable": True, "mount_id": "0000002c00000113"},
        {"source": "/dev/da2s1", "mount_point": "/mnt/usb1", "filesystem": "ufs", "total_bytes": 500107862016,
         "free_bytes": 0, "available_bytes": 0, "used_bytes": 500107862016, "read_only": True,
         "ejectable": True, "mount_id": "0000002d00000114"},
        {"source": "/dev/nvme0p1", "mount_point": "/mnt/ext1", "filesystem": "exfatfs", "total_bytes": 1000204886016,
         "free_bytes": 92341796864, "available_bytes": 92341796864, "used_bytes": 907863089152, "read_only": False, "ejectable": False},
    ]
    destinations = [dict(mounts[0], mount_point="/data", source="/user/data", filesystem="nullfs", path="/data/homebrew"),
                    dict(mounts[1], path="/mnt/usb0"), dict(mounts[3], path="/mnt/ext1/homebrew")]
    for conn in state()["smb"]:
        if conn["active"]:
            destinations.append({"path": f"smb://{conn['name']}", "source": conn["path"], "remote": True,
                                 "filesystem": "smb", "read_only": conn["read_only"]})
    return {"count": len(mounts), "destination_count": len(destinations), "mounts": mounts, "destinations": destinations}


def images_listing() -> dict:
    images = []
    for game in state()["games"]:
        if game.get("source_type") == "image":
            images.append({"path": game["path"], "mount_point": game.get("runtime_path", ""),
                           "size": game.get("size_bytes", 0), "mtime_sec": 1795134325, "mtime_nsec": 0,
                           "unit_id": -1, "backend": "LVD", "complete": game["title_id"] != "PPSA90005",
                           "source_available": game.get("source_available", True), "mapped": True,
                           "mounted": game.get("mounted", False)})
    return {"count": len(images), "images": images}


ROOT_STORAGE = [
    {"source": "/user/data", "mount_point": "/data", "filesystem": "nullfs", "total_bytes": 670692474880,
     "free_bytes": 622197080064, "available_bytes": 621697080064, "used_bytes": 48995394816, "read_only": False,
     "ejectable": False, "path": "/data"},
    {"source": "/dev/da1s1", "mount_point": "/mnt/usb0", "filesystem": "exfatfs", "total_bytes": 2000398934016,
     "free_bytes": 1284195344384, "available_bytes": 1284195344384, "used_bytes": 716203589632, "read_only": False,
     "ejectable": True, "mount_id": "0000002c00000113", "path": "/mnt/usb0"},
]

TREE = {
    "/data": [("homebrew", True), ("pkgs", True)],
    "/data/homebrew": [("PPSA90003-app", True), ("backports", True), ("Ember Hollow.exfat", False), ("PPSA90002.ffpkg", False)],
    "/data/homebrew/backports": [("PPSA90013 Kite Weather", True)],
    "/data/pkgs": [("UP0000-PPSA90010_00-ORBITRELAY000000.pkg", False), ("UP0000-PPSA90021_00-HALFLIGHT0000000.pkg", False),
                   ("UP0000-PPSA90021_00-HALFLIGHTPATCH00.pkg", False), ("broken-download.pkg", False)],
    "/mnt/usb0": [("games", True), ("packages", True), ("incoming", True)],
    "/mnt/usb0/games": [("Northbound Signal.exfat", False), ("Deep Field.exfat", False), ("PPSA90003-app", True)],
    "/mnt/usb0/packages": [("UP0000-PPSA90007_00-LUMENDRIFT000000.pkg", False), ("UP0000-PPSA90022_00-WINTERLINE00000.pkg", False)],
    "/mnt/usb0/incoming": [("PPSA90008-app", True)],
}


def package_entry(parent: str, name: str, directory: bool) -> dict:
    path = f"{parent}/{name}"
    entry = {"name": name, "path": path, "directory": directory, "can_delete": not path.startswith("/data/homebrew/PPSA"),
             "installable": False, "size_bytes": 0}
    m = re.search(r"(PPSA\d{5})", name)
    if directory and name.startswith("PPSA") and parent.endswith("backports"):
        entry.update({"backport": True, "backport_installed": True, "title_id": m.group(1), "title_name": "Kite Weather",
                      "version": "01.003.000", "sdk_version": 0x07508000})
    elif directory and m and name.endswith("-app"):
        entry.update({"installable": True, "source_type": "folder", "title_id": m.group(1),
                      "title_name": {"PPSA90003": "Glass Orchard", "PPSA90008": "Moss and Mortar"}.get(m.group(1), name),
                      "version": "01.000.000", "sdk_version": 0x07508000, "has_icon": True, "icon_revision": "r1"})
    elif not directory and name.endswith((".exfat", ".ffpkg", ".pfs")):
        entry.update({"installable": True, "source_type": "image", "size_bytes": 7516192768, "title_id": "", "title_name": name})
    elif not directory and name.endswith(".pkg"):
        if not m:
            entry.update({"size_bytes": 1048576})
        else:
            tid = m.group(1)
            patch = "PATCH" in name
            entry.update({"installable": True, "size_bytes": 12884901888 if not patch else 2147483648, "title_id": tid,
                          "title_name": {"PPSA90010": "Orbit Relay", "PPSA90021": "Half Light", "PPSA90007": "Lumen Drift",
                                         "PPSA90022": "Winterline"}.get(tid, tid),
                          "content_id": name[:-4], "version": "01.000.000" if not patch else "01.010.000",
                          "sdk_version": 0x07508000, "kind": "patch" if patch else "base", "replaceable": False,
                          "installed": False, "base_installed": tid == "PPSA90010", "base_queued": False,
                          "mountable": not patch, "mount_patch": False, "kraken_only": False,
                          "kraken_patch_applied": tid == "PPSA90010", "kraken_relink": False, "kraken_relink_patched": False,
                          "kraken_restore_base": tid == "PPSA90010", "has_icon": True, "icon_revision": "r1"})
    return entry


def listing(body: dict, files_only: bool) -> dict:
    path = (body.get("path") or "").rstrip("/")
    if state()["scenario"] == "empty":
        tree = {"/data": [("homebrew", True)], "/data/homebrew": []}
    else:
        tree = TREE
    roots = ["/data", "/mnt/usb0"] + [f"smb://{c['name']}" for c in state()["smb"] if c["active"]]
    entries = []
    if path:
        if path not in tree and not path.startswith("smb://"):
            raise ApiError(404, 2, "no such directory")
        for name, directory in tree.get(path, []):
            if files_only and not directory and not body.get("include_files"):
                continue
            entries.append(package_entry(path, name, directory) if not files_only else
                           {"name": name, "path": f"{path}/{name}", "directory": directory, "size_bytes": 0, "installable": False})
    parent = path.rsplit("/", 1)[0] if path and path not in roots else ""
    return {"root_storage": ROOT_STORAGE, "path": path, "parent": parent, "can_create_directory": bool(path),
            "truncated": False, "offset": 0, "limit": 256, "roots": roots, "entries": entries}


def launches(body: dict) -> dict:
    tid = body.get("title_id")
    records = []
    for game in state()["games"]:
        ll = game.get("last_launch")
        if not ll or (tid and game["title_id"] != tid):
            continue
        records.append(launch_record(game, ll))
    records.sort(key=lambda r: r["id"], reverse=True)
    limit = int(body.get("limit") or 20)
    return {"enabled": True, "running_confirm_seconds": 20, "error_window_seconds": 120, "launches": records[:limit]}


def launch_record(game: dict, ll: dict) -> dict:
    outcome = ll["outcome"]
    ok = {"state": "ok", "at": ll["at"], "detail": None}
    none = {"state": "not_observed", "at": None, "detail": None}
    failed = {"state": "failed", "at": ll["at"], "detail": None}
    pending = {"state": "pending", "at": None, "detail": None}
    stages = {k: dict(none) for k in ("mounted", "registered", "launch_requested", "sandbox_ready",
                                       "process_started", "running_confirmed", "exited")}
    order = list(stages)
    base_ms = int(time.mktime(time.strptime(ll["at"], "%Y-%m-%d %H:%M:%S")) * 1000) if len(ll["at"]) == 19 else 0
    offsets = {"mounted": 0, "registered": 40, "launch_requested": 60, "sandbox_ready": 900, "process_started": 1300,
               "running_confirmed": 21300, "exited": 5500}
    reach = {"running": 6, "exited_normally": 7, "crashed": 5, "spawn_failed": 3, "mount_failed": 0,
             "launch_in_progress": 3, "exited_early": 5}.get(outcome, 0)
    for i, key in enumerate(order[:reach]):
        stages[key] = dict(ok)
    for key in order:
        if stages[key]["state"] in ("ok", "failed") and base_ms:
            stages[key]["at_unix_ms"] = base_ms + offsets[key]
    if outcome == "mount_failed":
        stages["mounted"] = dict(failed, detail="nmount exfatfs: Input/output error", at_unix_ms=base_ms)
    elif outcome == "spawn_failed":
        stages["process_started"] = dict(failed, detail="spawn returned 0x80AA001A")
    elif outcome == "crashed":
        stages["running_confirmed"] = dict(failed, detail="process exited before confirmation", at_unix_ms=base_ms + 5500)
        stages["exited"] = dict(ok, at_unix_ms=base_ms + 5500)
    elif outcome == "launch_in_progress":
        stages["process_started"] = dict(pending)
    native = []
    diagnosis = None
    if ll.get("short_error"):
        code = {"CE-105779-9": -2137784263, "CE-107750-0": -2136342502}.get(ll["short_error"], -2137784263)
        native.append({"file": "error_0001.json", "file_mtime": ll["at"], "parsed": True, "short_error": ll["short_error"],
                       "internal_error_code": code, "internal_error_code_hex": f"0x{code & 0xffffffff:08X}",
                       "message": "", "title_id": game["title_id"], "display_version": game.get("version", ""),
                       "tick": None, "app_id": None, "category": ll.get("category"), "attribution": "title_id"})
    if ll.get("category"):
        advice = {
            "corrupt_data": "The game data failed an integrity check. Copy the image again from a known-good source, then mount it again.",
            "permission": "The system refused to start the executable. Check that kstuff is loaded and that the game's fakelib mode matches what it needs.",
            "mount_failed": "The image could not be mounted. Check that the drive is connected and that the file is complete, then rescan.",
        }.get(ll["category"], "")
        evidence = {
            "corrupt_data": ["Native error CE-105779-9 (0x80940039) attributed by title ID",
                             "Process started and exited after 4.2 s without a running confirmation"],
            "permission": ["Native error CE-107750-0 (0x80AA001A) attributed by title ID", "kernel: [SceShellCore] spawn denied"],
            "mount_failed": ["nmount exfatfs returned EIO on layer image", "Image marked incomplete in the image snapshot"],
        }.get(ll["category"], [])
        diagnosis = {"category": ll["category"], "confidence": "observed", "rule": f"rule_{ll['category']}",
                     "evidence": evidence, "advice": advice}
    process = {"pid": 4412, "app_id": "0x60000012", "alive_seconds": 4.2} if outcome == "crashed" else None
    return {"id": ll["id"], "title_id": game["title_id"], "managed": True, "at": ll["at"], "at_unix_ms": base_ms,
            "process": process, "outcome": outcome,
            "observation_interrupted": False, "short_error": ll.get("short_error"), "launch_attempts": 1,
            "stages": stages, "mount": None, "environment": {"kstuff_loaded": True,
            "shellcore_hooks_installed": True, "console_firmware": "13.60", "required_firmware": "7.00"},
            "native_errors": native, "diagnosis": diagnosis,
            "timeline": [{"at": ll["at"], "event": "launch_requested", "detail": game["title_id"]}]}


def handle_api(route: str, body: dict):
    s = state()
    if s["scenario"] == "offline":
        raise ApiError(503, 5, "service unavailable")
    legacy = s["scenario"] == "legacy"
    if legacy and (route in NEW_ROUTES or route.startswith("/diagnostics/")):
        raise ApiError(404, 2, "unknown API route")
    override = OPTIONS.get("fixtures")
    if override:
        name = {"/version": "version.json", "/games": "games.json", "/settings": "settings.json",
                "/storage": "storage.json", "/images": "images.json", "/manual/list": "manual_list.json",
                "/smb": "smb_list.json", "/debug-log": "debug_log.json", "/kernel-log": "kernel_log.json"}.get(route)
        if route == "/games/info":
            name = f"game_info_{body.get('title_id', '')}.json"
        if route == "/packages":
            name = "packages_root.json" if not body.get("path") else None
        if name and (Path(override) / name).is_file():
            return json.loads((Path(override) / name).read_text(encoding="utf-8"))
    if route == "/version":
        caps = ["web_ui", "storage_space", "storage_eject", "list_images", "list_games", "installed_pkg_games",
                "game_fakelib_settings", "game_info", "game_data_sizes", "pkg_install", "pkg_mount", "pkg_browser_stream",
                "smb2_smb3", "game_icon", "mount_game", "unmount_game", "uninstall_game", "uninstall_game_content",
                "install_game_source", "move_game_source", "copy_game_source", "delete_game_source", "unpack_game_image",
                "storage_job_status", "storage_job_cancel", "list_manual_sources", "add_manual_source",
                "remove_manual_source", "manage_settings", "read_debug_log", "read_kernel_log", "rescan"]
        if legacy:
            return {"api_version": 1, "shadowmount_version": "1.7", "capabilities": caps}
        return {"api_version": 1, "shadowmount_version": "1.7", "product": "UniMount", "unimount_version": "0.1.0",
                "kraken_mount_version": "0.3.1", "kraken_mount_revision": "e1e7845",
                "capabilities": caps + ["ownership", "game_art", "launch_game", "close_game"]}
    if route == "/games":
        return list_games(body)
    if route == "/games/info":
        return game_info(body)
    if route == "/images":
        return images_listing()
    if route == "/storage":
        if body.get("path", "").startswith("smb://"):
            return {"path": body["path"], "filesystem": "smb", "remote": True, "total_bytes": 4000787030016,
                    "free_bytes": 1500000000000, "available_bytes": 1500000000000, "used_bytes": 2500787030016,
                    "read_only": False}
        return storage_listing()
    if route == "/storage/eject":
        return start_job("eject", source=body.get("path", ""), duration=4.0, total=0)
    if route == "/settings":
        return dict(s["settings"], scan_path_count=len(s["settings"]["scan_paths"]))
    if route == "/settings/update":
        for key, value in body.items():
            if key in s["settings"] or key in ("scan_paths",):
                s["settings"][key] = value
        return {"saved": True}
    if route == "/scan":
        return {"scan_queued": True, "scan_deferred": False, "scan_deferred_reason": ""}
    if route == "/manual/list":
        return {"count": len(s["manual"]), "paths": s["manual"]}
    if route == "/manual/add":
        changed = body.get("path") not in s["manual"]
        if changed:
            s["manual"].append(body.get("path"))
        return {"changed": changed}
    if route == "/manual/remove":
        changed = body.get("path") in s["manual"]
        s["manual"] = [p for p in s["manual"] if p != body.get("path")]
        return {"changed": changed}
    if route == "/smb":
        action = body.get("action", "list")
        if action == "check":
            return {"dialect": 0x311, "connections": s["smb"]}
        if action == "save":
            s["smb"] = [c for c in s["smb"] if c["name"] not in (body.get("previous_name"), body.get("name"))]
            s["smb"].append({"name": body.get("name"), "path": body.get("path"), "username": body.get("username", ""),
                             "active": bool(body.get("active", True)), "read_only": bool(body.get("read_only")),
                             "password_set": bool(body.get("password"))})
        if action == "remove":
            s["smb"] = [c for c in s["smb"] if c["name"] != body.get("name")]
        return {"dialect": 0, "connections": s["smb"]}
    if route in ("/games/mount", "/games/unmount"):
        game = find_game(body.get("title_id", ""))
        if game.get("installed_pkg"):
            raise ApiError(403, 1, "installed PKG titles cannot be mounted")
        if game["title_id"] == "PPSA90001" and route == "/games/unmount":
            raise ApiError(409, 16, "A game is active or awaiting exit; close it fully and try again", "game_active")
        game["mounted"] = route == "/games/mount"
        return {"title_id": game["title_id"], "mode": body.get("mode", "ro")}
    if route == "/games/uninstall":
        game = find_game(body.get("title_id", ""))
        game["installed"] = False
        return {"scan_queued": False, "scan_deferred": False, "scan_deferred_reason": ""}
    if route == "/games/content/uninstall":
        game = find_game(body.get("title_id", ""))
        if body.get("kind") == "patch":
            game["has_patch"] = False
        else:
            game["dlc_count"] = max(0, game.get("dlc_count", 0) - 1)
        return {}
    if route == "/games/fakelib":
        game = find_game(body.get("title_id", ""))
        if "mode" in body:
            game["fakelib_mode_override"] = body["mode"]
            game["fakelib_mode"] = game["fakelib_default_mode"] if body["mode"] == "default" else body["mode"]
            game["fakelib_enabled"] = game["fakelib_mode"] != "disabled"
        if "ignore_global_fakelib" in body:
            game["ignore_global_fakelib"] = bool(body["ignore_global_fakelib"])
        if "playgo_stub_enabled" in body:
            game["playgo_stub_enabled"] = bool(body["playgo_stub_enabled"])
        return {k: game.get(k) for k in ("fakelib_mode", "fakelib_mode_override", "fakelib_default_mode", "fakelib_enabled",
                                          "fakelib_effective_enabled", "ignore_global_fakelib", "playgo_stub_enabled")
                if k in game} | {"saved": True, "applies_on_next_launch": True}
    if route in ("/games/copy", "/games/move", "/games/unpack", "/games/delete"):
        game = find_game(body.get("title_id", ""))
        op = route.rsplit("/", 1)[1]
        if op == "delete" and body.get("confirm") is not True:
            raise ApiError(400, 22, "confirm=true is required")
        if not game.get("source_available", True):
            return {"source_missing": True, "scan_queued": True, "scan_deferred": False, "scan_deferred_reason": ""}
        return start_job(op, title_id=game["title_id"], title_name=game["title_name"], source=game["path"],
                         destination=body.get("destination_dir", ""), source_type=game["source_type"],
                         total=game.get("size_bytes", 4294967296))
    if route == "/games/running":
        settle_launches()
        tid = s["running"]
        return {"running": bool(tid), "app_id": 1610612754 if tid else -1, "app_id_hex": "0x60000012" if tid else "0xFFFFFFFF",
                "title_id": tid, "pid": 4412 if tid else None}
    if route == "/games/launch":
        if not s["settings"].get("api_allow_launch", True):
            raise ApiError(403, 1, "launching is disabled", "launch_disabled")
        game = find_game(body.get("title_id", ""))
        if s["running"] and s["running"] != game["title_id"] and body.get("replace") is not True:
            raise ApiError(409, 16, "another application is running", "app_running")
        replaced = bool(s["running"] and s["running"] != game["title_id"])
        if replaced:
            for other in s["games"]:
                if other["title_id"] == s["running"] and other.get("last_launch"):
                    other["last_launch"]["outcome"] = "exited_normally"
        s["running"] = None
        game["last_launch"] = {"id": 100 + len(s["launches"]) + int(time.time()) % 1000, "outcome": "launch_in_progress",
                               "at": time.strftime("%Y-%m-%d %H:%M:%S"), "short_error": None, "category": None}
        s["launches"][game["title_id"]] = time.time()
        return {"title_id": game["title_id"], "app_id": 1610612754, "app_id_hex": "0x60000012", "replaced": replaced}
    if route == "/games/close":
        tid = s["running"]
        if not tid:
            raise ApiError(409, 16, "nothing is running", "no_app_running")
        if body.get("title_id") and body["title_id"] != tid:
            raise ApiError(409, 16, "a different title is running", "title_mismatch")
        s["running"] = None
        for game in s["games"]:
            if game["title_id"] == tid and game.get("last_launch"):
                game["last_launch"]["outcome"] = "exited_normally"
        return {"closed": True, "app_id": 1610612754, "app_id_hex": "0x60000012", "title_id": tid}
    if route == "/games/storage/status":
        return current_job()
    if route == "/games/storage/cancel":
        if s["job"]:
            s["job"]["cancel"] = True
        return current_job()
    if route == "/packages":
        return listing(body, False)
    if route == "/files":
        return listing(body, True)
    if route == "/files/mkdir":
        parent = body.get("path", "")
        TREE.setdefault(parent, []).append((body.get("name", "New folder"), True))
        TREE.setdefault(f"{parent}/{body.get('name')}", [])
        return {"path": f"{parent}/{body.get('name')}"}
    if route == "/packages/queue":
        action = body.get("action", "status")
        if action == "add":
            s["queue"].append(package_entry(*body["path"].rsplit("/", 1), False))
        elif action == "remove":
            s["queue"] = [e for e in s["queue"] if e["path"] != body.get("path")]
        elif action == "clear":
            s["queue"] = []
        elif action == "start":
            if not s["queue"]:
                raise ApiError(400, 22, "queue is empty")
            first = s["queue"].pop(0)
            return start_job("install", source_type="pkg", title_id=first.get("title_id"), title_name=first.get("title_name"),
                             source=first["path"], total=first.get("size_bytes", 1))
        return {"queue_running": False, "queue_completed": 0, "queue_total": len(s["queue"]), "entries": s["queue"]}
    if route == "/packages/install":
        entry = package_entry(*body["path"].rsplit("/", 1), False)
        return start_job("install", source_type="pkg", title_id=entry.get("title_id"), title_name=entry.get("title_name"),
                         source=body["path"], total=entry.get("size_bytes", 1))
    if route == "/packages/mount":
        entry = package_entry(*body["path"].rsplit("/", 1), False)
        return start_job("mount_package", source_type="kraken", title_id=entry.get("title_id"),
                         title_name=entry.get("title_name"), source=body["path"], total=entry.get("size_bytes", 1))
    if route == "/packages/stream":
        raise ApiError(501, 78, "browser streaming is not simulated by the mock server")
    if route == "/sources/install":
        return start_job("install", source_type="folder", source=body.get("path", ""), destination=body.get("destination_dir", ""))
    if route == "/sources/delete":
        return start_job("delete", source=body.get("path", ""), duration=6.0)
    if route == "/backports/install":
        return start_job("copy", source_type="backport", source=body.get("path", ""),
                         destination="/data/homebrew/backports")
    if route == "/debug-log":
        text = "\n".join(f"[20:{i // 60:02d}:{i % 60:02d}]   [SCAN] synthetic debug line {i}: checked /mnt/usb0/games" for i in range(220))
        return {"content": text, "file_size": len(text) * 3, "returned_bytes": len(text), "truncated": True}
    if route == "/kernel-log":
        text = "\n".join(f"[SceShellCore] synthetic kernel line {i}: heap free 38%" for i in range(160))
        return {"content": text, "total_bytes": 131072, "returned_bytes": len(text), "truncated": True}
    if route == "/system":
        return {
            "firmware": {"display": "13.60", "raw": 0x13600000, "raw_hex": "0x13600000"},
            "shellcore": {"offsets_available": True, "offsets_firmware": "13.60", "install_bridge_firmware": True,
                          "hooks": {"installed": True, "state": "installed", "hook_count": 6, "install_hook": True},
                          "registration": {"path": "shellcore_bridge", "available": True, "reason": ""}},
            "kstuff": {"loaded": True, "implementation": "kstuff-lite", "version": "1.2", "version_source": "pldmgr_meta",
                       "inferred": True, "evidence": ["process kstuff-lite.elf", "autoload.txt entry kstuff-lite.elf"]},
            "mechanisms": [
                {"name": "lvd", "available": True, "reason": ""}, {"name": "md", "available": True, "reason": ""},
                {"name": "nullfs", "available": True, "reason": ""}, {"name": "unionfs", "available": True, "reason": ""},
                {"name": "pfs_images", "available": False, "reason": "PFS keys not available on this firmware"},
                {"name": "kraken_pkg", "available": True, "reason": "", "bridge_revision": "e1e7845",
                 "ampr_runtime": "bundled", "ampr_runtime_path": "/data/shadowmount/ampr", "ampr_runtime_version": "0.5.0"},
                {"name": "per_title_registration", "available": True, "reason": "", "path": "shellcore_bridge"}],
            "limitations": [{"code": "native_debug_pkg_unavailable", "severity": "info",
                             "message": "Debug .pkg files mount through Kraken; the system installer cannot install them."}]}
    if route == "/ownership":
        return {"product": "UniMount", "version": "0.1.0", "pid": 812, "started": 1795200000,
                "ownership_ready": True, "previous_unclean": False, "previous_pid": 0,
                "kraken_takeover": {"state": "taken_over", "version": "0.3.1"},
                "legacy_mounts": {"released": 2, "left": 0},
                "compat_listener": {"enabled": True, "port": 8097, "bound": True},
                "auto_mount": {"enabled": True, "queued": 1,
                               "building": "/mnt/usb0/packages/UP0000-PPSA90007_00-LUMENDRIFT000000.pkg",
                               "failed": [{"path": "/mnt/usb0/packages/UP0000-PPSA90022_00-WINTERLINE00000.pkg",
                                           "error": "kraken_base_invalid"}]}}
    if route == "/diagnostics/launches":
        return launches(body)
    if route.startswith("/diagnostics/launches/"):
        wanted = route.rsplit("/", 1)[1]
        for record in launches({"limit": 64})["launches"]:
            if str(record["id"]) == wanted:
                return {"launch": record}
        raise ApiError(404, 2, "no such launch")
    if route == "/diagnostics/errors":
        return {"directory": "/system_data/priv/error/history", "readable": True, "read_error": None, "total_files": 3, "errors": [
            {"file": "error_0003.json", "file_mtime": "2026-11-21 19:02:51", "parsed": True, "short_error": "CE-105779-9",
             "internal_error_code": -2137784263, "internal_error_code_hex": "0x80940039", "message": "",
             "title_id": "PPSA90004", "display_version": "01.002.000", "category": "corrupt_data"},
            {"file": "error_0002.json", "file_mtime": "2026-11-21 17:11:36", "parsed": True, "short_error": "CE-107750-0",
             "internal_error_code": -2136342502, "internal_error_code_hex": "0x80AA001A", "message": "",
             "title_id": "PPSA90012", "display_version": "01.000.000", "category": "permission"},
            {"file": "error_0001.json", "file_mtime": "2026-11-18 09:40:02", "parsed": False}]}
    raise ApiError(404, 2, "unknown API route")


# ---------------------------------------------------------------- server --

class Handler(BaseHTTPRequestHandler):
    server_version = "UniMountMock/1"

    def log_message(self, fmt, *args):  # quieter console
        if "/api/v1/games/storage/status" not in (args[0] if args else ""):
            super().log_message(fmt, *args)

    def _send(self, code: int, body: bytes, ctype: str, extra: dict | None = None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, payload: dict):
        local = int(time.time()) + 3600
        self._send(code, json.dumps(payload).encode(), "application/json",
                   {"X-ShadowMount-Local-Time": str(local), "Access-Control-Allow-Origin": "*"})

    def do_GET(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        if url.path in ("/", "/index.html"):
            return self._send(200, WEB.read_bytes(), "text/html; charset=utf-8")
        if url.path.startswith("/__scenario/"):
            name = url.path.rsplit("/", 1)[1]
            with LOCK:
                OPTIONS["job"] = name not in ("idle", "readonly")
                OPTIONS["readonly"] = name == "readonly"
                OPTIONS["scenario"] = "default" if name in ("idle", "readonly") else name
                STATE.clear()
            return self._send(200, f"scenario={name}\n".encode(), "text/plain")
        if url.path == "/favicon.png":
            return self._send(404, b"", "text/plain")
        if state()["scenario"] == "offline" and url.path.startswith("/api/"):
            return self._send(503, b"", "text/plain")
        title = (query.get("title_id") or [""])[0]
        if url.path == "/api/v1/games/icon":
            override = OPTIONS.get("fixtures")
            if override:
                png = Path(override) / f"icon_{title}_cover.png"
                if png.is_file():
                    return self._send(200, png.read_bytes(), "image/png")
            return self._send(200, art_svg(title, 512, 512).encode(), "image/svg+xml")
        if url.path == "/api/v1/games/art":
            if state()["scenario"] == "legacy" or OPTIONS.get("fixtures"):
                return self._json(404, {"status": 2, "error": "unknown API route"})
            if title in NO_ART:
                return self._json(404, {"status": 2, "error": "no key art", "error_reason": "art_unavailable"})
            size = (query.get("size") or ["tv"])[0]
            w, h = (1920, 1080) if size == "tv" else (640, 360)
            return self._send(200, art_svg(title, w, h).encode(), "image/svg+xml")
        get_routes = ("/api/v1/system", "/api/v1/ownership", "/api/v1/diagnostics/errors", "/api/v1/diagnostics/launches")
        if url.path in get_routes or url.path.startswith("/api/v1/diagnostics/launches/"):
            body = {key: values[0] for key, values in query.items()}
            try:
                with LOCK:
                    result = handle_api(url.path[len("/api/v1"):], body)
                return self._json(200, {"status": 0, **result})
            except ApiError as exc:
                return self._json(exc.http, {"status": exc.status, "error": str(exc)})
        if url.path == "/api/v1/packages/icon":
            path = (query.get("path") or [""])[0]
            return self._send(200, art_svg(path, 256, 256).encode(), "image/svg+xml")
        return self._json(404, {"status": 2, "error": "unknown API route"})

    def do_POST(self):
        url = urlparse(self.path)
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        if not url.path.startswith("/api/v1/"):
            return self._json(404, {"status": 2, "error": "unknown API route"})
        try:
            body = json.loads(raw or b"{}")
            if not isinstance(body, dict):
                raise ValueError
        except ValueError:
            return self._json(400, {"status": 22, "error": "invalid JSON"})
        route = url.path[len("/api/v1"):]
        if route in GET_ONLY or route.startswith("/diagnostics/launches/"):
            return self._json(405, {"status": 22, "error": "use GET"})
        try:
            with LOCK:
                result = handle_api(route, body)
            time.sleep(0.05)
            return self._json(200, {"status": 0, **result})
        except ApiError as exc:
            payload = {"status": exc.status, "error": str(exc)}
            if exc.reason:
                payload["error_reason"] = exc.reason
            return self._json(exc.http, payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=10101)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--scenario", default="default", choices=["default", "idle", "empty", "sixty", "legacy", "offline"])
    parser.add_argument("--fixtures", help="directory of captured API responses that override the synthetic ones")
    args = parser.parse_args()
    OPTIONS["job"] = args.scenario != "idle"
    OPTIONS["scenario"] = "default" if args.scenario == "idle" else args.scenario
    OPTIONS["fixtures"] = args.fixtures
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"UniMount mock API on http://{args.host}:{args.port}/ (scenario {args.scenario})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
