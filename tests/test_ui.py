# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Checks the web UI in headless Chrome over the DevTools protocol.

Usage: test_ui.py SERVE_EXECUTABLE [--screenshot FILE]
Exits with 77 (skipped) when Chrome is not available.
With PM_JS_COVERAGE=FILE, writes which lines of the page code ran to FILE.
"""

import base64
import functools
import http.server
import json
import os
import random
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
    # path: one file, or a list of files chosen together.
    def set_file(self, selector, path):
        root = self.call("DOM.getDocument")["root"]["nodeId"]
        node = self.call("DOM.querySelector", nodeId=root,
                         selector=selector)["nodeId"]
        self.call("DOM.setFileInputFiles",
                  files=path if isinstance(path, list) else [path],
                  nodeId=node)

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
    check_held_click(browser)
    check_caps(browser)
    check_units(browser)
    check_levels_label(browser)
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


# Closes the details panel and waits until it is gone: the page hides it a
# moment after the click, and a selection made before then is lost.
def close_details(browser):
    browser.click("#details .close")
    assert browser.wait("document.getElementById('details').hidden"), \
        f"the details panel did not close\n{browser.history()}"


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


# A click on the details panel is not lost when the page updates while
# the button is pressed (the example changes its graph every 2 s): the
# panel waits for the release.
def check_held_click(browser):
    node = browser.eval(f"{element_selector('t')}?.dataset.id")
    assert browser.click_until(
        f'#graph .node[data-id="{node}"] > rect.box',
        "document.querySelector('#details h2')?.textContent === 't'",
        tries=5), "could not select t"
    close = "document.querySelector('#details .close')"
    browser.eval(f"window.heldClose = {close}")
    x, y = browser.eval(f"(r => [r.x + r.width / 2, r.y + r.height / 2])"
                        f"({close}.getBoundingClientRect())")
    browser.call("Input.dispatchMouseEvent", type="mousePressed", x=x, y=y,
                 button="left", clickCount=1)
    time.sleep(4.5)
    assert browser.eval(f"{close} === window.heldClose"), \
        "the panel was rebuilt under a pressed button"
    browser.call("Input.dispatchMouseEvent", type="mouseReleased", x=x, y=y,
                 button="left", clickCount=1)
    assert browser.wait("document.getElementById('details').hidden"), \
        f"the held click was lost\n{browser.history()}"


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

    # The panel may still be filling in and move the button under the
    # click: clicked again until it takes. If it never does, show what
    # happened: the clicks, and when the details changed.
    assert browser.click_until("#details .mute-toggle", f"{rows} === 0"), \
        f"muted rows still shown\n{browser.history()}"
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
    # A new warning may arrive at any time: check both at once.
    assert browser.wait(f"{rows} === 0"
                        f" && !{counter}.querySelector('.count')"), \
        "clear did not empty the panel and the count: " + browser.eval(
            f"`rows ${{{rows}}}, count '${{{counter}.textContent}}'`")
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
    assert browser.eval(f"{tip}.textContent.includes('click to pin')"), \
        "the live pad tooltip does not offer pinning"
    assert browser.eval(f"{tip}.textContent.includes('modepush')"), \
        "no mode in the pad tooltip"
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


LEVELS = [
    (233333334 / 233333332, "100 %", "full"),
    (0.996, "100 %", "full"),
    (1.3, "130 %", "full"),
    (0.8, "80 %", "high"),
    (0.746, "75 %", "high"),
    (0.5, "50 %", "ok"),
]


def check_levels_label(browser):
    got = browser.eval("import('./levels.js').then(m => " +
                       json.dumps([r for r, _, _ in LEVELS]) +
                       ".map((r) => [m.percent(r), m.fillClass(r)]))")
    assert got == [[p, c] for _, p, c in LEVELS], f"level labels: {got}"


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
    close_details(browser)
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
    close_details(browser)


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
        close_details(browser)
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
    select = "document.getElementById('pipeline-select')"
    try:
        browser.call("Page.navigate", url=url)
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
    except (AssertionError, RuntimeError):
        # What the page and the program showed.
        for what, expression in (
                ("selector", f"JSON.stringify({{hidden: {select}?.hidden, "
                 f"options: [...({select}?.options ?? [])].map(o => "
                 "o.textContent)})"),
                ("api/graph pipelines", "fetch('/api/graph').then(r => "
                 "r.json()).then(g => JSON.stringify(g.pipelines.map(p => "
                 "[p.name, p.state, p.children.length])))")):
            try:
                print(f"{what}: {browser.eval(expression)}", file=sys.stderr)
            except RuntimeError as e:
                print(f"{what}: not readable: {str(e)[:200]}",
                      file=sys.stderr)
        print(f"page errors: {browser.errors}", file=sys.stderr)
        print(browser.history(), file=sys.stderr)
        print(f"program: exit {server.poll()}; last output:", file=sys.stderr)
        for line in server.lines[-40:]:
            print("  " + line, file=sys.stderr)
        raise
    finally:
        server.terminate()
        server.wait()


# Negotiation fails when the source starts: the finding compares what each
# side handles, and a press on its place selects the element.
FORMAT_PIPELINE = "videotestsrc ! capsfilter name=cf ! fakesink"


PULL_PIPELINE = ("fakesrc can-activate-pull=true ! "
                 "fakesink can-activate-pull=true can-activate-push=false")


def check_pull_mode(browser, serve):
    server, url = start_server(serve, PULL_PIPELINE)
    try:
        browser.call("Page.navigate", url=url)
        sink = element_selector("fakesink0")
        assert browser.wait(f"!!{sink}", timeout=15), "no graph"
        tip = "document.querySelector('.tooltip')"
        browser.hover(f"{sink}?.querySelector('.pad[data-port]')")
        assert browser.wait(f"!{tip}.hidden && {tip}.textContent"
                            ".includes('modepull')"), "no pull mode in the tooltip"
        assert browser.eval(f"{tip}.textContent.includes("
                            "'no caps (data is pulled)')"), \
            "a pulled pad's tooltip waits for caps"
        browser.call("Input.dispatchMouseEvent", type="mouseMoved", x=2, y=2)
        sink_id = browser.eval(f"{sink}?.dataset.id")
        assert browser.click_until(
            f'#graph .node[data-id="{sink_id}"] > rect.box',
            "document.querySelector('#details h2')?.textContent"
            " === 'fakesink0'"), "could not select the sink"
        cells = ("[...document.querySelectorAll('#details tr')].find(r => "
                 "r.cells[0]?.textContent === 'sink')")
        assert browser.wait(f"{cells}?.cells[2].textContent === 'pull'"), \
            "no mode in the details panel"
        assert browser.eval(f"{cells}.cells[5].title.includes('pulled')")
        close_details(browser)
    finally:
        server.terminate()
        server.wait()


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
        # If the click is lost, show what happened: the clicks, the
        # details panel, and the finding's element in the graph.
        assert browser.wait("!!document.querySelector('#details h2')"), \
            "pressing the place did not select the element\n" + \
            browser.history() + "\nplace: " + browser.eval(
                "JSON.stringify([...document.querySelectorAll("
                "'#findings-rows .finding-place')].map(p => [p.textContent, "
                "p.disabled])) + ', graph nodes: ' + "
                "document.querySelectorAll('#graph .node').length")
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
    assert browser.wait(f"!!{row} && {row}.querySelector('.rec-source')"
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
    # Playback: opens at its start; the slider goes anywhere; Play moves on.
    time_label = "document.getElementById('play-time').textContent"
    assert browser.wait(f"!document.getElementById('playback').hidden && "
                        "/^0:00\\.000 \\/ 0:0[0-9]\\.[0-9]{3}$/.test("
                        f"{time_label}) && "
                        "document.getElementById('seek').value === '0'"), \
        "the recording did not open at its start: " + browser.eval(
            f"{time_label} + ' ' + document.getElementById('seek').value")
    browser.eval("(s => { s.value = 1000; s.dispatchEvent(new Event('input'));"
                 " })(document.getElementById('seek'))")
    assert browser.wait(f"(t => t[0] === t[1])({time_label}.split(' / '))"), \
        "the slider did not go to the end"
    browser.eval("(s => { s.value = 0; s.dispatchEvent(new Event('input')); })"
                 "(document.getElementById('seek'))")
    assert browser.wait(f"{time_label}.startsWith('0:00.000 /')"), \
        "the slider did not go to the start"
    browser.click("#play")
    assert browser.wait(f"!{time_label}.startsWith('0:00.000 /')", timeout=4), \
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
    assert browser.eval(f"[...{rows}].some(r => !r.classList.contains("
                        "'added'))"), "no stored recording in the list"
    assert browser.wait(KINDS_ALIGNED), "the kind column does not line up"
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
    check_moments(browser, os.path.join(RECORDINGS, name))
    check_stream_levels(browser, os.path.join(RECORDINGS, name))

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


def move_slider(browser, value):
    browser.eval(f"(s => {{ s.value = {value}; s.dispatchEvent("
                 "new Event('input')); })(document.getElementById('seek'))")


def open_messages(browser, header, messages, name):
    path = os.path.join(RECORDINGS, name)
    with open(path, "w") as f:
        f.write("\n".join(json.dumps(m) for m in [header, *messages]) + "\n")
    browser.set_file("#file-input", path)
    open_added(browser, name)
    return path


def type_in(browser, label, text):
    """Clicks the time or step label and types text, then Enter."""
    browser.eval(f"document.getElementById('{label}').click()")
    field = "time-input" if label == "play-time" else "step-input"
    browser.eval(f"(i => {{ i.value = {json.dumps(text)}; i.dispatchEvent("
                 "new KeyboardEvent('keydown', {key: 'Enter', bubbles: true}));"
                 f" }})(document.getElementById('{field}'))")


# Each moment of a recording shows what was recorded then, and the bar says
# where: the time, the step, ◂ ▸, typing either. Made from a good
# recording: steps at 0, 0.1 s (the graph), 0.2 s (its levels), 0.4 s (the
# graph again), 3 s (the pipeline gone).
def check_moments(browser, good):
    with open(good) as f:
        lines = [json.loads(line) for line in f.read().splitlines()]
    header, started = lines[0], lines[0]["started"]
    graph = next(m for m in lines if m.get("type") == "graph"
                 and m["pipelines"])
    levels = next(m for m in lines if m.get("type") == "levels"
                  and m["queues"])
    empty = {"type": "graph", "protocol": 1, "pipelines": [], "links": []}
    end = {"type": "end", "protocol": 1, "reason": "stopped"}
    path = open_messages(browser, header, [
        {**empty, "time": started},
        {"type": "levels", "protocol": 1, "time": started, "queues": [],
         "pipelines": []},
        {**graph, "time": started + 100000},
        {**levels, "time": started + 200000},
        {**graph, "time": started + 400000},
        {**empty, "time": started + 3000000},
        {**end, "time": started + 3000000},
    ], "moments.jsonl")

    labels = "document.querySelectorAll('#graph .level-text').length"
    moment = "document.getElementById('empty-moment')"
    shown = "document.getElementById('play-time').textContent"
    step = "document.getElementById('play-step').textContent"
    hint = "document.getElementById('play-hint')"
    button = ("[...document.querySelectorAll('#empty-moment button')]"
              ".find(b => b.textContent === {})")

    def at(time, n):
        assert browser.wait(f"{shown} === '{time} / 0:03.000' && "
                            f"{step} === 'step {n} / 5'"), \
            f"not at {time}, step {n}: {browser.eval(shown)}, " \
            f"{browser.eval(step)}"

    # Opens at its start: nothing yet, and the way to the pipeline.
    at("0:00.000", 1)
    assert browser.eval(f"!{moment}.hidden && {moment}.textContent.includes("
                        "'No pipeline yet. The program builds it at "
                        "0:00.100.')"), "no empty moment at the start"
    assert browser.eval("document.getElementById('step-back').disabled")
    assert browser.eval("document.getElementById('play').classList"
                        ".contains('glow')"), "Play does not glow"
    browser.eval("document.dispatchEvent(new KeyboardEvent('keydown', "
                 "{key: 'Shift'}))")
    assert browser.wait("!document.getElementById('play').classList"
                        ".contains('glow')"), "Play still glows after a key"
    browser.eval(f"{button.format(json.dumps('Go to the first pipeline'))}"
                 ".click()")
    at("0:00.100", 2)
    # The graph of 0.1 s, without the levels of 0.2 s.
    assert browser.wait(f"!!{element_selector('q-r')} && {labels} === 0 && "
                        f"{moment}.hidden"), "levels shown before recorded"

    # ◂ ▸ and the arrow keys.
    browser.click("#step-next")
    at("0:00.200", 3)
    assert browser.wait(f"{labels} > 0"), "no levels once they were recorded"
    browser.eval("document.body.dispatchEvent(new KeyboardEvent('keydown', "
                 "{key: 'ArrowLeft', bubbles: true}))")
    at("0:00.100", 2)
    browser.eval("document.body.dispatchEvent(new KeyboardEvent('keydown', "
                 "{key: 'ArrowRight', bubbles: true}))")
    at("0:00.200", 3)

    # A typed time goes to the nearest step; halfway to the later one.
    for typed, time, n in (("0.25", "0:00.200", 3), ("0.3", "0:00.400", 4),
                           ("0:00.35", "0:00.400", 4), ("0.15", "0:00.200", 3),
                           ("2.9", "0:03.000", 5), ("0", "0:00.000", 1)):
        type_in(browser, "play-time", typed)
        at(time, n)
    for wrong in ("abc", "9:59"):
        type_in(browser, "play-time", wrong)
        assert browser.wait(f"!{hint}.hidden && {hint}.textContent === "
                            "'Type a time from 0:00.000 to 0:03.000, for "
                            "example 7.25 or 0:07.250.'"), \
            f"no hint for the time {wrong}"
        at("0:00.000", 1)
    assert browser.eval("!document.getElementById('time-input').hidden"), \
        "the field closed after a wrong time"
    browser.eval("document.getElementById('time-input').dispatchEvent("
                 "new KeyboardEvent('keydown', {key: 'Escape', "
                 "bubbles: true}))")
    assert browser.wait(f"{hint}.hidden && document.getElementById("
                        "'time-input').hidden"), "Esc did not cancel"

    # A typed step; the end, and back to the pipeline.
    type_in(browser, "play-step", "9")
    assert browser.wait(f"{hint}.textContent === 'Type a step from 1 to 5.'"), \
        "no hint for a wrong step"
    type_in(browser, "play-step", "5")
    at("0:03.000", 5)
    assert browser.wait(f"!{moment}.hidden && {moment}.textContent.includes("
                        "'No pipeline here: the program removed it at "
                        "0:03.000.')"), "no empty moment at the end"
    assert browser.eval("document.getElementById('step-next').disabled")
    browser.eval(f"{button.format(json.dumps('Back to the last pipeline'))}"
                 ".click()")
    assert browser.wait(f"!!{element_selector('q-r')} && {shown} === "
                        "'0:02.999 / 0:03.000'"), "Back did not show it"
    browser.click("#viewing-close")
    assert browser.wait("document.getElementById('viewing').hidden")
    assert browser.wait(f"{moment}.hidden"), "the empty moment kept after Close"
    os.remove(path)

    # A gap: removed at 1 s, built again at 2 s.
    path = open_messages(browser, header, [
        {**graph, "time": started},
        {**empty, "time": started + 1000000},
        {**graph, "time": started + 2000000},
        {**end, "time": started + 3000000},
    ], "gap.jsonl")
    assert browser.wait(f"{shown}.endsWith('/ 0:02.000')"), "gap not opened"
    move_slider(browser, 500)
    assert browser.wait(f"{moment}.textContent.includes('No pipeline here: "
                        "the program removed it at 0:01.000 and builds the "
                        "next one at 0:02.000.')"), \
        "no message in a gap: " + browser.eval(
            f"{moment}.textContent + ' | ' + {shown}")
    browser.eval(f"{button.format(json.dumps('Go to the next pipeline'))}"
                 ".click()")
    assert browser.wait(f"!!{element_selector('q-r')} && {shown}"
                        ".startsWith('0:02.000')"), "Next did not show it"
    browser.click("#viewing-close")
    os.remove(path)

    # Never a pipeline.
    path = open_messages(browser, header, [
        {**empty, "time": started}, {**end, "time": started + 1000000}],
        "none.jsonl")
    assert browser.wait(f"{moment}.textContent === 'This recording has no "
                        "pipeline: the program did not build one while it "
                        f"was recorded.' && !{moment}.querySelector('button')"), \
        "no message for a recording without a pipeline: " + browser.eval(
            f"{moment}.textContent + ' | ' + {shown} + ' | ' + "
            "document.getElementById('viewing').hidden")
    browser.click("#viewing-close")
    os.remove(path)


# A multiqueue shows a bar and a number per stream, on the stream's row
# between its pad names, and no level for the whole element; a queue keeps
# its one label with the limit.
def check_stream_levels(browser, good):
    with open(good) as f:
        header = json.loads(f.readline())
    started = header["started"]

    def pad(pad_id, name, direction, presence):
        caps = ("video/x-h264, stream-format=(string)avc, alignment=(string)au,"
                " width=(int)1280, height=(int)720")
        return {"id": pad_id, "name": name, "path": f"p/multiqueue0:{name}",
                "direction": direction, "presence": presence, "ghost": False,
                "mode": "push", "caps": caps}

    def element(element_id, name, factory, kind, pads):
        return {"id": element_id, "name": name, "path": f"p/{name}",
                "factory": factory, "type": kind, "bin": False,
                "state": "PLAYING", "pads": pads, "children": []}

    graph = {"type": "graph", "protocol": 1, "time": started, "links": [],
             "pipelines": [{
                 "id": 1, "name": "p", "path": "p", "factory": "pipeline",
                 "type": "GstPipeline", "bin": True, "state": "PLAYING",
                 "pads": [], "children": [
                     element(2, "multiqueue0", "multiqueue", "GstMultiQueue", [
                         pad(3, "sink_0", "sink", "request"),
                         pad(4, "src_0", "src", "sometimes"),
                         pad(5, "sink_1", "sink", "request"),
                         pad(6, "src_1", "src", "sometimes")]),
                     element(7, "q", "queue", "GstQueue", [
                         pad(8, "sink", "sink", "always"),
                         pad(9, "src", "src", "always")])]}]}
    levels = {"type": "levels", "protocol": 1, "time": started,
              "pipelines": [], "queues": [
                  {"element": 2, "path": "p/multiqueue0", "kind": "multiqueue",
                   "limits": {"buffers": 0, "bytes": 10485760,
                              "time": 1000000000},
                   "levels": [{"pad": 3, "buffers": 1, "bytes": 10,
                               "time": 0},
                              {"pad": 5, "buffers": 30, "bytes": 100,
                               "time": 1020000000}],
                   "overruns": 0, "underruns": 0, "buffering": None},
                  {"element": 7, "path": "p/q", "kind": "queue",
                   "leaky": "no", "limits": {"buffers": 200, "bytes": 0,
                                             "time": 0},
                   "levels": [{"buffers": 100, "bytes": 0, "time": 0}],
                   "overruns": 0, "underruns": 0, "buffering": None}]}
    path = open_messages(browser, header, [
        graph, levels, {"type": "end", "protocol": 1, "time": started + 1000,
                        "reason": "stopped"}], "streams.jsonl")
    assert browser.wait(f"!!{element_selector('multiqueue0')} && document.querySelectorAll("
                        "'#graph .level-text').length === 3"), \
        "no level texts for the streams and the queue"
    # For each level text: its text, class, and whether it sits on a sink
    # row of its box, between that row's pad names.
    rows = json.loads(browser.eval("""JSON.stringify((() => {
      const box = (name) => [...document.querySelectorAll('#graph .node')]
        .find(n => n.querySelector(':scope > .name')?.textContent === name)
        .getBoundingClientRect();
      const names = [...document.querySelectorAll('#graph .padlabel')]
        .map(t => [t.textContent, t.getBoundingClientRect()]);
      return [...document.querySelectorAll('#graph .level-text')].map(t => {
        const r = t.getBoundingClientRect();
        const mid = (r.top + r.bottom) / 2;
        const row = names.filter(([, b]) => mid > b.top && mid < b.bottom);
        const sink = row.find(([n]) => n.startsWith('sink'));
        const src = row.find(([n]) => n.startsWith('src'));
        const bars = [...document.querySelectorAll('#graph .level-fill')]
          .filter(b => { const q = b.getBoundingClientRect();
                         return q.top < r.bottom && q.bottom > r.top; });
        return {text: t.textContent, cls: t.getAttribute('class'),
                sink: sink?.[0] ?? null,
                between: !!sink && !!src && r.left > sink[1].right &&
                  r.right < src[1].left,
                bars: bars.length,
                inMq: (m => r.left > m.left && r.right < m.right &&
                  r.top > m.top && r.bottom < m.bottom)(box('multiqueue0'))};
      });
    })())"""))
    mq = sorted((r for r in rows if r["inMq"]), key=lambda r: str(r["sink"]))
    assert [(r["text"], r["sink"], r["between"], r["bars"]) for r in mq] == [
        ("0 %", "sink_0", True, 1), ("102 %", "sink_1", True, 1)], \
        f"multiqueue rows: {rows}"
    assert "ok" in mq[0]["cls"] and "full" in mq[1]["cls"], \
        f"multiqueue colours: {rows}"
    queue = [r for r in rows if not r["inMq"]]
    assert [r["text"] for r in queue] == ["50 % buf"], \
        f"the queue's label: {rows}"

    # The pad table: "sometimes" and the other short cells on one line,
    # the headers apart.
    node = browser.eval(f"{element_selector('multiqueue0')}?.dataset.id")
    assert browser.click_until(
        f'#graph .node[data-id="{node}"] > rect.box',
        "document.querySelector('#details h2')?.textContent === 'multiqueue0'"), \
        "could not select the multiqueue"
    table = json.loads(browser.eval("""JSON.stringify((() => {
      const lines = (cell) => { const r = document.createRange();
        r.selectNodeContents(cell);
        return new Set([...r.getClientRects()].map(q => Math.round(q.top))).size; };
      const table = [...document.querySelectorAll('#details table')]
        .find(t => t.rows[0]?.cells[3]?.textContent === 'presence');
      const heads = [...table.rows[0].cells].map(c => { const r =
        document.createRange(); r.selectNodeContents(c);
        return r.getBoundingClientRect(); });
      return {wrapped: [...table.rows].slice(1).flatMap(row =>
          [...row.cells].slice(0, 4).filter(c => lines(c) > 1)
            .map(c => c.textContent)),
        touching: heads.slice(1).filter((h, i) => h.left - heads[i].right < 4)
          .length};
    })())"""))
    assert table == {"wrapped": [], "touching": 0}, f"the pad table: {table}"
    close_details(browser)

    # Open and Reload as wide: the next buttons line up.
    open_panel(browser, "recordings")
    lefts = browser.eval("JSON.stringify([...document.querySelectorAll("
                         "'.rec-row .rec-open')].filter(b => b.offsetParent)"
                         ".map(b => [b.textContent, b.nextElementSibling"
                         ".getBoundingClientRect().left]))")
    lefts = json.loads(lefts)
    assert {t for t, _ in lefts} == {"Open", "Reload"} and \
        len({round(x) for _, x in lefts}) == 1, f"buttons not lined up: {lefts}"
    close_panel(browser, "recordings")
    browser.click("#viewing-close")
    os.remove(path)

    # A card dragged away from its pad goes with the pad: hidden before the
    # pad exists, back where it was dragged to after.
    empty = {"type": "graph", "protocol": 1, "time": started,
             "pipelines": [], "links": []}
    later = {**graph, "time": started + 100000}
    path = open_messages(browser, {**header, "view": {
        "pins": [{"path": "p/multiqueue0:src_0"}]}}, [
        empty, later, {"type": "end", "protocol": 1,
                       "time": started + 1000000, "reason": "stopped"}],
        "dragged.jsonl")
    card = "document.querySelector('.pin-card')"
    assert browser.wait(f"!!{card} && {card}.hidden"), \
        "the card shown before its pad exists"
    browser.click("#step-next")
    assert browser.wait(f"!{card}.hidden"), "no card once the pad exists"
    browser.drag(".pin-card .pin-head", 200, 150)
    assert browser.wait(f"{card}.classList.contains('detached')"), \
        "the card was not dragged away"
    where = browser.eval(f"{card}.style.left + ' ' + {card}.style.top")
    browser.click("#step-back")
    assert browser.wait(f"{card}.hidden"), \
        "a dragged card stays before its pad exists"
    browser.click("#step-next")
    assert browser.wait(f"!{card}.hidden && {card}.style.left + ' ' + "
                        f"{card}.style.top === {json.dumps(where)}"), \
        "the dragged card did not come back where it was"
    browser.click("#viewing-close")
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


# The kind column starts at the same place in every row of the list
# (stored, added, demo rows alike).
KINDS_ALIGNED = ("(xs => xs.length > 1 && xs.every(x => Math.abs(x - xs[0]) < 1))"
                 "([...document.querySelectorAll('#recordings-rows .rec-row "
                 ".rec-kind')].map(k => k.getBoundingClientRect().left))")


# Damaged versions of a recording (fixed seed: repeatable): lines lost,
# doubled, swapped or cut; values of the wrong type, null, huge, negative;
# unknown types; deep nesting; garbage.
def damaged_recordings(text, count, seed=11):
    rnd = random.Random(seed)
    lines = text.splitlines()
    odd = [None, "", "x", -1, 2 ** 63, -(2 ** 63), 1e308, [], {}, [1, "a"],
           {"a": {"b": [None]}}, True]

    def shake(value, depth=0):
        if isinstance(value, dict) and value and depth < 6:
            key = rnd.choice(list(value))
            if rnd.random() < 0.6:
                value[key] = shake(value[key], depth + 1)
            elif rnd.random() < 0.5:
                del value[key]
            else:
                value[key] = rnd.choice(odd)
            return value
        if isinstance(value, list) and value and depth < 6:
            i = rnd.randrange(len(value))
            value[i] = shake(value[i], depth + 1)
            return value
        return rnd.choice(odd)

    for _ in range(count):
        out = list(lines)
        for _ in range(rnd.randint(1, 4)):
            # The header (line 0) now and then too.
            i = rnd.randrange(0 if rnd.random() < 0.15 else 1, len(out))
            kind = rnd.randrange(8)
            if kind == 0:
                del out[i]
            elif kind == 1:
                out.insert(i, out[i])
            elif kind == 2:
                j = rnd.randrange(1, len(out))
                out[i], out[j] = out[j], out[i]
            elif kind == 3:
                out[i] = out[i][:rnd.randrange(len(out[i]) + 1)]
            elif kind == 4:
                out.insert(i, rnd.choice(["null", "5", "[]", "{}", "\x00",
                                          "{" * 200, "garbage"]))
            else:
                try:
                    message = json.loads(out[i])
                except ValueError:
                    continue
                if kind == 5 and isinstance(message, dict):
                    message["type"] = rnd.choice(["graph", "levels", "pads",
                                                  "properties", "latency",
                                                  "zzz", 7])
                message = shake(message)
                out[i] = json.dumps(message)
        yield "\n".join(out) + "\n"


def check_guide(browser, site):
    browser.call("Page.navigate", url="about:blank")
    browser.call("Page.navigate", url=site)
    link = "document.getElementById('guide-link')"
    assert browser.wait(f"!!{link}?.offsetParent"), "no Guide link on the site"
    assert browser.eval("[...document.querySelectorAll('#empty a')].some(a => "
                        "a.getAttribute('href') === 'guide/')")
    for page, heading in (("", "Get started"), ("tour.html", "Tour"),
                          ("options.html", "Options"),
                          ("troubleshooting.html", "Troubleshooting")):
        browser.call("Page.navigate", url=f"{site}guide/{page}")
        assert browser.wait("document.querySelector('main h1')?.textContent"
                            f" === {json.dumps(heading)}"), f"guide {page}"
        assert browser.wait("[...document.images].every(i => i.complete && "
                            "i.naturalWidth > 0)"), f"a picture missing: {page}"
        assert browser.eval("document.querySelectorAll('header a').length "
                            ">= 6"), f"no page links: {page}"
    browser.click("header a.viewer")
    assert browser.wait("!!document.querySelector('#recordings-rows "
                        ".rec-row.demo')"), "Open the viewer did not lead back"


# A recording made from a live page with a pinned pad and a pinned
# property, as a user makes one; returns its file.
def record_with_pins(browser, serve):
    server, url = start_server(serve, RESTART_PIPELINE)
    try:
        browser.call("Page.navigate", url="about:blank")
        browser.call("Page.navigate", url=url)
        assert browser.wait(f"!!{element_selector('fakesink0')}", timeout=10)
        cards = "document.querySelectorAll('.pin-card').length"
        pad = (f"{element_selector('fakesink0')}"
               "?.querySelector('.pad[data-port]')")
        for _ in range(5):
            browser.try_click_point(pad)
            if browser.wait(f"{cards} === 1", timeout=3):
                break
        assert browser.eval(f"{cards} === 1"), "could not pin the pad"
        sink = browser.eval(f"{element_selector('fakesink0')}?.dataset.id")
        rows = "[...document.querySelectorAll('#details table.props tr')]"
        stats = f"{rows}.find(r => r.cells[1]?.textContent === 'stats')"
        assert browser.click_until(f'#graph .node[data-id="{sink}"] > rect.box',
                                   f"!!{stats}", tries=5), "no properties"
        browser.eval(f"{stats}.querySelector('.pin-prop').click()")
        assert browser.wait(f"{cards} === 2"), "could not pin the property"
        close_details(browser)
        open_panel(browser, "recordings")
        assert browser.wait("!document.getElementById('record').disabled")
        before = set(os.listdir(RECORDINGS))
        browser.click("#record")
        time.sleep(3)
        browser.click("#record")
        assert browser.wait("!document.querySelector("
                            "'#recordings-counter .count')"), "did not stop"
        close_panel(browser, "recordings")
        # Unpinned again, so later checks start without them.
        for _ in range(2):
            browser.eval("document.querySelector('.pin-card .pin-close')"
                         ".click()")
        assert browser.wait(f"{cards} === 0")
    finally:
        server.terminate()
        server.wait()
    made = [n for n in set(os.listdir(RECORDINGS)) - before
            if n.endswith(".jsonl")]
    assert len(made) == 1, f"recordings made: {made}"
    return os.path.join(RECORDINGS, made[0])


# The published site: the page with no pipeline-monitor server behind it,
# served as static files with the site's <meta> line and demo recordings.
def check_site(browser, serve):
    server, _ = start_server(serve, RESTART_PIPELINE, extra="record=on")
    time.sleep(3)
    server.terminate()
    server.wait()
    recorded = max((os.path.join(RECORDINGS, n) for n in os.listdir(RECORDINGS)
                    if n.endswith(".jsonl")), key=os.path.getmtime)
    pinned = record_with_pins(browser, serve)

    # Built as the published site is (packaging/build-site.py), served
    # under /pipeline-monitor/ as it will be. The first demo is a real
    # recording made with 0.10.0 (format version 1): every later version
    # must still show it.
    here = os.path.dirname(os.path.abspath(__file__))
    site = tempfile.mkdtemp(prefix="pm-site-")
    demos = os.path.join(site, "demo-sources")
    os.makedirs(demos)
    shutil.copy(os.path.join(here, "data", "recording-0.10.0.jsonl"),
                os.path.join(demos, "example.jsonl"))
    shutil.copy(pinned, os.path.join(demos, "pins.jsonl"))
    with open(os.path.join(demos, "demos.json"), "w", encoding="utf-8") as f:
        json.dump([{"title": "Example pipeline", "file": "example.jsonl"},
                   {"title": "Pinned", "file": "pins.jsonl"}], f)
    built = subprocess.run([sys.executable, os.path.join(
        here, "..", "packaging", "build-site.py"),
        os.path.join(site, "pipeline-monitor"), "--demos", demos],
        capture_output=True, text=True)
    assert built.returncode == 0, f"build-site.py failed: {built.stderr}"
    guide = os.path.isdir(os.path.join(site, "pipeline-monitor", "guide"))
    shutil.copy(recorded, os.path.join(site, "shared.jsonl"))

    handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                                directory=site)
    handler.log_message = lambda *args: None
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{httpd.server_address[1]}"
    banner = "document.getElementById('viewing')"
    error = "document.getElementById('recordings-error')"
    demo = ("document.querySelector('#recordings-rows .rec-row.demo')")
    pins_demo = ("[...document.querySelectorAll('#recordings-rows "
                 ".rec-row.demo')].find(r => r.textContent.includes("
                 "'Pinned'))")
    try:
        browser.call("Page.navigate", url="about:blank")
        browser.call("Page.navigate", url=f"{base}/pipeline-monitor/")
        assert browser.wait(f"!!{demo}"), "no demo recording listed"
        assert browser.eval("document.getElementById('connection')"
                            ".textContent === 'viewer'")
        assert browser.eval("document.querySelector('.rec-offline')"
                            ".textContent.includes('never uploaded')")
        # Open, with the mouse: shown here, marked, no server needed.
        browser.click_point(f"{demo}.querySelector('.rec-open')")
        # Its pipeline was built after the start: it opens there, empty,
        # with the way to the pipeline.
        go = ("[...document.querySelectorAll('#empty-moment button')].find("
              "b => b.textContent === 'Go to the first pipeline')")
        assert browser.wait(f"!{banner}.hidden && !!{go} && "
                            f"{demo}.classList.contains('shown')"), \
            "the 0.10.0 recording did not open at its start"
        assert browser.wait("!document.getElementById('playback').hidden && "
                            "/^0:00\\.000 \\/ 0:0[0-9]\\.[0-9]{3}$/.test("
                            "document.getElementById('play-time')"
                            ".textContent)"), \
            "no playback for the 0.10.0 recording"
        browser.eval(f"{go}.click()")
        assert browser.wait(f"!!{element_selector('q-r')}"), \
            "Go to the first pipeline did not show it"
        # A recording's state is just its state, not "last known".
        state = "document.getElementById('pipeline-state').textContent"
        assert browser.wait(f"!!{state} && !{state}.includes('last known')"), \
            "a recording's state says 'last known'"
        # Removed before the end: both ends are empty.
        for end in (0, 1000):
            move_slider(browser, end)
            assert browser.wait(f"!{element_selector('q-r')} && "
                                "!document.getElementById('empty-moment')"
                                ".hidden"), \
                f"slider at {end}: not an empty moment"
        move_slider(browser, 500)
        assert browser.wait(f"!!{element_selector('q-r')}"), \
            "no pipeline in the middle of the 0.10.0 recording"
        # The row: one title, a Download button, marked by its colour.
        assert browser.eval(f"{demo}.querySelector('.rec-title')"
                            ".textContent === 'Example pipeline'")
        assert browser.eval(f"{demo}.querySelector('.rec-download')"
                            ".tagName === 'BUTTON'"), "Download is not a button"
        assert browser.eval(f"{demo}.getAttribute('aria-current') === 'true'"
                            f" && !{demo}.textContent.includes('shown')")
        node = browser.eval(f"{element_selector('q-r')}?.dataset.id")
        assert browser.click_until(
            f'#graph .node[data-id="{node}"] > rect.box',
            "[...document.querySelectorAll('#details table.props tr')]"
            ".some(r => r.cells[1]?.textContent === 'max-size-buffers')",
            tries=5), "no recorded properties from the 0.10.0 recording"
        close_details(browser)
        # A recording's pads cannot be pinned: the tooltip does not offer it.
        tip = "document.querySelector('.tooltip')"
        browser.hover(f"{element_selector('q-r')}?.querySelector("
                      "'.pad[data-port]')")
        assert browser.wait(f"!{tip}.hidden && "
                            f"{tip}.textContent.includes('q-r:')"), \
            "no pad tooltip in a recording"
        assert browser.eval(f"!{tip}.textContent.includes('pin')"), \
            "a recording's pad tooltip offers pinning"
        browser.click("#viewing-close")
        assert browser.wait(f"{banner}.hidden")
        # The page's pins when it recorded: read-only cards, their values
        # following the slider (blank before the first recorded values).
        cards = "document.querySelectorAll('.pin-card')"
        open_panel(browser, "recordings")
        browser.click_point(f"{pins_demo}.querySelector('.rec-open')")
        assert browser.wait(f"!{banner}.hidden && {cards}.length === 2"), \
            "no pin cards in a recording made with pins"
        seek = ("(s => {{ s.value = {}; s.dispatchEvent(new Event('input'));"
                " }})(document.getElementById('seek'))")
        browser.eval(seek.format(1000))
        assert browser.wait(f"/^[1-9]/.test({pin_row('buffers/s')}) && "
                            f"/^[0-9]+/.test({pin_row('rendered')})"), \
            "a recording's pin cards have no values"
        browser.eval(seek.format(0))
        pad_card = ("[...document.querySelectorAll('.pin-card')].find(c => "
                    "c.querySelector('.pin-title').textContent.includes(':'))")
        assert browser.wait(f"{pad_card}.querySelector('dl').hidden && "
                            f"{pad_card}.textContent.includes('waiting')"), \
            "a pad card shows values before the first were recorded"
        assert browser.eval("!document.querySelector('.pin-card .pin-from')"
                            "?.offsetParent"), "path measuring in a recording"
        # A recording's cards fold but are not removed: they could not be
        # pinned again there.
        assert browser.eval("![...document.querySelectorAll('.pin-card "
                            ".pin-close, .pin-card .pin-prop-close')].some("
                            "b => b.offsetParent) && !!document.querySelector("
                            "'.pin-card .pin-fold')?.offsetParent"), \
            "a recording's card or property can be removed, or not folded"
        browser.click("#viewing-close")
        assert browser.wait(f"{banner}.hidden && {cards}.length === 0"), \
            "pin cards stayed after Close"
        # Added files work as anywhere else.
        browser.set_file("#file-input", recorded)
        open_added(browser, os.path.basename(recorded))
        assert browser.wait(f"!{banner}.hidden"), "an added file did not open"
        browser.click("#viewing-close")
        assert browser.eval(f"{added_row(os.path.basename(recorded))}"
                            ".querySelector('.rec-remove').title === 'Remove "
                            "from the list (the file stays on your computer)'"), \
            "a file from this computer: wrong Remove tooltip"
        assert browser.wait(KINDS_ALIGNED), "the kind column does not line up"

        # Several files at once: the recordings added in the chosen order
        # (the first on top), the others named in one message.
        with open(recorded, encoding="utf-8") as f:
            text = f.read()
        several = []
        for name, content in (("several-a.jsonl", text),
                              ("several-b.jsonl", text),
                              ("several-c.txt", "not a recording\n")):
            several.append(os.path.join(site, name))
            with open(several[-1], "w", encoding="utf-8") as f:
                f.write(content)
        browser.eval(f"{error}.hidden = true")
        browser.set_file("#file-input", several)
        assert browser.wait(f"!!{added_row('several-a.jsonl')} && "
                            f"!!{added_row('several-b.jsonl')}"), \
            "not every recording chosen together was added"
        assert browser.eval(
            "(r => r.indexOf('several-a.jsonl') < r.indexOf('several-b.jsonl'))"
            "([...document.querySelectorAll('#recordings-rows .rec-row.added')]"
            ".map(x => x.dataset.file))"), "not in the chosen order"
        assert browser.wait(f"!{error}.hidden && {error}.textContent === "
                            "'Not recordings this page can open: "
                            "several-c.txt.'"), \
            "the file that is not a recording was not named"
        # Dropped together: both added.
        browser.eval("(() => { const d = new DataTransfer(); "
                     f"for (const n of ['drop-a.jsonl', 'drop-b.jsonl']) "
                     f"d.items.add(new File([{json.dumps(text)}], n)); "
                     "window.dispatchEvent(new DragEvent('drop', "
                     "{dataTransfer: d, cancelable: true})); })()")
        assert browser.wait(f"!!{added_row('drop-a.jsonl')} && "
                            f"!!{added_row('drop-b.jsonl')}"), \
            "not every dropped file was added"

        # A link to a recording: added and shown.
        browser.call("Page.navigate", url="about:blank")
        browser.call("Page.navigate", url=f"{base}/pipeline-monitor/#file="
                     + urllib.parse.quote(f"{base}/shared.jsonl", safe=""))
        assert browser.wait(f"!{banner}.hidden && "
                            f"!!{added_row('shared.jsonl')}"), \
            "a recording from a link did not open"
        assert browser.eval(f"{added_row('shared.jsonl')}"
                            ".querySelector('.rec-remove').title === "
                            "'Remove from the list'"), \
            "a file from a link says it stays on your computer"
        # A link that cannot be read: said so.
        browser.call("Page.navigate", url="about:blank")
        browser.call("Page.navigate", url=f"{base}/pipeline-monitor/#file="
                     + urllib.parse.quote(f"{base}/missing.jsonl", safe=""))
        assert browser.wait(f"!{error}.hidden && "
                            f"{error}.textContent.includes('drop it')"), \
            "an unreadable link said nothing"
        assert browser.eval(f"{banner}.hidden")

        # Damaged recordings: each opens (and plays) or is reported; the
        # page never fails and stays usable.
        fixture = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "data", "recording-0.10.0.jsonl")
        with open(fixture, encoding="utf-8") as f:
            good = f.read()
        browser.call("Page.navigate", url="about:blank")
        browser.call("Page.navigate", url=f"{base}/pipeline-monitor/")
        assert browser.wait(f"!!{demo}")
        for n, text in enumerate(damaged_recordings(good, 40)):
            name = f"damaged-{n}.jsonl"
            browser.eval(f"{error}.hidden = true")
            browser.eval("(() => { const d = new DataTransfer(); "
                         f"d.items.add(new File([{json.dumps(text)}], "
                         f"{json.dumps(name)})); window.dispatchEvent("
                         "new DragEvent('drop', {dataTransfer: d, "
                         "cancelable: true})); })()")
            assert browser.wait(f"!!{added_row(name)} || !{error}.hidden"), \
                f"{name}: neither added nor refused"
            if browser.eval(f"!{added_row(name)}"):
                continue
            open_panel(browser, "recordings")
            browser.click_point(f"{added_row(name)}.querySelector('.rec-open')")
            assert browser.wait(f"!{banner}.hidden || !{error}.hidden"), \
                f"{name}: neither shown nor reported"
            for at in (0, 500, 1000):
                if browser.eval(f"{banner}.hidden"):
                    break
                browser.eval(seek.format(at))
            if not browser.eval(f"{banner}.hidden"):
                browser.click("#viewing-close")
            assert not browser.errors, \
                f"{name}: errors in the page: {browser.errors}\n{text[:2000]}"
        # Still usable: the good recording opens (its row is below the 40
        # added ones: scrolled to first).
        open_panel(browser, "recordings")
        browser.eval(f"{demo}.scrollIntoView({{block: 'center'}})")
        browser.click_point(f"{demo}.querySelector('.rec-open')")
        assert browser.wait(f"!{banner}.hidden"), \
            "the page did not recover from damaged recordings"
        browser.eval("document.querySelector('#empty-moment:not([hidden]) "
                     "button')?.click()")
        assert browser.wait(f"!!{element_selector('q-r')}"), \
            "no pipeline after the damaged recordings"
        browser.click("#viewing-close")

        # The guide (built where the markdown package is installed): linked
        # from the site's viewer, its pages and pictures there, and a way
        # back.
        if guide:
            check_guide(browser, f"{base}/pipeline-monitor/")

        # The site never tries to connect to a server.
        assert browser.eval("(window.socketLog || []).length === 0"), \
            f"the site tried to connect: {browser.history()}"
        assert not browser.errors, f"errors in the page: {browser.errors}"
    finally:
        httpd.shutdown()
        httpd.server_close()
        shutil.rmtree(site)
        os.remove(recorded)
        os.remove(pinned)


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
        assert browser.wait(f"!{banner}.hidden"), \
            "recording not opened in the command's viewer"
        # It may start before its pipeline: go there.
        browser.eval("document.querySelector('#empty-moment:not([hidden]) "
                     "button')?.click()")
        assert browser.wait(f"!!{element_selector('q-r')}"), \
            "no pipeline in the command's viewer"

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
        assert browser.wait(f"!{banner}.hidden", timeout=10), \
            "open FILE did not show it"
        browser.eval("document.querySelector('#empty-moment:not([hidden]) "
                     "button')?.click()")
        assert browser.wait(f"!!{element_selector('q-r')}"), \
            "no pipeline in the file opened by the command"
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
        close_details(browser)
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
        close_details(browser)
        select("q-r")
        assert browser.wait(measured, timeout=8), "queue not measured " \
            "after the restart"
        for name in ("q-r", "i-r"):
            close_details(browser)
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
    # The pipeline is live: its PTS is how long the test has run.
    assert browser.wait(
        f"/^[0-9]+:[0-9][0-9]:[0-9][0-9]\\.[0-9]{{9}}$/.test({pin_row('PTS')})"), \
        f"PTS: {browser.eval(pin_row('PTS'))}"
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
    # A recording running: its time and size in the header.
    open_panel(browser, "recordings")
    browser.wait("!document.getElementById('record').disabled")
    browser.click("#record")
    browser.wait("!!document.querySelector('#recordings-counter .count')")
    close_panel(browser, "recordings")
    # "Measure all" on: a latency label under every element.
    browser.click("#measure-all")
    browser.wait("document.querySelectorAll('#graph .latency-text').length "
                 ">= 5", timeout=6)
    # The video source's pad pinned, folded to its summary line.
    src = browser.eval(f"{element_selector('videotestsrc0')}?.dataset.id")
    browser.click_until(f'#graph .node[data-id="{src}"] .pad[data-port]',
                        "document.querySelectorAll('.pin-card').length === 1")
    browser.click(".pin-card .pin-fold")
    browser.wait("document.querySelector('.pin-card')"
                 ".classList.contains('folded')")
    # The audio queue's details: its level, latency and properties.
    queue = browser.eval(f"{element_selector('queue3')}?.dataset.id")
    section = "document.querySelector('#details .latency-section')"
    browser.click_until(f'#graph .node[data-id="{queue}"] > rect.box',
                        f"/ms avg/.test({section}?.textContent)")
    # Smaller and to the left, so the queue is not under the panel.
    browser.click("#zoom-out")
    for kind, x in (("mousePressed", 700), ("mouseMoved", 620),
                    ("mouseMoved", 590), ("mouseReleased", 590)):
        browser.call("Input.dispatchMouseEvent", type=kind, x=x, y=120,
                     button="left", buttons=1, clickCount=1)
    time.sleep(4)
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
            check_site(browser, serve)
            check_two_pipelines(browser, serve)
            check_format_finding(browser, serve)
            check_pull_mode(browser, serve)
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
