# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Checks the web UI in headless Chrome over the DevTools protocol.

Usage: test_ui.py SERVE_EXECUTABLE [--screenshot FILE]
Exits with 77 (skipped) when Chrome is not available.
With PM_JS_COVERAGE=FILE, writes which lines of the page code ran to FILE.
"""

import base64
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

PIPELINE = ("videotestsrc is-live=true ! jpegenc ! decodebin3 ! queue ! "
            "tee name=t ! queue ! videoconvert ! fakesink")
# Static branches for the README screenshot ("t" would be toggled).
SHOT_PIPELINE = ("videotestsrc is-live=true ! jpegenc ! decodebin3 ! queue ! "
                 "tee name=split ! queue ! videoconvert ! fakesink "
                 "split. ! queue ! fakesink "
                 "audiotestsrc is-live=true ! opusenc ! decodebin3 ! "
                 "audioconvert ! queue max-size-time=500000000 ! "
                 "fakesink sync=true ts-offset=300000000")
TRANSFORM = "document.querySelector('#graph > g').getAttribute('transform')"
AUTO_FIT = "document.getElementById('auto-fit').getAttribute('aria-pressed')"
NODES = "document.querySelectorAll('#graph .node').length"


def find_chrome():
    candidates = [os.environ.get("CHROME"),
                  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                  "/Applications/Chromium.app/Contents/MacOS/Chromium"]
    candidates += [shutil.which(n) for n in
                   ("google-chrome", "google-chrome-stable", "chromium",
                    "chromium-browser", "chrome")]
    # Windows: Chrome is usually not on PATH.
    for base in ("PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"):
        if os.environ.get(base):
            candidates.append(os.path.join(os.environ[base], "Google",
                                           "Chrome", "Application",
                                           "chrome.exe"))
    return next((c for c in candidates if c and os.path.exists(c)), None)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class WebSocket:
    def __init__(self, url):
        hostport, path = url[len("ws://"):].split("/", 1)
        host, port = hostport.split(":")
        self.sock = socket.create_connection((host, int(port)))
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((
            f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        ).encode())
        self.file = self.sock.makefile("rb")
        assert b" 101 " in self.file.readline()
        while self.file.readline() not in (b"\r\n", b""):
            pass

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            head = bytes([0x81, 0x80 | n])
        elif n < 65536:
            head = bytes([0x81, 0x80 | 126]) + struct.pack(">H", n)
        else:
            head = bytes([0x81, 0x80 | 127]) + struct.pack(">Q", n)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(head + mask + body)

    def recv(self):
        b0, b1 = self.file.read(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self.file.read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self.file.read(8))[0]
        return self.file.read(n).decode()

    def close(self):
        self.sock.close()


class Browser:
    def __init__(self, chrome, profile):
        # CI machines sometimes start Chrome very slowly: one more try.
        page = self.start(chrome, profile, 1) or self.start(chrome, profile, 2)
        if page is None:
            with open(self.log.name, errors="replace") as f:
                output = f.read()[-3000:]
            raise AssertionError(f"Chrome did not start ({self.state}):\n"
                                 f"{output}")
        self.attach(page)
        self.call("Runtime.enable")
        self.call("Page.enable")
        # The last "properties" messages, shown when a check about them
        # fails.
        self.frames = []
        # The last clicks, and in each page what the details panel showed
        # and when: printed when a click finds nothing.
        self.clicks = []
        self.call("Page.addScriptToEvaluateOnNewDocument", source=DETAILS_LOG)
        self.call("Network.enable")
        self.coverage_file = os.environ.get("PM_JS_COVERAGE")
        self.coverage = {}
        if self.coverage_file:
            self.call("Profiler.enable")
            self.call("Profiler.startPreciseCoverage", detailed=True)

    # Starts Chrome; returns its page target, or None after stopping it.
    def start(self, chrome, profile, attempt):
        port = free_port()
        args = [chrome, "--headless=new", f"--remote-debugging-port={port}",
                f"--user-data-dir={profile}", "--hide-scrollbars",
                "--no-first-run", "--window-size=1400,800"]
        # CI containers and Ubuntu 24.04 runners often block Chrome's sandbox.
        if os.environ.get("CI"):
            args.append("--no-sandbox")
        # Kept so a failed start can say why.
        self.log = open(os.path.join(profile, f"chrome-{attempt}.log"), "wb")
        self.proc = subprocess.Popen(
            args + ["about:blank"],
            stdout=subprocess.DEVNULL, stderr=self.log)
        page = None
        end = time.time() + 30
        while page is None and time.time() < end and self.proc.poll() is None:
            try:
                targets = json.load(urllib.request.urlopen(
                    f"http://127.0.0.1:{port}/json/list"))
                page = next(t for t in targets if t["type"] == "page")
            except Exception:
                time.sleep(0.1)
        if page is None:
            code = self.proc.poll()
            self.proc.kill()
            self.proc.wait()
            self.log.close()
            self.state = (f"exited with code {code}" if code is not None
                          else "no answer within 30 s")
        self.port = port
        return page

    def attach(self, page):
        self.target = page["id"]
        self.ws = WebSocket(page["webSocketDebuggerUrl"])
        self.next_id = 0
        self.errors = []
        self.frames = []
        self.clicks = []
        self.coverage_file = None

    # A tab this page opened, whose address ends with end, to control like
    # this one; close it with close_tab().
    def other_tab(self, end, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            targets = json.load(urllib.request.urlopen(
                f"http://127.0.0.1:{self.port}/json/list"))
            found = [t for t in targets if t["type"] == "page" and
                     t["id"] != self.target and t["url"].endswith(end)]
            if found:
                tab = Browser.__new__(Browser)
                tab.port = self.port
                tab.attach(found[0])
                tab.call("Runtime.enable")
                tab.call("Page.enable")
                return tab
            time.sleep(0.1)
        raise AssertionError(f"no tab opened at ...{end}")

    def close_tab(self):
        urllib.request.urlopen(
            f"http://127.0.0.1:{self.port}/json/close/{self.target}").read()
        self.ws.close()

    def call(self, method, **params):
        # Coverage is per document: kept before the page is replaced.
        if method in ("Page.navigate", "Page.reload") and self.coverage_file:
            self.take_coverage()
        self.next_id += 1
        self.ws.send(json.dumps({"id": self.next_id, "method": method,
                                 "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("method") == "Runtime.exceptionThrown":
                details = msg["params"]["exceptionDetails"]
                self.errors.append(details.get("exception", {})
                                   .get("description", details.get("text")))
            elif msg.get("method") == "Network.webSocketFrameReceived":
                data = msg["params"]["response"]["payloadData"]
                if data.startswith('{"type":"properties"'):
                    self.frames = self.frames[-11:] + [data]
            elif (msg.get("method") == "Runtime.consoleAPICalled"
                  and msg["params"]["type"] in ("error", "warning")):
                self.errors.append(" ".join(
                    str(a.get("value", a.get("description")))
                    for a in msg["params"]["args"]))
            if msg.get("id") == self.next_id:
                if "error" in msg:
                    raise RuntimeError(msg["error"])
                return msg["result"]

    def eval(self, expression):
        result = self.call("Runtime.evaluate", expression=expression,
                           returnByValue=True, awaitPromise=True)
        if "exceptionDetails" in result:
            raise RuntimeError(result["exceptionDetails"])
        return result["result"].get("value")

    # Errors count as "not yet": right after a reload the page may not have
    # its elements.
    def wait(self, expression, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.eval(expression):
                    return True
            except RuntimeError:
                pass
            time.sleep(0.1)
        return False

    def click(self, selector):
        point = self.eval(
            f"(() => {{ const n = document.querySelector({json.dumps(selector)});"
            " if (!n) return null; const r = n.getBoundingClientRect();"
            " return [r.x + r.width / 2, r.y + r.height / 2]; })()")
        assert point, f"nothing matches {selector}\n{self.history()}"
        self.press(selector, point)

    # Clicks at point, noting what was there.
    def press(self, what, point):
        under = self.eval(
            f"(n => n ? n.tagName + (n.id ? '#' + n.id : '') + "
            f"(n.getAttribute('class') ? '.' + n.getAttribute('class') : '') + "
            f"(n.closest('[data-id]') ? ' in ' + "
            f"n.closest('[data-id]').dataset.id : '') : 'nothing')"
            f"(document.elementFromPoint({point[0]}, {point[1]}))")
        self.clicks = self.clicks[-14:] + [
            f"{time.time():.2f} {what} at {point[0]:.0f},{point[1]:.0f}: "
            f"{under}"]
        for kind in ("mousePressed", "mouseReleased"):
            self.call("Input.dispatchMouseEvent", type=kind, x=point[0],
                      y=point[1], button="left", clickCount=1)

    def history(self):
        try:
            details = self.eval("JSON.stringify(window.detailsLog || [])")
            sockets = self.eval("JSON.stringify(window.socketLog || [])")
            state = self.eval(
                "JSON.stringify({hash: location.hash, connection: "
                "document.getElementById('connection')?.textContent, "
                "details: !document.getElementById('details')?.hidden})")
        except RuntimeError as e:
            details, state, sockets = "[]", f"cannot read: {e}", "[]"
        return ("last clicks:\n  " + "\n  ".join(self.clicks) +
                f"\npage: {state}\ndetails shown (time, element):\n  " +
                "\n  ".join(" ".join(map(str, d))
                             for d in json.loads(details or "[]")) +
                "\nWebSocket (time, event):\n  " +
                "\n  ".join(" ".join(map(str, d))
                             for d in json.loads(sockets or "[]")))

    # The live graph can move between finding an element and clicking it.
    def click_until(self, selector, condition, tries=3):
        for _ in range(tries):
            self.click(selector)
            if self.wait(condition, timeout=3):
                return True
        return False

    # Clicks the element an expression returns, at once (it may not last).
    def click_point(self, expression):
        assert self.try_click_point(expression), \
            f"nothing to click: {expression}\n{self.history()}"

    # As click_point; False if there is nothing to click.
    def try_click_point(self, expression):
        point = self.eval(
            f"(() => {{ const n = {expression}; if (!n) return null;"
            " const r = n.getBoundingClientRect();"
            " return [r.x + r.width / 2, r.y + r.height / 2]; })()")
        if point:
            self.press(expression[:60], point)
        return bool(point)

    # Drags the element a selector matches by (dx, dy) in small steps.
    def drag(self, selector, dx, dy):
        point = self.eval(
            f"(() => {{ const n = document.querySelector({json.dumps(selector)});"
            " if (!n) return null; const r = n.getBoundingClientRect();"
            " return [r.x + 20, r.y + r.height / 2]; })()")
        assert point, f"nothing matches {selector}"
        x, y = point
        self.call("Input.dispatchMouseEvent", type="mousePressed", x=x, y=y,
                  button="left", clickCount=1)
        for i in range(1, 11):
            self.call("Input.dispatchMouseEvent", type="mouseMoved",
                      x=x + dx * i / 10, y=y + dy * i / 10, button="left",
                      buttons=1)
        self.call("Input.dispatchMouseEvent", type="mouseReleased",
                  x=x + dx, y=y + dy, button="left", clickCount=1)

    # Moves the mouse to the middle of the element an expression returns.
    def hover(self, expression):
        point = self.eval(
            f"(() => {{ const n = {expression}; if (!n) return null;"
            " const r = n.getBoundingClientRect();"
            " return [r.x + r.width / 2, r.y + r.height / 2]; })()")
        assert point, f"nothing to hover: {expression}"
        self.call("Input.dispatchMouseEvent", type="mouseMoved", x=point[0],
                  y=point[1])

    # Chooses a file for an <input type="file">, as a person would.
    def set_file(self, selector, path):
        root = self.call("DOM.getDocument")["root"]["nodeId"]
        node = self.call("DOM.querySelector", nodeId=root,
                         selector=selector)["nodeId"]
        self.call("DOM.setFileInputFiles", files=[path], nodeId=node)

    def screenshot(self, path):
        data = self.call("Page.captureScreenshot")["data"]
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))

    # Counts per character of each script: nested ranges come after the
    # ones they are in, and override them.
    def take_coverage(self):
        result = self.call("Profiler.takePreciseCoverage")["result"]
        for script in result:
            name = urllib.parse.urlparse(script["url"]).path.lstrip("/")
            if not name.endswith(".js") or name.startswith("vendor/"):
                continue
            ranges = sorted((r for f in script["functions"]
                             for r in f["ranges"]),
                            key=lambda r: (r["startOffset"], -r["endOffset"]))
            counts = {}
            for r in ranges:
                for i in range(r["startOffset"], r["endOffset"]):
                    counts[i] = r["count"]
            ran = self.coverage.setdefault(name, set())
            ran.update(i for i, c in counts.items() if c > 0)

    def write_coverage(self, web_dir):
        lines_out = []
        for name in sorted(self.coverage):
            with open(os.path.join(web_dir, name), encoding="utf-8") as f:
                source = f.read()
            ran = self.coverage[name]
            missed, total, offset = [], 0, 0
            for number, line in enumerate(source.split("\n"), 1):
                code = line.strip()
                start = offset + len(line) - len(line.lstrip())
                offset += len(line) + 1
                if not code or code.startswith(("//", "*", "/*")) or \
                        code in ("}", "});", "};", ")", ");", "]", "];"):
                    continue
                total += 1
                if not any(i in ran for i in range(start, start + len(code))):
                    missed.append(number)
            share = 100 * (total - len(missed)) / total if total else 100
            spans = []
            for n in missed:
                if spans and spans[-1][1] == n - 1:
                    spans[-1][1] = n
                else:
                    spans.append([n, n])
            lines_out.append(f"{name:14} {total - len(missed):5}/{total:<5} "
                             f"{share:5.1f}%  " + ",".join(
                                 str(a) if a == b else f"{a}-{b}"
                                 for a, b in spans))
        with open(self.coverage_file, "w") as f:
            f.write("\n".join(lines_out) + "\n")

    def close(self):
        if self.coverage_file:
            self.take_coverage()
            self.write_coverage(os.path.join(
                os.path.dirname(os.path.abspath(__file__)), "..", "web"))
        self.ws.close()
        self.proc.terminate()
        self.proc.wait()
        self.log.close()


# In each page: the element the details panel shows (null: none), each time
# it changes, with the time.
DETAILS_LOG = """
window.detailsLog = [];
window.socketLog = [];
(() => {
  const Real = window.WebSocket;
  const note = (what) => {
    window.socketLog = window.socketLog.slice(-19)
      .concat([[(Date.now() / 1000).toFixed(2), what]]);
  };
  window.WebSocket = function (url, protocols) {
    const ws = new Real(url, protocols);
    note("connect " + url);
    ws.addEventListener("open", () => note("open"));
    ws.addEventListener("close", (e) => note(`close ${e.code} ${e.reason}`));
    return ws;
  };
  window.WebSocket.prototype = Real.prototype;
  Object.assign(window.WebSocket, { CONNECTING: 0, OPEN: 1, CLOSING: 2,
    CLOSED: 3 });
})();
document.addEventListener("DOMContentLoaded", () => {
  const details = document.getElementById("details");
  if (!details)
    return;
  let last;
  new MutationObserver(() => {
    const now = details.hidden ? null
      : details.querySelector("h2")?.textContent ?? null;
    if (now !== last) {
      last = now;
      window.detailsLog = window.detailsLog.slice(-19)
        .concat([[(Date.now() / 1000).toFixed(2), now]]);
    }
  }).observe(details, { childList: true, subtree: true, attributes: true });
});
"""


# Recordings of the test runs go here, not to the user's folder.
RECORDINGS = tempfile.mkdtemp(prefix="pm-ui-rec-")


# wait: the program starts only once a page is connected; extra: more
# tracer options.
def start_server(serve, pipeline, *args, debug=None, port=0, wait=False,
                 extra=""):
    # In GST_TRACERS a backslash escapes: Windows paths use "/".
    folder = RECORDINGS.replace("\\", "/")
    options = f'port={port},recordings="{folder}"' + \
        (",wait=true" if wait else "") + (f",{extra}" if extra else "")
    env = dict(os.environ, GST_TRACERS=f"pipeline-monitor({options})")
    if debug:
        env["GST_DEBUG"] = debug
    proc = subprocess.Popen([serve, *args, pipeline], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            text=True)
    found = {}
    # The last lines it printed, shown when a check fails.
    proc.lines = []

    def read():
        for line in proc.stderr:
            proc.lines = proc.lines[-199:] + [line.rstrip()]
            m = re.match(r"pipeline-monitor: (http://\S+)", line)
            if m and "url" not in found:
                found["url"] = m.group(1)

    threading.Thread(target=read, daemon=True).start()
    # The first run after a build can spend a while scanning plugins.
    end = time.time() + 60
    while "url" not in found and time.time() < end:
        time.sleep(0.05)
    assert "url" in found, "server did not report its address"
    return proc, found["url"]


def element_selector(name):
    return ("[...document.querySelectorAll('#graph .node')]"
            ".find(n => n.querySelector(':scope > .name')?.textContent === "
            f"{json.dumps(name)})")


def check(browser, url):
    browser.call("Page.navigate", url=url)
    if not browser.wait(f"{NODES} > 0"):
        status = browser.eval("document.getElementById('connection')"
                              "?.textContent")
        raise AssertionError(f"graph was not drawn: connection {status!r}, "
                             f"errors {browser.errors}")
    assert browser.eval("document.getElementById('connection').textContent") \
        == "connected"
    assert browser.eval("document.getElementById('empty').hidden")

    # The About box under the name: maker and links, closed by Esc and by
    # a click elsewhere.
    about = "document.getElementById('about')"
    browser.click("#brand")
    assert browser.wait(f"!{about}.hidden"), "About box did not open"
    assert browser.eval(f"{about}.querySelector('a[href=\"https://"
                        "imaginarybit.com\"]') !== null")
    browser.call("Input.dispatchKeyEvent", type="keyDown", key="Escape")
    assert browser.wait(f"{about}.hidden"), "Esc did not close About"
    browser.click("#brand")
    assert browser.wait(f"!{about}.hidden")
    browser.click("#graph")
    assert browser.wait(f"{about}.hidden"), "a click elsewhere did not close it"

    mq = browser.eval(f"{element_selector('multiqueue0')}?.dataset.id")
    assert mq, "multiqueue0 not drawn"
    assert browser.click_until(
        f'#graph .node[data-id="{mq}"] > rect.box',
        "document.querySelector('#details h2')?.textContent === 'multiqueue0'"), \
        "details panel did not open"

    assert browser.eval(AUTO_FIT) == "false", "auto-fit still on after a click"
    view = browser.eval(TRANSFORM)
    count = browser.eval(NODES)
    assert browser.wait(f"{NODES} !== {count}"), "graph did not update live"
    assert browser.eval(TRANSFORM) == view, "view moved after the user clicked"

    assert browser.click_until(
        '.toggle[data-path$="parsebin0"] rect',
        "!!document.querySelector('#graph .node.collapsed')"), \
        "bin did not collapse"
    assert not browser.eval(f"!!{element_selector('typefind')}"), \
        "children of a collapsed bin are still drawn"

    browser.click("#auto-fit")
    assert browser.wait(f"{AUTO_FIT} === 'true'"), "auto-fit did not turn on"
    # A branch coming and going no longer changes the zoom (keepPlaces),
    # so check that the fit follows the window.
    view = browser.eval(TRANSFORM)
    browser.call("Emulation.setDeviceMetricsOverride", width=1000, height=700,
                 deviceScaleFactor=1, mobile=False)
    assert browser.wait(f"{TRANSFORM} !== {json.dumps(view)}"), \
        "auto-fit did not follow the window"
    browser.call("Emulation.clearDeviceMetricsOverride")
    # The wheel zooms where the pointer is, and stops auto-fit.
    view = browser.eval(TRANSFORM)
    browser.call("Input.dispatchMouseEvent", type="mouseWheel", x=400, y=300,
                 deltaX=0, deltaY=-240)
    assert browser.wait(f"{TRANSFORM} !== {json.dumps(view)} && "
                        f"{AUTO_FIT} === 'false'"), "the wheel did not zoom"

    browser.call("Page.reload")
    assert browser.wait("!!document.querySelector('#graph .node.collapsed')"), \
        "collapsed bin not remembered after reload"
    assert browser.eval(f"{AUTO_FIT} === 'true'"), \
        "auto-fit not back on after zooming and a reload"
    assert browser.click_until(
        '.toggle[data-path$="parsebin0"] rect',
        "!document.querySelector('#graph .node.collapsed')"), \
        "bin did not expand"

    # Auto-fit turned off with its button stays off after a reload (the
    # graph is fitted once).
    if browser.eval(f"{AUTO_FIT} === 'false'"):
        browser.click("#auto-fit")
    assert browser.wait(f"{AUTO_FIT} === 'true'")
    browser.click("#auto-fit")
    assert browser.wait(f"{AUTO_FIT} === 'false'")
    browser.call("Page.reload")
    assert browser.wait(f"!!{element_selector('queue0')}", timeout=10)
    time.sleep(1)
    assert browser.eval(f"{AUTO_FIT} === 'false'"), \
        "auto-fit off not remembered after reload"
    assert browser.eval(
        "(g => { const r = g.getBoundingClientRect(), s = document."
        "getElementById('graph').getBoundingClientRect(); return r.width > 0 "
        "&& r.left >= s.left - 1 && r.right <= s.right + 1 && "
        "r.top >= s.top - 1 && r.bottom <= s.bottom + 1; })"
        "(document.querySelector('#graph > g'))"), \
        "the graph was not fitted after reload"
    browser.click("#auto-fit")
    assert browser.wait(f"{AUTO_FIT} === 'true'")
    browser.call("Page.reload")
    assert browser.wait(f"!!{element_selector('queue0')}", timeout=10)
    assert browser.wait(f"{AUTO_FIT} === 'true'")

    counts = set()
    end = time.time() + 5
    while time.time() < end:
        counts.add(browser.eval(NODES))
        time.sleep(0.2)
    assert len(counts) > 1, f"graph did not update live: {counts}"

    check_log(browser)
    check_caps(browser)
    check_units(browser)
    check_places(browser)
    check_levels(browser)
    check_properties(browser)
    check_element_pins(browser)
    check_latency(browser)
    # The test pipeline is healthy.
    assert browser.wait("!document.querySelector('#problems-counter .count')"
                        " && document.getElementById('problems-counter')"
                        ".textContent === 'Problems'"), \
        "problems found in a healthy pipeline"
    check_pins(browser)
    check_paths(browser)

    browser.call("Emulation.setDeviceMetricsOverride", width=390, height=760,
                 deviceScaleFactor=1, mobile=True)
    time.sleep(0.3)
    assert not browser.eval("document.documentElement.scrollWidth > innerWidth")
    browser.call("Emulation.clearDeviceMetricsOverride")

    assert not browser.errors, f"errors in the page: {browser.errors}"


PANELS = {"log": "log", "problems": "findings", "recordings": "recordings"}


# Opens a bottom panel, also if it is open already (the page keeps the open
# one across reloads).
def open_panel(browser, name):
    panel = f"document.getElementById('{PANELS[name]}-panel')"
    if browser.eval(f"{panel}.hidden"):
        browser.click(f"#{name}-counter")
    assert browser.wait(f"!{panel}.hidden"), f"{name} panel did not open"


def close_panel(browser, name):
    if not browser.eval(f"document.getElementById('{PANELS[name]}-panel')"
                        ".hidden"):
        browser.click(f"#{PANELS[name]}-close")


# The server runs "serve --warnings": the tee "t" posts a warning each time
# a branch is added or removed (every two seconds).
def check_log(browser):
    counter = "document.getElementById('log-counter')"
    rows = "document.querySelectorAll('#log-rows .log-row').length"
    tee = element_selector("t")
    badge = f"{tee}?.querySelector('.badge')"

    assert browser.wait(f"!!{counter}.querySelector('.count.warning')"), \
        "warning count did not appear"
    assert browser.eval(f"{counter}.title.includes('warning')")
    assert browser.wait(f"!!{badge}"), "no badge on the tee"

    open_panel(browser, "log")
    assert browser.wait("!document.getElementById('log-panel').hidden"), \
        "log panel did not open"
    assert browser.wait(f"{rows} > 0"), "no rows in the log panel"
    assert browser.eval("document.querySelector('#log-rows .log-path')"
                        ".textContent") == "t"
    assert browser.eval("document.querySelector('#log-rows .log-path')"
                        ".title.startsWith('pipeline0/t')")

    browser.click("#log-rows .log-path")
    assert browser.wait("document.querySelector('#details h2')?.textContent"
                        " === 't'"), "clicking a row did not select the tee"

    browser.click("#details .mute-toggle")
    assert browser.wait(f"{rows} === 0"), "muted rows still shown"
    assert browser.wait(f"{badge}?.classList.contains('muted')"), \
        "badge not shown as muted"
    browser.call("Page.reload")
    assert browser.wait(f"{badge}?.classList.contains('muted')"), \
        "mute not remembered after reload"
    assert browser.wait("!document.getElementById('log-panel').hidden"), \
        "the open panel not kept after reload"
    # The list is drawn again when the log arrives: click until it works.
    assert browser.click_until("#log-muted .unmute", f"{rows} > 0"), \
        "unmuting did not bring rows back"

    browser.click("#log-clear")
    assert browser.wait(f"{rows} === 0"), "clear did not empty the panel"
    assert browser.eval(f"!{counter}.querySelector('.count')"), \
        "count shown after clear"
    assert browser.wait(f"{rows} > 0", timeout=6), \
        "new warnings did not appear after clear"
    assert browser.eval("document.querySelector('#log-rows .log-count')"
                        ".textContent") == "", "count did not restart at clear"
    close_panel(browser, "log")


CAPS_SUMMARIES = [
    ("video/x-raw, format=(string)I420, width=(int)1280, height=(int)720, "
     "framerate=(fraction)30/1", "video/x-raw I420 1280×720 30/1"),
    ("audio/x-raw, format=(string)S16LE, layout=(string)interleaved, "
     "rate=(int)44100, channels=(int)2", "audio/x-raw S16LE 44100 Hz 2 ch"),
    ("video/x-h264, stream-format=(string)avc, alignment=(string)au, "
     "width=(int)640, height=(int)480", "video/x-h264 avc au 640×480"),
    ("video/x-raw(memory:GLMemory), format=(string)RGBA, width=(int)1920, "
     "height=(int)1080, framerate=(fraction)0/1",
     "video/x-raw(GLMemory) RGBA 1920×1080"),
    ("video/x-raw(memory:SystemMemory), format=(string)NV12, width=(int)2, "
     "height=(int)2", "video/x-raw NV12 2×2"),
    ("image/jpeg", "image/jpeg"),
    ("", ""),
]

# parseCaps: commas inside lists, arrays and quotes stay in the value.
CAPS_FIELDS = [
    ('video/x-raw, format=(string){ I420, NV12 }, width=(int)2',
     [["format", "{ I420, NV12 }"], ["width", "2"]]),
    ('audio/x-opus, streamheader=(buffer)< 4f70, 4f71 >, name=(string)"a, b"',
     [["streamheader", "< 4f70, 4f71 >"], ["name", "a, b"]]),
]


def check_caps(browser):
    labels = "document.querySelectorAll('#graph .edge-label').length"
    switch = "document.getElementById('caps-labels').getAttribute('aria-pressed')"

    got = browser.eval("import('./caps.js').then(m => " +
                       json.dumps([c for c, _ in CAPS_SUMMARIES]) +
                       ".map(m.summarizeCaps))")
    assert got == [s for _, s in CAPS_SUMMARIES], f"caps summaries: {got}"
    got = browser.eval("import('./caps.js').then(m => " +
                       json.dumps([c for c, _ in CAPS_FIELDS]) +
                       ".map((c) => m.parseCaps(c).fields))")
    assert got == [f for _, f in CAPS_FIELDS], f"caps fields: {got}"

    tee = element_selector("t")
    tee_id = browser.eval(f"{tee}?.dataset.id")
    assert browser.click_until(
        f'#graph .node[data-id="{tee_id}"] > rect.box',
        "document.querySelector('#details h2')?.textContent === 't'"), \
        "could not select the tee"
    assert browser.wait("document.querySelector('#details .caps-toggle')"
                        "?.textContent.startsWith('video/x-raw')"), \
        "no caps in the details panel"
    full = "document.querySelector('#details tr.caps-full')"
    browser.click("#details .caps-toggle")
    assert browser.wait(f"{full}?.textContent.includes('width')"), \
        "full caps did not open"
    browser.click("#details .caps-toggle")
    assert browser.wait(f"!{full}"), "full caps did not close"

    tip = "document.querySelector('.tooltip')"
    pad = (f"{tee}?.querySelector('.pad[data-port]')")
    browser.hover(pad)
    assert browser.wait(f"!{tip}.hidden && {tip}.textContent.includes('t:sink')"
                        f" && {tip}.querySelector('.caps-media')"
                        "?.textContent.startsWith('video/x-raw')"), \
        "no pad tooltip with caps"
    browser.hover("[...document.querySelectorAll('#graph .edge-hit')]"
                  ".find(p => p.dataset.edge)")
    assert browser.wait(f"!{tip}.hidden && {tip}.textContent.includes('→')"), \
        "no link tooltip"
    browser.call("Input.dispatchMouseEvent", type="mouseMoved", x=2, y=2)
    assert browser.wait(f"{tip}.hidden"), "tooltip did not hide"

    assert browser.eval(labels) == 0, "caps labels shown with the switch off"
    browser.click("#caps-labels")
    assert browser.wait(f"{switch} === 'true' && {labels} > 0"), \
        "caps switch did not show labels"
    browser.call("Page.reload")
    assert browser.wait(f"{switch} === 'true' && {labels} > 0"), \
        "caps switch not remembered after reload"
    browser.click("#caps-labels")
    assert browser.wait(f"{switch} === 'false' && {labels} === 0"), \
        "caps switch did not hide labels"


UNITS = [
    ("formatStamp", 4802684583, "auto", "0:00:04.802684583"),
    ("formatStamp", 3723000000001, "auto", "1:02:03.000000001"),
    ("formatStamp", 4802684583, "ms", "4802.685 ms"),
    ("formatStamp", None, "auto", "—"),
    ("formatSpan", 850, "auto", "850 ns"),
    ("formatSpan", 850000, "auto", "850.0 µs"),
    ("formatSpan", 33333333, "auto", "33.3 ms"),
    ("formatSpan", -12400000, "auto", "-12.4 ms"),
    ("formatSpan", 2500000000, "auto", "2.500 s"),
    ("formatSpan", 33333333, "ns", "33333333 ns"),
    ("formatSpan", 33333333, "s", "0.033333 s"),
    ("formatBytes", 115200, None, "112.5 KiB"),
]


def check_units(browser):
    got = browser.eval("import('./units.js').then(m => " +
                       json.dumps([[f, v, u] for f, v, u, _ in UNITS]) +
                       ".map(([f, v, u]) => m[f](v, u)))")
    assert got == [s for *_, s in UNITS], f"units: {got}"


# keepPlaces: two unconnected parts; the upper one shrinks (a branch goes
# away), the lower one must stay where it was.
PLACES = """
import('./layout.js').then(({ keepPlaces }) => {
  const make = (lowerY) => ({
    width: 300, height: lowerY + 60,
    children: [
      { id: 'a', x: 10, y: 10, width: 50, height: 30,
        ports: [{ id: 'pa', x: 60, y: 20 }] },
      { id: 'b', x: 100, y: 10, width: 50, height: 30,
        ports: [{ id: 'pb', x: 100, y: 20 }] },
      { id: 'c', x: 10, y: lowerY, width: 50, height: 30, ports: [] },
    ],
    edges: [{ sources: ['pa'], targets: ['pb'], sections: [{
      startPoint: { x: 60, y: 20 }, endPoint: { x: 100, y: 20 } }] }],
  });
  const memory = { tops: new Map() };
  const first = make(120);
  keepPlaces(first, memory);
  const second = make(60);
  keepPlaces(second, memory);
  const third = make(200);
  keepPlaces(third, memory);
  return [first.children[2].y, second.children[2].y, second.height,
          second.children[0].y, second.edges[0].sections[0].startPoint.y,
          third.children[2].y];
})"""


def check_places(browser):
    got = browser.eval(PLACES)
    assert got == [120, 120, 180, 10, 20, 200], f"keepPlaces: {got}"


def check_levels(browser):
    times = "document.getElementById('pipeline-times')"
    assert browser.wait(f"!{times}.hidden && "
                        f"{times}.textContent.includes('running 0:')"), \
        "no pipeline times in the header"
    assert browser.wait("document.querySelectorAll('#graph .level-fill')"
                        ".length >= 2"), "no fill bars on queues"

    queue = element_selector("queue0")
    tip = "document.querySelector('.tooltip')"
    browser.hover(f"{queue}?.querySelector('rect.box')")
    assert browser.wait(f"!{tip}.hidden && "
                        f"{tip}.textContent.includes('queue0 (queue)') && "
                        f"{tip}.textContent.includes('overruns')"), \
        "no queue tooltip"
    browser.call("Input.dispatchMouseEvent", type="mouseMoved", x=2, y=2)

    # The level text sits on the queue; a click there selects the queue.
    assert browser.eval(
        "[...document.querySelectorAll('#graph .level-text')].every(t => {"
        " const r = t.getBoundingClientRect();"
        " const n = document.elementFromPoint(r.x + r.width / 2,"
        " r.y + r.height / 2);"
        " return !!n?.closest('.node[data-level]'); })"), \
        "the level text takes clicks meant for the queue"
    qid = browser.eval(f"{queue}?.dataset.id")
    assert browser.click_until(
        f'#graph .node[data-id="{qid}"] > rect.box',
        "document.querySelector('#details .queue-levels')"
        "?.textContent.includes('underruns')"), "no queue details"


def check_properties(browser):
    # The open panel covers the right side, where the sink is.
    browser.click("#details .close")
    assert browser.wait("document.getElementById('details').hidden")
    sink = browser.eval(f"{element_selector('fakesink0')}?.dataset.id")
    rows = "[...document.querySelectorAll('#details table.props tr')]"
    assert browser.click_until(
        f'#graph .node[data-id="{sink}"] > rect.box',
        f"{rows}.some(r => r.cells[1]?.textContent === 'sync')"), \
        "no properties in the details panel"
    stats = (f"{rows}.find(r => r.cells[1]?.textContent === 'stats')"
             "?.querySelector('button.caps-toggle')")
    assert browser.wait(f"!!{stats}"), "no stats structure"
    browser.eval(f"{stats}.click()")
    assert browser.wait("document.querySelector('#details dl.fields')"
                        "?.textContent.includes('rendered')"), \
        "structure did not open"
    # Stays open across refreshes.
    time.sleep(1.5)
    assert browser.eval("!!document.querySelector('#details dl.fields')")


def rendered_seen(browser):
    seen = []
    for data in browser.frames:
        message = json.loads(data)
        rendered = [p["value"]["fields"].get("rendered")
                    for e in message.get("elements", [])
                    for p in e.get("properties", []) if p["name"] == "stats"]
        seen.append((message["time"], rendered))
    return seen


def check_element_pins(browser):
    # The panel is open on fakesink0 (check_properties).
    rows = "[...document.querySelectorAll('#details table.props tr')]"
    deadline = (f"{rows}.find(r => r.cells[1]?.textContent === "
                "'processing-deadline')?.cells[2].textContent")
    assert browser.wait(f"{deadline} === '20.0 ms'"), "time not readable"

    card = ("[...document.querySelectorAll('.pin-card')].find(c => "
            "c.querySelector('.pin-title').textContent === 'fakesink0')")
    browser.eval(f"{rows}.find(r => r.cells[1]?.textContent === 'stats')"
                 ".querySelector('.pin-prop').click()")
    assert browser.wait(f"!!{card}"), "no element card"
    rendered = (f"[...{card}.querySelectorAll('dt')].find(d => d.textContent "
                "=== 'rendered')?.nextElementSibling.textContent")
    assert browser.wait(f"/^[0-9]+ \\(\\+[0-9.]+\\/s\\)$/.test({rendered})",
                        timeout=10), \
        f"no counter rate: {browser.eval(f'{card}?.innerText')!r}; " \
        f"last properties messages (time, rendered): {rendered_seen(browser)}"
    sink = browser.eval(f"{element_selector('fakesink0')}?.dataset.id")
    assert browser.eval(f"document.querySelector('#graph .node[data-id="
                        f"\"{sink}\"]').classList.contains('pinned')")
    assert browser.eval(f"{rows}.find(r => r.cells[1]?.textContent === "
                        "'stats').querySelector('.pin-prop.on') !== null")

    # A second property joins the card; removing it keeps the card.
    def prop(name):
        return (f"{rows}.find(r => r.cells[1]?.textContent === "
                f"'{name}').querySelector('.pin-prop')")
    names = f"[...{card}.querySelectorAll('dt')].map(d => d.textContent)"
    browser.eval(f"{prop('sync')}.click()")
    assert browser.wait(f"{names}.includes('sync')"), \
        "second property not on the card"
    browser.eval(f"{prop('sync')}.click()")
    assert browser.wait(f"!!{card} && !{names}.includes('sync') && "
                        f"{names}.includes('rendered')"), \
        "removing one property changed the rest of the card"

    browser.call("Page.reload")
    assert browser.wait(f"!!{card} && /^[0-9]/.test({rendered})"), \
        "element pin not remembered after reload"
    browser.eval(f"{card}.querySelector('.pin-close').click()")
    assert browser.wait(f"!{card}"), "element card not removed"


def check_latency(browser):
    labels = "document.querySelectorAll('#graph .latency-text').length"
    button = "document.getElementById('measure-all')"
    assert browser.wait(f"!!{element_selector('queue0')}")
    browser.click("#measure-all")
    assert browser.wait(f"{button}.textContent.includes('buf/s')"), \
        "measure all did not start"
    assert browser.wait(f"{labels} >= 3", timeout=6), "no latency labels"

    queue = browser.eval(f"{element_selector('queue0')}?.dataset.id")
    section = "document.querySelector('#details .latency-section')"
    assert browser.click_until(
        f'#graph .node[data-id="{queue}"] > rect.box',
        f"{section}?.textContent.includes('measured')"), "no latency section"
    assert browser.wait(f"/[0-9.]+ (ns|µs|ms) avg/.test({section}.textContent)"),\
        "no measured latency in the panel"
    browser.eval(f"{section}.querySelector('.measure-toggle').click()")
    assert browser.wait(f"{section}.textContent.includes('Stop measuring')")

    # Stopping "measure all" keeps the element measured by this page.
    browser.click("#measure-all")
    assert browser.wait(f"{button}.textContent === 'latency'")
    assert browser.wait(f"{labels} === 1", timeout=4), \
        "measure all labels did not go away"
    browser.eval(f"{section}.querySelector('.measure-toggle').click()")
    assert browser.wait(f"{labels} === 0", timeout=4)
    browser.click("#details .close")


def check_paths(browser):
    def pin(element, pad):
        port = browser.eval(pad_id_expression(element, pad))
        browser.click_point(f"document.querySelector('#graph "
                            f"[data-port=\"p{port}\"]')")
        return port

    def card(name):
        return ("[...document.querySelectorAll('.pin-card')].find(c => "
                "c.querySelector('.pin-title').textContent === "
                f"{json.dumps(name)})")

    def row(name, label_start):
        return (f"[...{card(name)}.querySelectorAll('dt')].find(d => "
                f"!d.hidden && d.textContent.startsWith("
                f"{json.dumps(label_start)}))")

    assert browser.wait(f"!!{element_selector('queue0')}")
    pin("queue0", "sink")
    assert browser.wait(f"!!{card('queue0:sink')}")
    pin("fakesink0", "sink")
    assert browser.wait(f"!!{card('fakesink0:sink')}")
    auto = row("fakesink0:sink", "from queue0:sink")
    assert browser.wait(f"/[0-9.]+ (ns|µs|ms) \\(/.test("
                        f"{auto}?.nextElementSibling.textContent)",
                        timeout=10), "no time from the upstream pinned pad"

    # Choose another pad by hand; Esc cancels choosing.
    hint = "document.getElementById('pick-hint')"
    browser.eval(f"{card('fakesink0:sink')}.querySelector('.pin-from').click()")
    assert browser.wait(f"!{hint}.hidden")
    browser.call("Input.dispatchKeyEvent", type="keyDown", key="Escape")
    assert browser.wait(f"{hint}.hidden"), "Esc did not cancel"
    browser.eval(f"{card('fakesink0:sink')}.querySelector('.pin-from').click()")
    port = browser.eval(pad_id_expression("videotestsrc0", "src"))
    browser.click_point(f"document.querySelector('#graph "
                        f"[data-port=\"p{port}\"]')")
    assert browser.wait(f"!!{row('fakesink0:sink', 'from videotestsrc0:src')}"),\
        "chosen pad not used"
    assert not browser.eval(f"!!{card('videotestsrc0:src')}"), \
        "choosing a pad pinned it"
    # Clicked again, it goes back to the nearest pinned pad upstream.
    browser.eval(f"{card('fakesink0:sink')}.querySelector('.pin-from').click()")
    assert browser.wait(f"!!{row('fakesink0:sink', 'from queue0:sink')}"), \
        "did not go back to the automatic choice"

    # Unpinning the card that is choosing stops choosing.
    browser.eval(f"{card('queue0:sink')}.querySelector('.pin-from').click()")
    assert browser.wait(f"!{hint}.hidden")
    browser.eval(f"{card('queue0:sink')}.querySelector('.pin-close').click()")
    assert browser.wait(f"{hint}.hidden"), "unpin did not cancel choosing"

    browser.eval(f"{card('fakesink0:sink')}.querySelector('.pin-close').click()")
    assert browser.wait("document.querySelectorAll('.pin-card').length === 0")


# Tee branches without queues, not live: the data flow blocks.
FAULTY_PIPELINE = "videotestsrc ! tee name=ft ! fakesink ft. ! fakesink"


def check_findings(browser, serve):
    server, url = start_server(serve, FAULTY_PIPELINE)
    try:
        counter = "document.getElementById('problems-counter')"
        browser.call("Page.navigate", url=url)
        assert browser.wait(f"/^[0-9]+$/.test({counter}.querySelector("
                            "'.count.warning')?.textContent)", timeout=10), \
            "no problems found in a faulty pipeline"
        # One panel at a time; the open one's button is pressed.
        open_panel(browser, "log")
        assert browser.wait("!document.getElementById('log-panel').hidden")
        open_panel(browser, "problems")
        assert browser.wait("document.getElementById('log-panel').hidden && "
                            "!document.getElementById('findings-panel')"
                            ".hidden"), "two panels open"
        assert browser.eval(f"{counter}.getAttribute('aria-pressed')") == \
            "true"
        rows = "document.getElementById('findings-rows')"
        assert browser.wait(f"{rows}.textContent.includes('Data flow "
                            "blocked at')"), "blocked finding not listed"
        assert browser.wait("!!document.querySelector('#graph "
                            ".pad.problem-warning')"), "pad not marked"
        assert browser.wait("!!document.querySelector('#graph "
                            ".edge.problem-warning')"), "link not marked"
        browser.eval("[...document.querySelectorAll('.finding-place')]"
                     ".find(b => b.textContent.includes('blocked at'))"
                     ".click()")
        assert browser.wait("document.querySelector('#details h2')"
                            "?.textContent === 'ft'"), \
            "clicking a problem did not select its element"
        assert browser.wait("document.querySelector('#details "
                            ".element-problems')?.textContent"
                            ".includes('blocked at')"), \
            "no problems in the details"
        # The panels cover the sink.
        close_panel(browser, "problems")
        browser.click("#details .close")
        sink = browser.eval(f"{element_selector('fakesink1')}?.dataset.id")
        box = f'#graph .node[data-id="{sink}"] > rect.box'
        # On a slow machine the graph can still be moving, or something
        # can lie over it: click once its middle is the sink's.
        assert browser.wait(f"(r => r.width > 0 && document.elementFromPoint("
                            f"r.x + r.width / 2, r.y + r.height / 2)"
                            f"?.closest('.node')?.dataset.id === '{sink}')"
                            f"(document.querySelector('{box}')"
                            ".getBoundingClientRect())", timeout=10), \
            "fakesink1 not clickable"
        assert browser.click_until(
            box, "document.querySelector('#details h2')?.textContent === "
            "'fakesink1'", tries=5), "could not select fakesink1"
        assert browser.wait("document.querySelector('#details dl')"
                            "?.textContent.includes('prerollwaiting for "
                            "data')"), "no preroll in the details"
        assert not browser.errors, f"errors in the page: {browser.errors}"
    finally:
        server.terminate()
        server.wait()


# The program ends by itself (EOS after 30 frames) and exits.
ENDING_PIPELINE = "videotestsrc num-buffers=30 ! queue ! fakesink sync=true"


def check_ended(browser, serve):
    server, url = start_server(serve, ENDING_PIPELINE)
    try:
        browser.call("Page.navigate", url=url)
        assert browser.wait(f"{NODES} > 0"), "graph was not drawn"
        server.wait(timeout=10)
        status = "document.getElementById('connection')"
        assert browser.wait(f"{status}.textContent === 'program ended'"), \
            "page did not see the program end"
        assert browser.eval("!document.getElementById('ended').hidden")
        # The last state stays on screen.
        assert browser.eval(f"{NODES} > 0"), "last state not kept"
        assert browser.wait("document.getElementById('pipeline-state')"
                            ".textContent.includes('NULL')"), \
            "the stopped pipeline is not shown as NULL"
        queue = browser.eval(f"{element_selector('queue0')}?.dataset.id")
        assert browser.click_until(
            f'#graph .node[data-id="{queue}"] > rect.box',
            "document.querySelector('#details h2')?.textContent === "
            "'queue0'"), "could not select queue0"
        # Buttons stay usable; what they turn on starts with the next run.
        toggle = "document.querySelector('#details .measure-toggle')"
        assert browser.wait(f"!!{toggle}?.offsetParent")
        assert browser.eval("!!document.querySelector('#details "
                            ".mute-toggle')?.offsetParent")
        browser.eval(f"{toggle}.click()")
        assert browser.wait("document.querySelector('#details "
                            ".latency-section').textContent.includes("
                            "'starts when the program runs')"), \
            "no note that measuring starts with the next run"
        browser.eval(f"{toggle}.click()")
        assert browser.wait(f"{toggle}.textContent === 'Measure'")
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait()


# Two pipelines: a selector; an element that makes new timestamps cannot be
# measured.
TWO_PIPELINES = ("audiotestsrc is-live=true ! identity name=retime "
                 "datarate=1000 ! fakesink sync=false")
SECOND_PIPELINE = "videotestsrc is-live=true ! fakesink name=other-sink"


def check_two_pipelines(browser, serve):
    server, url = start_server(serve, TWO_PIPELINES, "--second",
                               SECOND_PIPELINE)
    try:
        browser.call("Page.navigate", url=url)
        select = "document.getElementById('pipeline-select')"
        assert browser.wait(f"!{select}.hidden && {select}.options.length "
                            "=== 2", timeout=10), "no pipeline selector"
        for want in ("other-sink", "retime"):
            browser.eval(f"(s => {{ s.value = [...s.options].find(o => "
                         f"o.selected === false).value; s.dispatchEvent("
                         f"new Event('change')); }})({select})")
            if browser.wait(f"!!{element_selector(want)}"):
                continue
            browser.eval(f"(s => {{ s.value = [...s.options].find(o => "
                         f"o.selected === false).value; s.dispatchEvent("
                         f"new Event('change')); }})({select})")
            assert browser.wait(f"!!{element_selector(want)}"), \
                f"selecting a pipeline did not show {want}"

        node = browser.eval(f"{element_selector('retime')}?.dataset.id")
        assert browser.click_until(
            f'#graph .node[data-id="{node}"] > rect.box',
            "document.querySelector('#details h2')?.textContent === "
            "'retime'", tries=5), "could not select retime"
        browser.eval("document.querySelector('#details .latency-section "
                     ".measure-toggle').click()")
        assert browser.wait("[...document.querySelectorAll('#graph "
                            ".latency-text')].some(t => t.textContent"
                            ".startsWith('latency ?'))", timeout=10), \
            "no 'latency ?' for an element that makes new timestamps"
        browser.eval("document.querySelector('#details .latency-section "
                     ".measure-toggle').click()")
        assert not browser.errors, f"errors in the page: {browser.errors}"
    finally:
        server.terminate()
        server.wait()


# Negotiation fails when the source starts: the finding compares what each
# side handles, and a press on its place selects the element.
FORMAT_PIPELINE = "videotestsrc ! capsfilter name=cf ! fakesink"


def check_format_finding(browser, serve):
    # It fails at once and ends: it waits for the page to be there.
    server, url = start_server(serve, FORMAT_PIPELINE, "--set-caps",
                               "cf=audio/x-raw", wait=True)
    try:
        browser.call("Page.navigate", url=url)
        caps = "document.querySelector('#findings-rows .finding-caps')"
        assert browser.wait(f"!!{caps}", timeout=15), \
            "no format comparison in the finding"
        assert browser.eval(f"{caps}.querySelectorAll('.finding-caps-side')"
                            ".length === 2")
        assert browser.eval(f"!!{caps}.querySelector('.caps-media.differs')"), \
            "the different media types are not marked"
        assert browser.eval("/[0-9]+× · [0-9]+ (s|min) ago/.test(document."
                            "querySelector('#findings-rows .finding-count')"
                            ".textContent)"), "no count and time"
        open_panel(browser, "problems")
        assert browser.wait("!document.getElementById('findings-panel')"
                            ".hidden")
        browser.click("#findings-rows .finding.error .finding-place")
        assert browser.wait("!!document.querySelector('#details h2')"), \
            "pressing the place did not select the element"
        close_panel(browser, "problems")
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait()


# Record, stop, snapshot, download and delete from the panel.
def check_recordings(browser, serve):
    server, url = start_server(serve, RESTART_PIPELINE)
    try:
        browser.call("Page.navigate", url=url)
        assert browser.wait(f"!!{element_selector('q-r')}", timeout=10)
        counter = "document.getElementById('recordings-counter')"
        rows = "document.querySelectorAll('#recordings-rows .rec-row')"
        open_panel(browser, "recordings")
        assert browser.wait("!document.getElementById('recordings-panel')"
                            ".hidden")
        assert browser.wait("!document.getElementById('record').disabled")
        browser.click("#record")
        count = f"{counter}.querySelector('.count')"
        assert browser.wait(f"{count}?.textContent.startsWith('●')"), \
            "no running recording in the header"
        # Updated about once a second: at least 2 s, not an exact text.
        assert browser.wait(f"(m => m && Number(m[1]) * 60 + Number(m[2]) "
                            f">= 2)(/([0-9]+):([0-9]{{2}}) · [0-9.]+ "
                            f"(B|KB|MB)$/.exec({count}?.textContent))",
                            timeout=10), \
            "the running recording's time and size do not grow"
        browser.click("#record")
        assert browser.wait(f"!{count} && {rows}.length === 1"), \
            "recording did not stop"
        # The panel's top edge resizes it.
        height = browser.eval("document.getElementById('recordings-panel')"
                              ".offsetHeight")
        browser.drag("#recordings-panel .panel-resize", 0, -80)
        assert browser.wait("document.getElementById('recordings-panel')"
                            f".offsetHeight > {height + 40}"), "no resize"
        browser.click("#snapshot")
        assert browser.wait(f"{rows}.length === 2 && {rows}[0].querySelector("
                            "'.rec-kind').textContent === 'snapshot' && "
                            f"!{rows}[0].classList.contains('running')",
                            timeout=8), "no snapshot in the list"
        name = browser.eval(f"{rows}[1].dataset.name")
        text = browser.eval(f"fetch('api/recordings/' + "
                            f"{json.dumps(name)}).then(r => r.text())")
        assert text.startswith('{"format":"pipeline-monitor-recording"'), \
            "download is not a recording"
        assert '"view":{"pins":' in text.split("\n")[0], "no view saved"
        check_viewer(browser, rows, url)
        # Delete asks for a second click.
        for _ in range(2):
            browser.eval(f"{rows}[0].querySelector('.rec-delete').click()")
            browser.eval(f"{rows}[0].querySelector('.rec-delete').click()")
            time.sleep(0.5)
        assert browser.wait(f"{rows}.length === 0"), "recordings not deleted"
        assert not browser.errors, f"errors in the page: {browser.errors}"
        close_panel(browser, "recordings")
    finally:
        server.terminate()
        server.wait()


# The row of a file added from this computer.
def added_row(name):
    return ("[...document.querySelectorAll('#recordings-rows .rec-row.added')]"
            f".find(r => r.dataset.file === {json.dumps(name)})")


# Opens an added file from the list, with the mouse.
def open_added(browser, name):
    open_panel(browser, "recordings")
    assert browser.wait(f"!!{added_row(name)}"), f"{name} not in the list"
    browser.click_point(f"{added_row(name)}.querySelector('.rec-open')")


# Open a recording read-only, from the list and from a file, and close it.
def check_viewer(browser, rows, url):
    banner = "document.getElementById('viewing')"
    status = "document.getElementById('connection')"
    name = browser.eval(f"{rows}[1].dataset.name")
    # In the live page, Open opens a viewer tab; this one keeps monitoring.
    browser.click_point(f"{rows}[1].querySelector('.rec-open')")
    tab = browser.other_tab("#viewer=" + urllib.parse.quote(name, safe=""))
    try:
        assert tab.wait(f"!{banner}.hidden"), "Open: the tab shows nothing"
        assert not tab.errors, f"errors in the viewer tab: {tab.errors}"
    finally:
        tab.close_tab()
    assert browser.eval(f"{status}.textContent === 'connected'")

    # Add file… puts a file from this computer in the list; its Open opens
    # a viewer tab that shows it (also after a reload).
    path = os.path.join(RECORDINGS, name)
    open_panel(browser, "recordings")
    browser.set_file("#file-input", path)
    row = added_row(name)
    assert browser.wait(f"!!{row} && {row}.querySelector('.rec-kind')"
                        ".textContent.includes('file')"), "file not added"
    browser.click_point(f"{row}.querySelector('.rec-open')")
    tab = browser.other_tab("#viewer-file")
    try:
        assert tab.wait(f"!{banner}.hidden && {banner}.textContent"
                        ".includes('Recording of serve')"), \
            "an added file did not open in the viewer tab"
        tab.call("Page.reload")
        assert tab.wait(f"!{banner}.hidden"), \
            "an added file not shown again after a reload of its tab"
        assert not tab.errors, f"errors in the viewer tab: {tab.errors}"
    finally:
        tab.close_tab()
    assert browser.eval(f"{status}.textContent === 'connected'")
    # Remove takes it off the list only.
    browser.click_point(f"{row}.querySelector('.rec-remove')")
    assert browser.wait(f"!{row}"), "Remove did not take the file off"
    assert os.path.exists(path), "Remove deleted the file"
    # A dropped file is added too.
    browser.eval("fetch('api/recordings/' + encodeURIComponent("
                 f"{json.dumps(name)})).then(r => r.text()).then(t => {{ "
                 "const d = new DataTransfer(); "
                 "d.items.add(new File([t], 'dropped.jsonl')); "
                 "window.dispatchEvent(new DragEvent('drop', "
                 "{dataTransfer: d, cancelable: true})); })")
    assert browser.wait(f"!!{added_row('dropped.jsonl')}"), \
        "a dropped file was not added"
    browser.click_point(f"{added_row('dropped.jsonl')}"
                        ".querySelector('.rec-remove')")
    assert not browser.errors, f"errors in the page: {browser.errors}"

    # That tab: a viewer, not connected to the program.
    browser.call("Page.navigate", url="about:blank")
    browser.call("Page.navigate", url=f"{url}#viewer={name}")
    assert browser.wait(f"!{banner}.hidden && "
                        f"{banner}.textContent.includes('Recording of serve')"), \
        "recording not opened"
    assert browser.wait(f"{status}.textContent === 'viewer'")
    # Its row is marked, and its Open reads it again.
    stored = (f"[...{rows}].find(r => r.dataset.name === "
              f"{json.dumps(name)})")
    assert browser.wait(f"{stored}?.classList.contains('shown') && "
                        f"{stored}.querySelector('.rec-open').textContent "
                        "=== 'Reload'"), "the shown recording is not marked"
    assert browser.eval(f"[...{rows}].filter(r => r.classList.contains("
                        "'shown')).length === 1")
    open_panel(browser, "recordings")
    browser.eval("document.getElementById('viewing').hidden = true")
    browser.click_point(f"{stored}.querySelector('.rec-open')")
    assert browser.wait(f"!{banner}.hidden && {stored}.classList.contains("
                        "'shown')"), "Reload did not show it again"
    # Playback: shows the end; Play moves on; the slider goes anywhere.
    time_label = "document.getElementById('play-time').textContent"
    assert browser.wait(f"!document.getElementById('playback').hidden && "
                        f"/^0:0[0-9] \\/ 0:0[0-9]$/.test({time_label})"), \
        "no playback controls"
    browser.eval("(s => { s.value = 0; s.dispatchEvent(new Event('input')); })"
                 "(document.getElementById('seek'))")
    assert browser.wait(f"{time_label}.startsWith('0:00 /')"), \
        "the slider did not go to the start"
    browser.click("#play")
    assert browser.wait(f"!{time_label}.startsWith('0:00 /')", timeout=4), \
        "playing did not move on"
    browser.click("#play")
    # At 16× it plays to the end and stops; Play again starts over.
    for speed in ("4×", "16×"):
        browser.click("#speed")
        assert browser.wait("document.getElementById('speed').textContent "
                            f"=== {json.dumps(speed)}")
    browser.click("#play")
    assert browser.wait("document.getElementById('play').title === 'Play' && "
                        f"(t => t[0] === t[1])({time_label}.split(' / '))",
                        timeout=6), "playing did not stop at the end"
    browser.click("#play")
    assert browser.wait("document.getElementById('play').title === 'Play'",
                        timeout=6), "playing again did not stop at the end"
    browser.click("#speed")
    assert browser.wait(f"!!{element_selector('q-r')}"), "no recorded graph"
    node = browser.eval(f"{element_selector('q-r')}?.dataset.id")
    assert browser.click_until(
        f'#graph .node[data-id="{node}"] > rect.box',
        "document.querySelector('#details h2')?.textContent === 'q-r'",
        tries=5), "could not select q-r in the recording"
    assert browser.wait("[...document.querySelectorAll('#details table.props "
                        "tr')].some(r => r.cells[1]?.textContent === "
                        "'max-size-buffers')"), \
        "no properties from the recording"
    assert browser.eval("!document.querySelector('#details .measure-toggle')"
                        "?.offsetParent"), "Measure shown in a recording"
    assert browser.eval("!document.querySelector('#details .pin-prop')"
                        "?.offsetParent"), "pinning shown in a recording"
    assert browser.eval("!document.getElementById('record').offsetParent"), \
        "Record shown in a viewer"
    browser.click("#viewing-close")
    assert browser.wait(f"{banner}.hidden && location.hash === '#viewer' && "
                        "!document.getElementById('recordings-panel').hidden"), \
        "Close did not go back to the empty viewer"
    assert browser.eval(f"![...{rows}].some(r => r.classList.contains("
                        "'shown'))"), "a row still marked after Close"

    # The same file, added from the disk, opens here; the mark moves to it.
    browser.set_file("#file-input", path)
    open_added(browser, name)
    assert browser.wait(f"!{banner}.hidden"), "file not opened"
    assert browser.wait(f"{added_row(name)}.classList.contains('shown') && "
                        f"[...{rows}].filter(r => r.classList.contains("
                        "'shown')).length === 1"), "the mark did not move"
    browser.click("#viewing-close")
    assert browser.wait(f"{banner}.hidden")
    not_one = os.path.join(RECORDINGS, "not-a-recording.jsonl")
    with open(not_one, "w") as f:
        f.write("hello\n")
    browser.set_file("#file-input", not_one)
    assert browser.wait("document.getElementById('recordings-error')"
                        ".textContent.includes('not a recording')"), \
        "a file that is not a recording was not refused"
    os.remove(not_one)
    check_bad_recordings(browser, os.path.join(RECORDINGS, name))

    # Back to the live page for the rest.
    browser.call("Page.navigate", url="about:blank")
    browser.call("Page.navigate", url=url)
    assert browser.wait(f"{status}.textContent === 'connected'")
    assert browser.wait("!document.getElementById('recordings-panel')"
                        ".hidden"), "the open panel not kept in the live tab"
    assert browser.wait(f"{rows}.length === 2")


# Files that are not what pipeline-monitor writes, made from a good one:
# refused, opened without their bad lines, or closed again if damaged.
def check_bad_recordings(browser, good):
    with open(good) as f:
        lines = f.read().splitlines()
    header = json.loads(lines[0])
    started = header["started"]
    error = "document.getElementById('recordings-error')"
    banner = "document.getElementById('viewing')"

    # A new name each time (the same file again is no change), kept until
    # the end: the page reads it after it is chosen.
    made = []

    def open_file(text, opens=True):
        path = os.path.join(RECORDINGS, f"bad-{len(made)}.jsonl")
        made.append(path)
        with open(path, "w") as f:
            f.write(text)
        browser.eval(f"{error}.hidden = true")
        browser.set_file("#file-input", path)
        if opens:
            open_added(browser, os.path.basename(path))

    for change in ({"version": 2}, {"started": "yesterday"}):
        open_file(json.dumps({**header, **change}) + "\n" +
                  "\n".join(lines[1:]), opens=False)
        assert browser.wait(f"!{error}.hidden && "
                            f"{error}.textContent.includes('not a recording')"), \
            f"a header with {change} was not refused"
        assert browser.eval(f"{banner}.hidden")

    # Lines that are not messages, of unknown types or without a time, and
    # a last line cut off: left out.
    junk = ["null", "5", '"text"', '{"type":"constructor","time":1}',
            '{"type":"levels"}', '{"type":"graph","time":"soon"}',
            json.dumps({"type": "future", "time": started})]
    open_file("\n".join([lines[0], *junk, *lines[1:], lines[1][:20]]) + "\n")
    assert browser.wait(f"!{banner}.hidden && "
                        f"!!{element_selector('q-r')}"), \
        "a recording with bad lines was not opened"
    browser.click("#viewing-close")
    assert browser.wait(f"{banner}.hidden")

    # A graph that is not one: the page says so and stays usable.
    open_file("\n".join([json.dumps({**header, "view": {"collapsed": 5}}),
                         json.dumps({"type": "graph", "time": started + 1,
                                     "pipelines": 5})]) + "\n")
    assert browser.wait(f"!{error}.hidden && "
                        f"{error}.textContent.includes('damaged')"), \
        "a damaged recording was not reported"
    assert browser.eval(f"{banner}.hidden && location.hash === '#viewer'")
    open_added(browser, os.path.basename(good))
    assert browser.wait(f"!{banner}.hidden && "
                        f"!!{element_selector('q-r')}"), \
        "a good recording did not open after a damaged one"
    browser.click("#viewing-close")
    assert browser.wait(f"{banner}.hidden")
    assert not browser.errors, f"errors in the page: {browser.errors}"
    for path in made:
        os.remove(path)


# A folder that cannot be made: Record says why, and nothing runs.
def check_record_fails(browser, serve):
    blocker = os.path.join(RECORDINGS, "not-a-folder")
    with open(blocker, "w"):
        pass
    folder = os.path.join(blocker, "recordings").replace("\\", "/")
    server, url = start_server(serve, RESTART_PIPELINE,
                               extra=f'recordings="{folder}"')
    try:
        browser.call("Page.navigate", url=url)
        assert browser.wait(f"!!{element_selector('q-r')}", timeout=10)
        open_panel(browser, "recordings")
        assert browser.wait("!document.getElementById('record').disabled")
        browser.click("#record")
        error = "document.getElementById('recordings-error')"
        assert browser.wait(f"!{error}.hidden && "
                            f"{error}.textContent.startsWith('Cannot create')"), \
            "a failed Record was not reported"
        assert browser.eval("!document.querySelector("
                            "'#recordings-counter .count')"), \
            "a failed Record shows as running"

        # Without the program the live page adds and opens nothing: its
        # last state stays on screen.
        added = os.path.join(RECORDINGS, "added.jsonl")
        with open(added, "w") as f:
            f.write(json.dumps({"format": "pipeline-monitor-recording",
                                "version": 1, "kind": "snapshot",
                                "started": 1794139542123456}) + "\n")
        browser.set_file("#file-input", added)
        assert browser.wait(f"!!{added_row('added.jsonl')}")
        server.terminate()
        server.wait()
        assert browser.wait("document.getElementById('connection')"
                            ".textContent !== 'connected'", timeout=10)
        assert browser.wait(
            "document.getElementById('add-file').disabled && [...document"
            ".querySelectorAll('#recordings-rows button')].every(b => "
            "b.disabled)"), "buttons still on without the program"
        assert browser.eval("document.querySelector('.rec-offline')"
                            ".textContent.includes('pipeline-monitor command')")
        browser.eval("(() => { const d = new DataTransfer(); "
                     "d.items.add(new File(['{}'], 'late.jsonl')); "
                     "window.dispatchEvent(new DragEvent('drop', "
                     "{dataTransfer: d, cancelable: true})); })()")
        assert browser.wait(f"{error}.textContent.includes("
                            "'when the program runs')"), \
            "a file dropped without the program was not refused"
        assert browser.eval(f"!{added_row('late.jsonl')}")
        assert browser.eval(f"!!{element_selector('q-r')}"), \
            "the last state is gone"
        assert not browser.errors, f"errors in the page: {browser.errors}"
        close_panel(browser, "recordings")
        os.remove(added)
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait()
        os.remove(blocker)


# The pipeline-monitor command: recordings without a program.
def check_command(browser, serve, tool):
    server, _ = start_server(serve, RESTART_PIPELINE, extra="record=on")
    time.sleep(3)
    server.terminate()
    server.wait()

    def run(*args):
        proc = subprocess.Popen([tool, "--no-browser", "--port", "0",
                                 "--recordings", RECORDINGS, *args],
                                stdout=subprocess.PIPE, text=True)
        line = proc.stdout.readline()
        m = re.match(r"pipeline-monitor: (http://\S+)", line)
        assert m, f"no address from the command: {line!r}"
        return proc, m.group(1)

    def fails(code, *args):
        done = subprocess.run([tool, *args], capture_output=True, text=True,
                              timeout=10)
        assert done.returncode == code, f"{args}: exit {done.returncode}"

    out = subprocess.run([tool, "--version"], capture_output=True, text=True,
                         timeout=10).stdout
    assert re.match(r"pipeline-monitor \d+\.\d+", out), out
    fails(2, "--port")
    fails(2, "play")
    fails(2, "open", "a", "b")
    fails(1, "open", os.path.join(RECORDINGS, "missing.jsonl"))

    status = "document.getElementById('connection')"
    banner = "document.getElementById('viewing')"
    rows = "document.querySelectorAll('#recordings-rows .rec-row')"
    tool_proc, url = run()
    try:
        # Without "#viewer" the page finds out and shows the viewer.
        browser.call("Page.navigate", url="about:blank")
        browser.call("Page.navigate", url=url.split("#")[0])
        assert browser.wait(f"{status}.textContent === 'viewer' && "
                            "location.hash === '#viewer'", timeout=10), \
            "the command's page is not a viewer"
        assert browser.wait(f"{rows}.length >= 1"), "no recordings listed"
        path = os.path.join(RECORDINGS, browser.eval(f"{rows}[0].dataset.name"))
        browser.eval(f"{rows}[0].querySelector('.rec-open').click()")
        assert browser.wait(f"!{banner}.hidden && !!{element_selector('q-r')}"), \
            "recording not opened in the command's viewer"

        # Delete, with the mouse (a second click within 3 s).
        browser.click("#viewing-close")
        assert browser.wait(f"{banner}.hidden")
        spare = os.path.join(RECORDINGS, os.path.basename(path).replace(
            ".jsonl", "-copy.jsonl"))
        shutil.copy(path, spare)
        browser.call("Page.reload")
        delete = (f"[...{rows}].find(r => r.dataset.name === "
                  f"{json.dumps(os.path.basename(spare))})"
                  "?.querySelector('.rec-delete')")
        assert browser.wait(f"{delete} && !{delete}.disabled"), \
            "no Delete for the copy"
        browser.click_point(delete)
        assert browser.wait(f"{delete}?.textContent === 'Delete?'")
        browser.click_point(delete)
        assert browser.wait(f"!{delete}"), "Delete did not remove the row"
        assert not os.path.exists(spare), "Delete did not remove the file"
    finally:
        tool_proc.terminate()
        tool_proc.wait()
    # Without the command the list's buttons are off.
    assert browser.wait(f"[...{rows}].every(r => [...r.querySelectorAll("
                        "'button')].every(b => b.disabled))", timeout=10), \
        "the list's buttons still work without the command"
    # And they look it: dimmed, no pointer, no hover colour.
    delete = f"{rows}[0].querySelector('.rec-delete')"
    browser.call("Input.dispatchMouseEvent", type="mouseMoved", **dict(zip(
        ("x", "y"), browser.eval(f"(r => [r.x + r.width / 2, r.y + "
                                 f"r.height / 2])({delete}"
                                 ".getBoundingClientRect())"))))
    assert browser.eval(
        f"(s => Number(s.opacity) < 1 && s.cursor === 'default' && "
        f"s.borderTopColor === getComputedStyle(document.getElementById("
        f"'add-file')).borderTopColor)(getComputedStyle({delete}))"), \
        "a disabled button does not look disabled"

    tool_proc, url = run("open", path)
    try:
        assert url.endswith("#viewer=opened")
        browser.call("Page.navigate", url="about:blank")
        browser.call("Page.navigate", url=url)
        assert browser.wait(f"!{banner}.hidden && !!{element_selector('q-r')}",
                            timeout=10), "open FILE did not show it"
    finally:
        tool_proc.terminate()
        tool_proc.wait()
    os.remove(path)
    assert not browser.errors, f"errors in the page: {browser.errors}"


# Measurements are kept by element path: they come back with a new run,
# also when turned on while no program is connected.
RESTART_PIPELINE = ("audiotestsrc is-live=true ! queue name=q-r ! "
                    "identity name=i-r ! fakesink sync=true")


def check_measure_restart(browser, serve):
    port = free_port()
    section = "document.querySelector('#details .latency-section')"
    measured = f"/[0-9.]+ (ns|µs|ms) avg/.test({section}?.textContent)"

    def select(name):
        node = browser.eval(f"{element_selector(name)}?.dataset.id")
        assert browser.click_until(
            f'#graph .node[data-id="{node}"] > rect.box',
            "document.querySelector('#details h2')?.textContent === "
            f"{json.dumps(name)}", tries=5), f"could not select {name}"

    server, url = start_server(serve, RESTART_PIPELINE, port=port)
    programs = [server]
    try:
        browser.call("Page.navigate", url=url)
        assert browser.wait(f"!!{element_selector('q-r')}", timeout=10)
        select("q-r")
        browser.eval(f"{section}.querySelector('.measure-toggle').click()")
        assert browser.wait(measured, timeout=8), "queue not measured"

        # Killed: no goodbye. The last state stays, marked as such.
        server.kill()
        server.wait()
        assert browser.wait("!document.getElementById('lost').hidden"), \
            "no connection-lost banner"
        assert browser.wait("document.getElementById('pipeline-state')"
                            ".textContent.includes('(last known)')")
        assert browser.wait(f"{section}.textContent.includes("
                            "'starts when the program runs')")
        # Turned on while nothing is connected.
        browser.click("#details .close")
        select("i-r")
        browser.eval(f"{section}.querySelector('.measure-toggle').click()")
        assert browser.wait(f"{section}.textContent.includes("
                            "'starts when the program runs')")

        # A new run on the same address: both are measured again, by path.
        server, url = start_server(serve, RESTART_PIPELINE, port=port)
        programs.append(server)
        assert browser.wait("document.getElementById('connection')"
                            ".textContent === 'connected'", timeout=15), \
            "page did not reconnect"
        assert browser.wait("document.getElementById('lost').hidden")
        # The selection is found again by its path, as measurements are.
        assert browser.wait("document.querySelector('#details h2')"
                            "?.textContent === 'i-r'"), \
            "selection not kept across the restart"
        assert browser.wait(measured, timeout=8), "identity not measured " \
            "after the restart"
        browser.click("#details .close")
        select("q-r")
        assert browser.wait(measured, timeout=8), "queue not measured " \
            "after the restart"
        for name in ("q-r", "i-r"):
            browser.click("#details .close")
            select(name)
            browser.eval(f"{section}.querySelector('.measure-toggle')"
                         ".click()")
        assert not browser.errors, f"errors in the page: {browser.errors}"
    except (AssertionError, RuntimeError):
        # Why the page lost its selection: its connection and the programs.
        print(browser.history(), file=sys.stderr)
        for n, proc in enumerate(programs):
            print(f"program {n + 1}: exit {proc.poll()}; last output:",
                  file=sys.stderr)
            for line in proc.lines[-25:]:
                print("  " + line, file=sys.stderr)
        raise
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait()


def pin_row(name):
    return ("[...document.querySelectorAll('.pin-card dt')].find(d => "
            f"d.textContent === {json.dumps(name)})?.nextElementSibling"
            ".textContent")


def watched():
    return ("fetch('/api/pads').then(r => r.json())"
            ".then(j => j.pads.length)")


def check_pins(browser):
    cards = "document.querySelectorAll('.pin-card').length"
    card = "document.querySelector('.pin-card')"
    # The page may just have been reloaded.
    assert browser.wait(f"!!{element_selector('videotestsrc0')}")
    src = browser.eval(f"{element_selector('videotestsrc0')}?.dataset.id")
    pad = f'#graph .node[data-id="{src}"] .pad[data-port]'

    assert browser.click_until(pad, f"{cards} === 1"), "click did not pin"
    assert browser.eval(f"{card}.querySelector('.pin-title').textContent") \
        == "videotestsrc0:src"
    assert browser.wait(f"!!document.querySelector('{pad}.pinned')"), \
        "pad not marked as pinned"
    assert browser.wait(f"/^[1-9][0-9.]* ≈ fps$/.test({pin_row('buffers/s')})"), \
        "no live rate"
    assert browser.wait(f"{watched()}.then(n => n === 1)"), "no probe"
    assert browser.wait(f"/^0:00:/.test({pin_row('PTS')})")
    tip = "document.querySelector('.tooltip')"
    browser.hover(f"document.querySelector('{pad}')")
    assert browser.wait(f"!{tip}.hidden && "
                        f"{tip}.textContent.includes('buffers/s')"), \
        "no rate in the tooltip of a pinned pad"
    browser.call("Input.dispatchMouseEvent", type="mouseMoved", x=2, y=2)

    browser.click("#time-unit")
    assert browser.wait(f"{pin_row('duration')}?.endsWith(' ns')"), \
        "time unit did not change"
    for _ in range(3):
        browser.click("#time-unit")
    assert browser.wait(f"{pin_row('duration')} === '33.3 ms'"), \
        "time unit did not return to auto"

    browser.hover(f"{card}.querySelector('.pin-rows')")
    assert browser.wait("!!document.querySelector('#graph .pad.highlight')"), \
        "hovering a card did not highlight its pad"

    # A click on the card's title brings its pad to the middle.
    browser.click(".pin-card .pin-title")
    assert browser.wait(
        f"(p => (g => Math.abs(p.x + p.width / 2 - g.x - g.width / 2) < 20 && "
        f"Math.abs(p.y + p.height / 2 - g.y - g.height / 2) < 20)("
        f"document.getElementById('graph').getBoundingClientRect()))("
        f"document.querySelector('{pad}').getBoundingClientRect())"), \
        "clicking a card did not show its pad"

    browser.drag(".pin-card .pin-head", 220, 160)
    assert browser.wait(f"{card}.classList.contains('detached')"), \
        "dragging far did not detach"
    browser.call("Page.reload")
    assert browser.wait(f"{cards} === 1 && "
                        f"{card}.classList.contains('detached')"), \
        "pin not remembered after reload"
    assert browser.wait(f"/^[1-9]/.test({pin_row('buffers/s')})"), \
        "no live values after reload"

    browser.click(".pin-card .pin-anchor")
    assert browser.wait(f"!{card}.classList.contains('detached')"), \
        "anchor did not bring the card back"
    # Anchored again, the card moves when the graph changes.
    assert browser.click_until(".pin-card .pin-fold",
                               f"{card}.classList.contains('folded')"), \
        "no fold"
    assert browser.click_until(".pin-card .pin-fold",
                               f"!{card}.classList.contains('folded')"), \
        "no unfold"
    assert browser.click_until(".pin-card .pin-close", f"{cards} === 0"), \
        "card not removed"
    assert browser.wait(f"{watched()}.then(n => n === 0)"), \
        "probe not removed after unpin"

    # The example adds and removes a branch with "extra-sink" every 2 s.
    extra = f"{element_selector('extra-sink')}?.querySelector('.pad[data-port]')"
    # It can go between seeing it and clicking it: then wait for the next.
    for _ in range(3):
        assert browser.wait(f"!!{extra}", timeout=6), "no extra branch"
        if browser.try_click_point(extra) and \
                browser.wait(f"{cards} === 1", timeout=2):
            break
    assert browser.wait(f"{cards} === 1"), \
        f"could not pin the extra branch\n{browser.history()}"
    assert browser.wait(f"{card}.classList.contains('gone') && !{card}.hidden"
                        " && document.querySelector('.pin-note')"
                        ".textContent.startsWith('pad is gone')",
                        timeout=6), "card did not say the pad is gone"
    assert browser.wait(f"!{card}.classList.contains('gone') && "
                        f"/^[0-9]/.test({pin_row('buffers/s')})", timeout=6), \
        "pin did not follow the pad when it came back"
    # The card moves with the graph; clicking × was checked above.
    browser.eval("document.querySelector('.pin-card .pin-close').click()")
    assert browser.wait(f"{cards} === 0")


def screenshot(browser, url, path):
    browser.call("Emulation.setDeviceMetricsOverride", width=1280, height=660,
                 deviceScaleFactor=2, mobile=False)
    browser.call("Emulation.setEmulatedMedia", features=[
        {"name": "prefers-color-scheme", "value": "light"}])
    browser.call("Page.navigate", url=url)
    assert browser.wait(f"{NODES} > 0")
    time.sleep(1)
    # "Measure all" on: a latency label under every element; the audio
    # queue's details open at its latency, measured against reported.
    browser.click("#measure-all")
    browser.wait("document.querySelectorAll('#graph .latency-text').length "
                 ">= 5", timeout=6)
    queue = browser.eval(f"{element_selector('queue3')}?.dataset.id")
    section = "document.querySelector('#details .latency-section')"
    browser.click_until(f'#graph .node[data-id="{queue}"] > rect.box',
                        f"/ms avg/.test({section}?.textContent)")
    browser.eval("document.querySelector('#details').scrollTop = "
                 "document.querySelector('#details .latency-section')"
                 ".offsetTop - 40")
    # Smaller and to the left, so the queue is not under the panel.
    browser.click("#zoom-out")
    for kind, x in (("mousePressed", 700), ("mouseMoved", 620),
                    ("mouseMoved", 610), ("mouseReleased", 610)):
        browser.call("Input.dispatchMouseEvent", type=kind, x=x, y=120,
                     button="left", buttons=1, clickCount=1)
    time.sleep(3)
    browser.screenshot(path)


# What the page and the example program show when the first check fails.
def diagnose(browser, server):
    graph = ("fetch('/api/graph').then(r => r.json()).then(g => "
             "JSON.stringify(g.pipelines.map(p => [p.name, p.state, "
             "p.children.length])))")
    print("api/graph pipelines:", browser.eval(graph), file=sys.stderr)
    drawn = browser.wait(f"{NODES} > 0", timeout=20)
    print(f"graph drawn after 20 s more: {drawn}", file=sys.stderr)
    print("api/graph pipelines:", browser.eval(graph), file=sys.stderr)
    print(f"example running: {server.poll() is None}; its last output:",
          file=sys.stderr)
    for line in server.lines[-80:]:
        print("  " + line, file=sys.stderr)


def pad_id_expression(element, pad):
    return ("fetch('/api/graph').then(r => r.json()).then(g => { "
            "let id = null; const v = (e) => { if (e.name === "
            f"{json.dumps(element)}) for (const p of e.pads) if (p.name === "
            f"{json.dumps(pad)}) id = p.id; e.children.forEach(v); }}; "
            "g.pipelines.forEach(v); return id; })")


def main():
    serve = sys.argv[1]
    shot = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--screenshot" \
        else None
    # The pipeline-monitor command, if given: "--tool PATH".
    tool = sys.argv[sys.argv.index("--tool") + 1] \
        if "--tool" in sys.argv else None
    chrome = find_chrome()
    if chrome is None:
        print("Chrome not found, skipping")
        return 77

    profile = tempfile.mkdtemp(prefix="pm-ui-test-")
    if shot:
        server, url = start_server(serve, SHOT_PIPELINE)
    else:
        server, url = start_server(serve, PIPELINE, "--warnings", "--flow-log",
                                   debug="2")
    browser = None
    try:
        browser = Browser(chrome, profile)
        if shot:
            screenshot(browser, url, shot)
        else:
            try:
                check(browser, url)
            except AssertionError:
                diagnose(browser, server)
                raise
            check_findings(browser, serve)
            check_ended(browser, serve)
            check_measure_restart(browser, serve)
            check_recordings(browser, serve)
            check_record_fails(browser, serve)
            if tool:
                check_command(browser, serve, tool)
            check_two_pipelines(browser, serve)
            check_format_finding(browser, serve)
    finally:
        if browser:
            browser.close()
        server.terminate()
        server.wait()
        shutil.rmtree(profile, ignore_errors=True)
        shutil.rmtree(RECORDINGS, ignore_errors=True)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
