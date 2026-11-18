#!/usr/bin/env python3
"""Pressure tests, run by hand before a release: the tracer stays correct
and its memory flat when pushed hard.

  large      a pipeline of about 300 elements: graph size, how long the
             server takes to build it, and a page drawing it
  pages      ten pages at once on a pipeline for a while: the cost per page
  churn      a branch added and removed every 100 ms: memory stays flat,
             nothing is left behind
  recording  a recording of a busy pipeline: how fast it grows, and that it
             reads back

  pressure.py [large,pages,churn,recording] [--seconds 60]

Needs gst-launch-1.0, Python with GStreamer (gi) and GST_PLUGIN_PATH as
for bench_overhead.py; "large" also needs Chrome (as test_ui.py).
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_overhead import Page  # noqa: E402

PORT = 8791
LIVE = "videotestsrc is-live=true ! video/x-raw,width=320,height=240"


def env_for(**options):
    env = dict(os.environ)
    text = ",".join(f"{k}={v}" for k, v in {"port": PORT, **options}.items())
    env["GST_TRACERS"] = f"pipeline-monitor({text})"
    return env


def get(path):
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/{path}",
                                timeout=30) as r:
        return r.read()


def wait_server(timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            get("api/hello")
            return
        except OSError:
            time.sleep(0.1)
    sys.exit("the tracer's server did not start")


def rss_mb(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    return int(out) / 1024 if out else 0.0


def cpu_s(pid):
    """User + system CPU seconds of a running process (ps, not exact)."""
    out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    parts = [float(p) for p in out.replace("-", ":").split(":")]
    seconds = 0.0
    for p in parts:
        seconds = seconds * 60 + p
    return seconds


def stop(proc):
    proc.send_signal(signal.SIGINT)
    try:
        return proc.wait(15)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.wait()


def launch(description, **options):
    return subprocess.Popen(["gst-launch-1.0", "-q"] + description.split(),
                            env=env_for(**options), stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)


def count_elements(graph):
    def walk(e):
        return 1 + sum(walk(c) for c in e["children"])
    return sum(walk(p) - 1 for p in graph["pipelines"])


def large(args):
    branches = 60
    description = f"{LIVE} ! tee name=t " + " ".join(
        "t. ! queue ! videoconvert ! videoscale ! "
        "video/x-raw,width=160,height=120 ! fakesink sync=true"
        for _ in range(branches))
    proc = launch(description)
    try:
        wait_server()
        time.sleep(3)
        start = time.monotonic()
        body = get("api/graph")
        took = time.monotonic() - start
        graph = json.loads(body)
        n = count_elements(graph)
        print(f"large: {n} elements, graph {len(body) / 1024:.0f} KiB, "
              f"served in {took * 1000:.0f} ms, memory {rss_mb(proc.pid):.0f} "
              "MB")
        assert n >= branches * 5, f"only {n} elements in the graph"
        drawn = draw(n)
        if drawn is not None:
            print(f"large: the page drew {n} elements in {drawn:.1f} s")
    finally:
        stop(proc)


def draw(n):
    """Seconds a page takes to draw n elements; None without Chrome."""
    try:
        import test_ui
        chrome = test_ui.find_chrome()
    except (ImportError, OSError, SystemExit):
        return None
    if not chrome:
        return None
    browser = test_ui.Browser(chrome, tempfile.mkdtemp())
    try:
        start = time.monotonic()
        browser.call("Page.navigate", url=f"http://127.0.0.1:{PORT}/")
        if not browser.wait("document.querySelectorAll('#graph .node')"
                            f".length >= {n}", timeout=60):
            sys.exit("large: the page did not draw every element")
        took = time.monotonic() - start
        assert not browser.errors, f"page errors: {browser.errors}"
        return took
    finally:
        browser.close()


def pages(args):
    proc = launch(f"{LIVE} ! queue ! videoconvert ! fakesink sync=true")
    try:
        wait_server()
        time.sleep(2)
        alone0 = cpu_s(proc.pid)
        time.sleep(args.seconds / 2)
        alone = (cpu_s(proc.pid) - alone0) / (args.seconds / 2)
        clients = [Page(PORT) for _ in range(10)]
        for c in clients:
            c.connect()
        with0 = cpu_s(proc.pid)
        time.sleep(args.seconds / 2)
        with_pages = (cpu_s(proc.pid) - with0) / (args.seconds / 2)
        for c in clients:
            c.close()
        got = [sum(c.messages.values()) for c in clients]
        print(f"pages: CPU {alone * 100:.1f} % of a core without pages, "
              f"{with_pages * 100:.1f} % with 10; each page got "
              f"{min(got)}-{max(got)} messages, "
              f"{sum(c.bytes for c in clients) / 10 / 1024 / (args.seconds / 2):.1f}"
              " KiB/s")
        assert min(got) > 0, "a page got nothing"
    finally:
        stop(proc)


CHURN = r"""
import sys, gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib
Gst.init(None)
pipe = Gst.parse_launch(sys.argv[1])
# Messages left on the bus keep their elements alive: read them, as an
# application should.
pipe.get_bus().add_signal_watch()
pipe.set_state(Gst.State.PLAYING)
tee = pipe.get_by_name("t")
branch = []
def toggle():
    if not branch:
        q = Gst.ElementFactory.make("queue")
        s = Gst.ElementFactory.make("fakesink")
        s.set_property("async", False)
        pipe.add(q); pipe.add(s); q.link(s)
        s.sync_state_with_parent(); q.sync_state_with_parent()
        pad = tee.request_pad_simple("src_%u")
        pad.link(q.get_static_pad("sink"))
        branch[:] = [q, s, pad]
    else:
        q, s, pad = branch
        tee.release_request_pad(pad)
        for e in (q, s):
            e.set_state(Gst.State.NULL)
            pipe.remove(e)
        branch.clear()
    return True
