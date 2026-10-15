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
                 "split. ! queue ! fakesink")
NODES = "document.querySelectorAll('#graph .node').length"


def find_chrome():
    candidates = [os.environ.get("CHROME"),
                  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                  "/Applications/Chromium.app/Contents/MacOS/Chromium"]
    candidates += [shutil.which(n) for n in
                   ("google-chrome", "google-chrome-stable", "chromium",
                    "chromium-browser", "chrome")]
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
        self.proc = subprocess.Popen(
            [chrome, "--headless=new", f"--remote-debugging-port={port}",
             f"--user-data-dir={profile}", "--hide-scrollbars",
             "--no-first-run", "--window-size=1400,800", "about:blank"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        page = None
        for _ in range(100):
            try:
                targets = json.load(urllib.request.urlopen(
                    f"http://127.0.0.1:{port}/json/list"))
                page = next(t for t in targets if t["type"] == "page")
                break
            except Exception:
                time.sleep(0.1)
        assert page, "Chrome did not start"
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

    def wait(self, expression, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if self.eval(expression):
                return True
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

    def screenshot(self, path):
        data = self.call("Page.captureScreenshot")["data"]
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))

    def close(self):
        self.ws.close()
        self.proc.terminate()
        self.proc.wait()


def start_server(serve, pipeline):
    env = dict(os.environ, GST_TRACERS="pipeline-monitor(port=0)")
    proc = subprocess.Popen([serve, pipeline], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            text=True)
    found = {}

    def read():
        for line in proc.stderr:
            m = re.match(r"pipeline-monitor: (http://\S+)", line)
            if m and "url" not in found:
                found["url"] = m.group(1)

    threading.Thread(target=read, daemon=True).start()
    end = time.time() + 10
    while "url" not in found and time.time() < end:
        time.sleep(0.05)
    assert "url" in found, "server did not report its address"
    return proc, found["url"]


def element_selector(name):
    return ("[...document.querySelectorAll('#graph .node')]"
            f".find(n => n.textContent.startsWith({json.dumps(name)}))")


def check(browser, url):
    browser.call("Page.navigate", url=url)
    assert browser.wait(f"{NODES} > 0"), "graph was not drawn"
    assert browser.eval("document.getElementById('connection').textContent") \
        == "connected"
    assert browser.eval("document.getElementById('empty').hidden")

    mq = browser.eval(f"{element_selector('multiqueue0')}?.dataset.id")
    assert mq, "multiqueue0 not drawn"
    browser.click(f'#graph .node[data-id="{mq}"] > rect.box')
    assert browser.wait("document.querySelector('#details h2')?.textContent"
                        " === 'multiqueue0'"), "details panel did not open"

    browser.click('.toggle[data-path$="parsebin0"] rect')
    assert browser.wait("!!document.querySelector('#graph .node.collapsed')")
    assert not browser.eval(f"!!{element_selector('typefind')}"), \
        "children of a collapsed bin are still drawn"

    browser.call("Page.reload")
    assert browser.wait("!!document.querySelector('#graph .node.collapsed')"), \
        "collapsed bin not remembered after reload"
    browser.click('.toggle[data-path$="parsebin0"] rect')
    assert browser.wait("!document.querySelector('#graph .node.collapsed')")

    counts = set()
    end = time.time() + 5
    while time.time() < end:
        counts.add(browser.eval(NODES))
        time.sleep(0.2)
    assert len(counts) > 1, f"graph did not update live: {counts}"

    browser.call("Emulation.setDeviceMetricsOverride", width=390, height=760,
                 deviceScaleFactor=1, mobile=True)
    time.sleep(0.3)
    assert not browser.eval("document.documentElement.scrollWidth > innerWidth")
    browser.call("Emulation.clearDeviceMetricsOverride")

    assert not browser.errors, f"errors in the page: {browser.errors}"


def screenshot(browser, url, path):
    browser.call("Emulation.setDeviceMetricsOverride", width=1280, height=560,
                 deviceScaleFactor=2, mobile=False)
    browser.call("Emulation.setEmulatedMedia", features=[
        {"name": "prefers-color-scheme", "value": "light"}])
    browser.call("Page.navigate", url=url)
    assert browser.wait(f"{NODES} > 0")
    mq = browser.eval(f"{element_selector('multiqueue0')}?.dataset.id")
    browser.click(f'#graph .node[data-id="{mq}"] > rect.box')
    browser.wait("!!document.querySelector('#details h2')")
    time.sleep(2.5)
    browser.screenshot(path)


def main():
    serve = sys.argv[1]
    shot = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--screenshot" \
        else None
    chrome = find_chrome()
    if chrome is None:
        print("Chrome not found, skipping")
        return 77

    profile = tempfile.mkdtemp(prefix="pm-ui-test-")
    server, url = start_server(serve, SHOT_PIPELINE if shot else PIPELINE)
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
