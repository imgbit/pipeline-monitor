# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Checks the web UI in headless Chrome over the DevTools protocol.

Usage: test_ui.py SERVE_EXECUTABLE [--screenshot FILE]
Exits with 77 (skipped) when Chrome is not available.
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
        port = free_port()
        args = [chrome, "--headless=new", f"--remote-debugging-port={port}",
                f"--user-data-dir={profile}", "--hide-scrollbars",
                "--no-first-run", "--window-size=1400,800"]
        # CI containers and Ubuntu 24.04 runners often block Chrome's sandbox.
        if os.environ.get("CI"):
            args.append("--no-sandbox")
        # Kept so a failed start can say why; CI machines start Chrome slowly.
        self.log = open(os.path.join(profile, "chrome.log"), "wb")
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
            with open(self.log.name, errors="replace") as f:
                output = f.read()[-3000:]
            state = (f"exited with code {code}" if code is not None
                     else "no answer within 30 s")
            raise AssertionError(f"Chrome did not start ({state}):\n{output}")
        self.ws = WebSocket(page["webSocketDebuggerUrl"])
        self.next_id = 0
        self.errors = []
        self.call("Runtime.enable")
        self.call("Page.enable")

    def call(self, method, **params):
        self.next_id += 1
        self.ws.send(json.dumps({"id": self.next_id, "method": method,
                                 "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("method") == "Runtime.exceptionThrown":
                details = msg["params"]["exceptionDetails"]
                self.errors.append(details.get("exception", {})
                                   .get("description", details.get("text")))
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
        assert point, f"nothing matches {selector}"
        for kind in ("mousePressed", "mouseReleased"):
            self.call("Input.dispatchMouseEvent", type=kind, x=point[0],
                      y=point[1], button="left", clickCount=1)

    # The live graph can move between finding an element and clicking it.
    def click_until(self, selector, condition, tries=3):
        for _ in range(tries):
            self.click(selector)
            if self.wait(condition, timeout=3):
                return True
        return False

    # Clicks the element an expression returns, at once (it may not last).
    def click_point(self, expression):
        point = self.eval(
            f"(() => {{ const n = {expression}; if (!n) return null;"
            " const r = n.getBoundingClientRect();"
            " return [r.x + r.width / 2, r.y + r.height / 2]; })()")
        assert point, f"nothing to click: {expression}"
        for kind in ("mousePressed", "mouseReleased"):
            self.call("Input.dispatchMouseEvent", type=kind, x=point[0],
                      y=point[1], button="left", clickCount=1)

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

    def screenshot(self, path):
        data = self.call("Page.captureScreenshot")["data"]
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))

    def close(self):
        self.ws.close()
        self.proc.terminate()
        self.proc.wait()
        self.log.close()


def start_server(serve, pipeline, *args):
    env = dict(os.environ, GST_TRACERS="pipeline-monitor(port=0)")
    proc = subprocess.Popen([serve, *args, pipeline], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            text=True)
    found = {}

    def read():
        for line in proc.stderr:
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
    assert browser.wait(f"{NODES} > 0"), "graph was not drawn"
    assert browser.eval("document.getElementById('connection').textContent") \
        == "connected"
    assert browser.eval("document.getElementById('empty').hidden")

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

    browser.call("Page.reload")
    assert browser.wait("!!document.querySelector('#graph .node.collapsed')"), \
        "collapsed bin not remembered after reload"
    assert browser.click_until(
        '.toggle[data-path$="parsebin0"] rect',
        "!document.querySelector('#graph .node.collapsed')"), \
        "bin did not expand"

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
    check_pins(browser)
    check_paths(browser)

    browser.call("Emulation.setDeviceMetricsOverride", width=390, height=760,
                 deviceScaleFactor=1, mobile=True)
    time.sleep(0.3)
    assert not browser.eval("document.documentElement.scrollWidth > innerWidth")
    browser.call("Emulation.clearDeviceMetricsOverride")

    assert not browser.errors, f"errors in the page: {browser.errors}"


# The server runs "serve --warnings": the tee "t" posts a warning each time
# a branch is added or removed (every two seconds).
def check_log(browser):
    counter = "document.getElementById('log-counter')"
    rows = "document.querySelectorAll('#log-rows .log-row').length"
    tee = element_selector("t")
    badge = f"{tee}?.querySelector('.badge')"

    assert browser.wait(f"{counter}.textContent.includes('warning')"), \
        "warning counter did not appear"
    assert browser.eval(f"{counter}.classList.contains('has-warnings')")
    assert browser.wait(f"!!{badge}"), "no badge on the tee"

    browser.click("#log-counter")
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
    browser.click("#log-counter")
    browser.click("#log-muted .unmute")
    assert browser.wait(f"{rows} > 0"), "unmuting did not bring rows back"

    browser.click("#log-clear")
    assert browser.wait(f"{rows} === 0"), "clear did not empty the panel"
    assert browser.eval(f"{counter}.textContent") == "No errors"
    assert browser.wait(f"{rows} > 0", timeout=6), \
        "new warnings did not appear after clear"
    assert browser.eval("document.querySelector('#log-rows .log-count')"
                        ".textContent") == "", "count did not restart at clear"


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
                        timeout=6), \
        f"no counter rate: {browser.eval(f'{card}?.innerText')!r}"
    sink = browser.eval(f"{element_selector('fakesink0')}?.dataset.id")
    assert browser.eval(f"document.querySelector('#graph .node[data-id="
                        f"\"{sink}\"]').classList.contains('pinned')")
    assert browser.eval(f"{rows}.find(r => r.cells[1]?.textContent === "
                        "'stats').querySelector('.pin-prop.on') !== null")

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
                        f"{auto}?.nextElementSibling.textContent)", timeout=6), \
        "no time from the upstream pinned pad"

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

    for name in ("fakesink0:sink", "queue0:sink"):
        browser.eval(f"{card(name)}.querySelector('.pin-close').click()")
    assert browser.wait("document.querySelectorAll('.pin-card').length === 0")


