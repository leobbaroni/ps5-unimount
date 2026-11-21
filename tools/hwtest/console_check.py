#!/usr/bin/env python3
"""Scripted hardware checks against a console running UniMount.

Test-only; run it from a PC on the same LAN. Standard library only.

    python tools/hwtest/console_check.py --console <host> status
    python tools/hwtest/console_check.py --console <host> deploy build/unimount.elf
    python tools/hwtest/console_check.py --console <host> launch <TITLEID>
    python tools/hwtest/console_check.py --console <host> helper browse http://<pc>:8766/

status  prints the running version, ownership state, the running game, the
        size of the console's error history and each title's last launch.
deploy  uploads an ELF into a PS5 Payload Manager slot and loads it, then
        waits for the new instance to answer.
launch  starts a title through UniMount, waits for the launch record to be
        confirmed running, holds, closes the game and waits for the record to
        end. It passes only when the record went running -> exited_normally
        and the console wrote nothing new to its error history. A mounted
        game with an icon is not a pass.
helper  sends one line to the unimount-hwtest helper payload (tools/hwtest).
fetch   saves a file from /data/shadowmount/ on the console (debug.log,
        debug.log.1, ...) through the helper, without starting UniMount.

Nobody watches the TV during these runs: picture and sound are not checked.
Neither API has authentication. Trusted LAN only.
"""
import argparse
import json
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class Console:
    def __init__(self, host, api_port, pldmgr_port, helper_port):
        self.api_base = "http://%s:%d/api/v1" % (host, api_port)
        self.pldmgr_base = "http://%s:%d" % (host, pldmgr_port)
        self.helper_address = (host, helper_port)

    def api(self, route, body=None, timeout=20):
        """GET when body is None, otherwise POST the JSON body. Returns (status, object)."""
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.api_base + route, data=data,
                                         headers={"Content-Type": "application/json"} if data is not None else {})
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.status, json.loads(response.read().decode("utf-8", "replace") or "{}")
        except urllib.error.HTTPError as error:
            text = error.read().decode("utf-8", "replace")
            try:
                return error.code, json.loads(text)
            except ValueError:
                return error.code, {"error": text.strip()}

    def pldmgr(self, route, body=b"", timeout=120):
        request = urllib.request.Request(self.pldmgr_base + route, data=body, method="POST", headers={
            "X-PLDMGR-Request": "1", "Content-Type": "application/octet-stream"})
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read().decode("utf-8", "replace").strip()

    def helper_bytes(self, line):
        with socket.create_connection(self.helper_address, timeout=8) as connection:
            connection.sendall((line + "\n").encode())
            connection.settimeout(20)
            chunks = []
            try:
                while True:
                    chunk = connection.recv(65536)
                    if not chunk:
                        break
                    chunks.append(chunk)
            except socket.timeout:
                pass
        return b"".join(chunks)

    def helper(self, line):
        return self.helper_bytes(line).decode("utf-8", "replace").strip()

    def version(self):
        return self.api("/version", {})[1]

    def running(self):
        return self.api("/games/running", {})[1]

    def error_files(self):
        return self.api("/diagnostics/errors")[1].get("total_files")

    def last_record(self, title_id):
        query = "?" + urllib.parse.urlencode({"title_id": title_id, "limit": 1})
        launches = self.api("/diagnostics/launches" + query)[1].get("launches") or []
        return launches[0] if launches else None


def describe_version(info):
    return "UniMount %s (%s)" % (info.get("unimount_version", "?"), info.get("shadowmount_version", "?"))


def cmd_status(console, _args):
    print(describe_version(console.version()))
    ownership = console.api("/ownership")[1]
    print("previous instance left uncleanly: %s" % ownership.get("previous_unclean"))
    running = console.running()
    print("running game: %s" % (running.get("title_id") or ("app 0x%X" % running["app_id"] if running.get("running") else "none")))
    print("error history files: %s" % console.error_files())
    for game in console.api("/games", {})[1].get("games", []):
        last = game.get("last_launch") or {}
        print("  %s  %-8s  last launch: %s" % (game.get("title_id"), game.get("source_type") or game.get("type") or "",
                                                last.get("outcome") or "none recorded"))
    return 0


def cmd_deploy(console, args):
    with open(args.elf, "rb") as handle:
        payload = handle.read()
    filename = args.filename or args.elf.replace("\\", "/").rsplit("/", 1)[-1]
    query = urllib.parse.urlencode({"filename": filename, "mode": "replace"})
    print("upload %s (%d bytes): %s" % (filename, len(payload), console.pldmgr("/manage:upload?" + query, payload)))
    print("load %s: %s" % (args.slot, console.pldmgr("/loadpayload:" + args.slot)))
    deadline = time.time() + 90
    time.sleep(5)  # the old instance is still answering while it hands over
    while time.time() < deadline:
        try:
            print(describe_version(console.version()))
            print("previous instance left uncleanly: %s" % console.api("/ownership")[1].get("previous_unclean"))
            return 0
        except (OSError, ValueError):
            time.sleep(3)
    print("FAIL: the API did not answer within 90 s")
    return 1