GLib.timeout_add(100, toggle)
loop = GLib.MainLoop()
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 2, lambda: loop.quit() or False)
loop.run()
pipe.set_state(Gst.State.NULL)
"""


def churn(args):
    env = env_for()
    env["PM_LEFTOVER_GRACE_MS"] = "2000"
    proc = subprocess.Popen([sys.executable, "-c", CHURN,
                             f"{LIVE} ! tee name=t ! queue ! fakesink"],
                            env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        wait_server()
        page = Page(PORT)
        page.connect()
        samples = []
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            time.sleep(5)
            samples.append(rss_mb(proc.pid))
        findings = json.loads(get("api/findings"))["findings"]
        left = [f["text"] for f in findings if f["id"].startswith("leftovers")]
        graph = json.loads(get("api/graph"))
        page.close()
        first = sum(samples[1:4]) / 3
        last = sum(samples[-3:]) / 3
        print(f"churn: {int(args.seconds * 10)} branches added and removed; "
              f"memory {first:.0f} -> {last:.0f} MB "
              f"({samples}); {count_elements(graph)} elements at the end; "
              f"leftovers: {left or 'none'}; page got "
              f"{page.messages.get('graph', 0)} graphs")
        assert not left, "removed branches left behind"
        assert last < first * 1.2 + 5, "memory keeps growing"
    finally:
        stop(proc)


def recording(args):
    folder = tempfile.mkdtemp()
    proc = launch("videotestsrc is-live=true ! tee name=t " + " ".join(
        "t. ! queue ! videoconvert ! fakesink sync=true" for _ in range(10)),
        recordings=folder, record="true")
    try:
        wait_server()
        page = Page(PORT)
        page.connect()
        time.sleep(args.seconds)
        page.close()
    finally:
        stop(proc)
    files = [os.path.join(folder, f) for f in os.listdir(folder)]
    assert len(files) == 1, f"recordings: {files}"
    size = os.path.getsize(files[0])
    with open(files[0]) as f:
        lines = [json.loads(line) for line in f]
    per_s = size / args.seconds
    print(f"recording: {size / 1024:.0f} KiB in {args.seconds:.0f} s "
          f"({per_s / 1024:.1f} KiB/s, {len(lines)} lines, all JSON, last "
          f"\"{lines[-1].get('type')}\"); 100 MB in about "
          f"{100 * 1024 * 1024 / per_s / 3600:.1f} h")
    assert lines[-1].get("type") == "end", "the recording has no end"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tests", nargs="?",
                        default="large,pages,churn,recording")
    parser.add_argument("--seconds", type=float, default=60)
    args = parser.parse_args()
    for name in args.tests.split(","):
        {"large": large, "pages": pages, "churn": churn,
         "recording": recording}[name](args)
    print("pressure: done")


if __name__ == "__main__":
    main()
