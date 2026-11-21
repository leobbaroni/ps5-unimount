#!/usr/bin/env python3
"""Measure the interface's layout in the console's own browser, unattended.

Test-only. Serves the interface through this PC with a small reporting script
added to <head>, and forwards every other request to the console. Open the
printed URL in the PS5 browser (for example with the hwtest helper's
"browse" command). The page then presses a synthetic D-pad key so the legend
shows, selects each game on the shelf in turn and reports its own geometry
back here. The probe prints one line per game and exits 0 when every game
fits the viewport, 1 when one does not, 2 on timeout.

    python tools/hwtest/layout_probe.py --console http://<console>:10101

The script must live in <head>: the PS5 browser stops parsing the page when
the interface sets up its L1/R1 history entries, so anything placed after the
page's own script never runs there.

No authentication, like the API it forwards to. Trusted LAN only.
"""
import argparse
import json
import os
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Plain ES5 on purpose; it has to run on the console's WebKit.
PROBE_JS = r"""
(function () {
  var tick = 0, index = -1, waited = 0, errors = [];
  window.addEventListener('error', function (e) {
    errors.push(String(e.message).slice(0, 80) + '@' + e.lineno);
  });
  function rect(el) {
    if (!el) return null;
    var b = el.getBoundingClientRect();
    return [Math.round(b.left), Math.round(b.top), Math.round(b.width), Math.round(b.height)];
  }
  function send(data) {
    try {
      var x = new XMLHttpRequest();
      x.open('GET', '/__report?d=' + encodeURIComponent(JSON.stringify(data)) + '&t=' + Date.now(), true);
      x.send();
    } catch (e) {}
  }
  function key(name, code) {
    var ev = document.createEvent('Event');
    ev.initEvent('keydown', true, true);
    ev.key = name; ev.keyCode = code; ev.which = code;
    (document.activeElement || document.body).dispatchEvent(ev);
  }
  function measure(i, count) {
    var title = document.getElementById('heroTitle');
    var view = document.getElementById('view-library');
    var legend = document.getElementById('legend');
    var size = title ? parseFloat(getComputedStyle(title).fontSize) : 0;
    return {
      game: i, games: count,
      viewport: [window.innerWidth, window.innerHeight],
      doc_height: document.documentElement.scrollHeight,
      scroll_y: Math.round(window.pageYOffset),
      topbar: rect(document.querySelector('.topbar')),
      title: rect(title), title_chars: title ? title.textContent.trim().length : 0, title_px: size,
      title_clipped: title ? title.scrollHeight > title.clientHeight + size * 0.5 : false,
      tracker: rect(document.getElementById('tracker')),
      actions: rect(document.getElementById('heroActions')),
      shelf: rect(document.getElementById('shelf')),
      legend: rect(legend), legend_hints: legend ? legend.children.length : 0,
      view_scroll: view ? [view.scrollHeight, view.clientHeight] : null,
      view_class: view ? view.className : '',
      errors: errors
    };
  }
  function step() {
    var shelf = document.getElementById('shelf');
    var cards = shelf ? shelf.children : [];
    if (!cards.length) {
      if (++waited > 20) { send({ done: true, problem: 'no games listed after 30 s' }); return; }
      setTimeout(step, 1500);
      return;
    }
    if (tick === 0) {
      key('ArrowRight', 39);
    } else if (tick % 2 === 1) {
      index++;
      if (index >= cards.length || index >= 24) { send({ done: true }); return; }
      (cards[index].querySelector('button') || cards[index]).click();
    } else {
      send(measure(index, cards.length));
    }
    tick++;
    setTimeout(step, 1500);
  }
  setTimeout(step, 1500);
})();
"""

results = []
finished = threading.Event()
problem = []


def judge(report):
    """Return the list of ways one report fails to fit the viewport."""
    faults = []
    height = report["viewport"][1]
    if report["doc_height"] > height:
        faults.append("page %d px taller than the viewport" % (report["doc_height"] - height))
    if report["scroll_y"]:
        faults.append("page scrolled by %d px" % report["scroll_y"])
    if not report["topbar"] or report["topbar"][1] != 0:
        faults.append("top bar not at the top")
    scroll = report["view_scroll"]
    if scroll and scroll[0] > scroll[1] + 1:
        faults.append("library overflows its shell by %d px" % (scroll[0] - scroll[1]))
    if report["title_clipped"]:
        faults.append("title clipped")
    if report["errors"]:
        faults.append("script errors: " + "; ".join(report["errors"]))
    return faults


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    console = ""

    def log_message(self, *_):
        pass

    def report(self):
        query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        try:
            data = json.loads(query.get("d", ["{}"])[0])
        except ValueError:
            data = {}
        self.send_response(204)
        self.end_headers()
        if data.get("done"):
            if data.get("problem"):
                problem.append(data["problem"])
            finished.set()
        elif "viewport" in data:
            faults = judge(data)
            results.append(faults)
            print("game %d/%d  viewport %dx%d  page %d  title %d chars at %g px  tracker %s  shelf %s  legend %d hints  [%s]  %s" % (
                data["game"] + 1, data["games"], data["viewport"][0], data["viewport"][1], data["doc_height"],
                data["title_chars"], data["title_px"], data["tracker"], data["shelf"], data["legend_hints"],
                data["view_class"], "FITS" if not faults else "FAIL: " + ", ".join(faults)), flush=True)

    def forward(self, body=None):
        if self.path.startswith("/__report"):
            return self.report()
        request = urllib.request.Request(self.console + self.path, data=body, method=self.command)
        for name in ("Content-Type", "Accept"):
            if self.headers.get(name):
                request.add_header(name, self.headers[name])
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                data, status, headers = response.read(), response.status, response.getheaders()
        except urllib.error.HTTPError as error:
            data, status, headers = error.read(), error.code, list(error.headers.items())
        except OSError as error:
            self.send_response(502)
            self.end_headers()
            self.wfile.write(str(error).encode())
            return
        if self.path.split("?")[0] in ("/", "/index.html") and b"<head>" in data:
            data = data.replace(b"<head>", b"<head><script>" + PROBE_JS.encode() + b"</script>", 1)
        self.send_response(status)
        for name, value in headers:
            if name.lower() not in ("content-length", "transfer-encoding", "connection", "content-encoding"):
                self.send_header(name, value)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.forward()

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        self.forward(self.rfile.read(length) if length else b"")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--console", required=True, help="base URL of the interface, e.g. http://<console>:10101")
    parser.add_argument("--port", type=int, default=8766, help="port to listen on (default 8766)")
    parser.add_argument("--timeout", type=int, default=180, help="seconds to wait for the browser (default 180)")
    args = parser.parse_args()
    Handler.console = args.console.rstrip("/")
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print("Open http://<this PC>:%d/ in the console's browser. Waiting up to %d s." % (args.port, args.timeout), flush=True)
    if not finished.wait(args.timeout):
        print("TIMEOUT: the browser did not finish (%d game(s) reported)" % len(results), flush=True)
        os._exit(2)
    if problem:
        print("FAIL: " + problem[0], flush=True)
        os._exit(1)
    failed = sum(1 for faults in results if faults)
    print("%s: %d game(s) measured, %d did not fit" % ("FAIL" if failed or not results else "PASS", len(results), failed), flush=True)
    os._exit(1 if failed or not results else 0)


if __name__ == "__main__":
    sys.exit(main())