def wait_for(console, title_id, newer_than, accept, seconds):
    """Poll the title's newest record until accept(outcome) or the time is up."""
    deadline = time.time() + seconds
    record = None
    while time.time() < deadline:
        record = console.last_record(title_id)
        if record and record.get("id", 0) > newer_than and accept(record.get("outcome")):
            return record
        time.sleep(3)
    return record if record and record.get("id", 0) > newer_than else None


def cmd_launch(console, args):
    title_id = args.title_id
    if console.running().get("running"):
        print("FAIL: a game is already running; close it first")
        return 1
    errors_before = console.error_files()
    previous = console.last_record(title_id)
    previous_id = previous.get("id", 0) if previous else 0
    confirm = console.api("/diagnostics/launches?limit=1")[1].get("running_confirm_seconds") or 20

    status, reply = console.api("/games/launch", {"title_id": title_id}, timeout=90)
    if status != 200 or reply.get("status") not in (0, None):
        print("FAIL: launch request refused (HTTP %d): %s" % (status, json.dumps(reply)))
        return 1
    print("launch requested; waiting up to %d s for the record to be confirmed" % (confirm + 25))
    record = wait_for(console, title_id, previous_id, lambda outcome: outcome != "launch_in_progress", confirm + 25)
    first = record.get("outcome") if record else None
    print("record %s: %s" % (record.get("id") if record else "-", first or "no new launch record"))

    still_running = False
    if first == "running":
        time.sleep(args.hold)
        still_running = bool(console.running().get("running"))
        print("after %d s more: %s" % (args.hold, "still running" if still_running else "no longer running"))

    final = first
    if console.running().get("running"):
        print("close: %s" % json.dumps(console.api("/games/close", {}, timeout=40)[1]))
        ended = wait_for(console, title_id, previous_id,
                         lambda outcome: outcome not in ("launch_in_progress", "running"), 45)
        final = ended.get("outcome") if ended else None
        print("record after close: %s" % final)

    errors_after = console.error_files()
    print("error history files: %s before, %s after" % (errors_before, errors_after))
    passed = first == "running" and still_running and final == "exited_normally" and errors_before == errors_after
    print("PASS" if passed else "FAIL")
    return 0 if passed else 1


def cmd_helper(console, args):
    print(console.helper(" ".join(args.line)))
    return 0


def cmd_fetch(console, args):
    data = console.helper_bytes("read " + args.remote)
    if data.startswith(b"err "):
        print("FAIL: " + data.decode("utf-8", "replace").strip())
        return 1
    with open(args.local, "wb") as handle:
        handle.write(data)
    print("%s: %d bytes -> %s" % (args.remote, len(data), args.local))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--console", required=True, help="console host name or address")
    parser.add_argument("--api-port", type=int, default=10101)
    parser.add_argument("--pldmgr-port", type=int, default=8084)
    parser.add_argument("--helper-port", type=int, default=10190)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status").set_defaults(run=cmd_status)
    deploy = commands.add_parser("deploy")
    deploy.add_argument("elf")
    deploy.add_argument("--slot", default="local/unimount", help="PS5 Payload Manager slot to load (default local/unimount)")
    deploy.add_argument("--filename", help="file name inside the slot (default: the ELF's own name)")
    deploy.set_defaults(run=cmd_deploy)
    launch = commands.add_parser("launch")
    launch.add_argument("title_id")
    launch.add_argument("--hold", type=int, default=20, help="seconds to keep the game running after confirmation")
    launch.set_defaults(run=cmd_launch)
    helper = commands.add_parser("helper")
    helper.add_argument("line", nargs="+")
    helper.set_defaults(run=cmd_helper)
    fetch = commands.add_parser("fetch")
    fetch.add_argument("remote", help="a file under /data/shadowmount/ on the console")
    fetch.add_argument("local", help="where to save it on this PC")
    fetch.set_defaults(run=cmd_fetch)
    args = parser.parse_args()
    console = Console(args.console, args.api_port, args.pldmgr_port, args.helper_port)
    try:
        return args.run(console, args)
    except urllib.error.HTTPError as error:
        print("FAIL: HTTP %d from %s" % (error.code, error.url))
        return 1
    except OSError as error:
        print("FAIL: console not reachable (%s). In rest mode it answers ping but no port." % error)
        return 2


if __name__ == "__main__":
    sys.exit(main())