def pin_row(name):
    return ("[...document.querySelectorAll('.pin-card dt')].find(d => "
            f"d.textContent === {json.dumps(name)})?.nextElementSibling"
            ".textContent")


def watched():
    return ("fetch('/api/monitors').then(r => r.json())"
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
    assert browser.wait(f"!!{extra}", timeout=6), "no extra branch"
    browser.click_point(extra)
    assert browser.wait(f"{cards} === 1"), "could not pin the extra branch"
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
    # The audio sink's properties in the details panel, its "stats" pinned
    # to a card moved to the free space below the graph.
    sink = browser.eval(f"{element_selector('fakesink2')}?.dataset.id")
    rows = "[...document.querySelectorAll('#details table.props tr')]"
    browser.click_until(f'#graph .node[data-id="{sink}"] > rect.box',
                        f"{rows}.some(r => r.cells[1]?.textContent === "
                        "'stats')")
    browser.eval(f"{rows}.find(r => r.cells[1]?.textContent === 'stats')"
                 ".querySelector('.pin-prop').click()")
    browser.wait("!!document.querySelector('.pin-card')")
    # The card opens next to the sink, under the panel: move it with the
    # panel closed, then open the panel again.
    browser.click("#details .close")
    time.sleep(0.5)
    browser.drag(".pin-card .pin-head", -760, 60)
    browser.click_until(f'#graph .node[data-id="{sink}"] > rect.box',
                        f"{rows}.some(r => r.cells[1]?.textContent === "
                        "'stats')")
    time.sleep(0.5)
    browser.eval("document.querySelector('#details').scrollTop = "
                 "document.querySelector('#details h3:last-of-type')"
                 ".offsetTop - 12")
    time.sleep(3)
    browser.screenshot(path)


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
    chrome = find_chrome()
    if chrome is None:
        print("Chrome not found, skipping")
        return 77

    profile = tempfile.mkdtemp(prefix="pm-ui-test-")
    if shot:
        server, url = start_server(serve, SHOT_PIPELINE)
    else:
        server, url = start_server(serve, PIPELINE, "--warnings")
    browser = None
    try:
        browser = Browser(chrome, profile)
        if shot:
            screenshot(browser, url, shot)
        else:
            check(browser, url)
    finally:
        if browser:
            browser.close()
        server.terminate()
        server.wait()
        shutil.rmtree(profile, ignore_errors=True)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
