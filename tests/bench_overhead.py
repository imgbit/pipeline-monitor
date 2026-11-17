#!/usr/bin/env python3
"""Measures what pipeline-monitor costs a pipeline, and how fast the page
hears about changes.

Each run starts a pipeline for a while, then stops it, and reports the
process's CPU time (user + sys), its peak memory, and, with a page, the
bytes the page received. A page is a WebSocket client in this script, so
the browser's own work is not counted.

  bench_overhead.py [--seconds 30] [--runs 5] [--out results.json]

Needs gst-launch-1.0 and Python with GStreamer (gi) on PATH/PYTHONPATH,
and GST_PLUGIN_PATH pointing at the built plugin.
"""

import argparse
import base64
import json
import os
import signal
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

PORT = 8790

VIDEO = ("videotestsrc is-live=true ! "
         "video/x-raw,width=640,height=480,framerate=30/1")
PIPELINES = {
    "simple": f"{VIDEO} ! videoconvert ! fakesink sync=true",
    # 3 + 8 x 6 elements.
    "busy": f"{VIDEO} ! tee name=t " + " ".join(
        "t. ! queue ! videoconvert ! videoscale ! "
        "video/x-raw,width=320,height=240 ! identity ! fakesink sync=true"
        for _ in range(8)),
    # An element added, then removed, every 0.5 s (see CHANGING).
    "changing": None,
}

# Run as its own process: the "changing" pipeline. Prints the wall-clock
# time of each change (monotonic clocks differ between processes on macOS), so the page side can measure how late it heard.
CHANGING = r"""
import sys, time, gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib
Gst.init(None)
pipe = Gst.parse_launch(sys.argv[1])
pipe.set_state(Gst.State.PLAYING)
loop = GLib.MainLoop()
n = [0]
extra = [None]
def step():
    if extra[0] is None:
        n[0] += 1
        e = Gst.ElementFactory.make("identity", f"x{n[0]}")
        at = time.time()
        pipe.add(e)
        e.sync_state_with_parent()
        extra[0] = e
        print(f"add x{n[0]} {at:.6f}", flush=True)
    else:
        e = extra[0]
        at = time.time()
        e.set_state(Gst.State.NULL)
        pipe.remove(e)
        extra[0] = None
        print(f"remove x{n[0]} {at:.6f}", flush=True)
    return True
GLib.timeout_add(500, step)
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 2, lambda: loop.quit() or False)
loop.run()
pipe.set_state(Gst.State.NULL)
"""


class Page(threading.Thread):
    """A WebSocket client that reads everything the server sends."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.bytes = 0
        self.messages = {}
        # (wall-clock time, graph text) for each graph message.
        self.graphs = []
        self.stop = False

    def connect(self, timeout=10):
        end = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port),
                                                     timeout=2)
                break
            except OSError:
                if time.monotonic() > end:
                    raise
                time.sleep(0.05)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            f"Origin: http://127.0.0.1:{self.port}\r\n\r\n".encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.sock.recv(1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError(head.decode(errors="replace"))
        self.sock.settimeout(0.5)
        self.start()

    def read(self, n):
        data = b""
        while len(data) < n:
            try:
                chunk = self.sock.recv(n - len(data))
            except socket.timeout:
                if self.stop:
                    raise EOFError
                continue
            if not chunk:
                raise EOFError
            data += chunk
        return data

    def send_frame(self, opcode, payload):
        mask = os.urandom(4)
        head = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head += bytes([0x80 | n])
        else:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(head + mask + body)

    def run(self):
        parts = b""
        try:
            while True:
                b0, b1 = self.read(2)
                n = b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self.read(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self.read(8))[0]
                payload = self.read(n)
                self.bytes += 2 + n
                opcode = b0 & 0x0F
                if opcode == 9:
                    self.send_frame(10, payload)
                    continue
                if opcode == 8:
                    break
                parts += payload
                if not b0 & 0x80:
                    continue
                self.message(parts, time.time())
                parts = b""
        except (EOFError, OSError):
            pass

    def message(self, data, at):
        text = data.decode(errors="replace")
        try:
            kind = json.loads(text).get("type", "?")
        except ValueError:
            kind = "?"
        self.messages[kind] = self.messages.get(kind, 0) + 1
        if kind == "graph":
            self.graphs.append((at, text))

    def close(self):
        self.stop = True
        try:
            self.sock.close()
        except OSError:
            pass


def env_for(tracer):
    env = dict(os.environ)
    env.pop("GST_TRACERS", None)
    if tracer:
        env["GST_TRACERS"] = f"pipeline-monitor(port={PORT})"
        # One line per graph built (pm_model_to_json), counted below.
        env["G_MESSAGES_DEBUG"] = "pipeline-monitor"
    return env


def command(name):
    if name == "changing":
        return [sys.executable, "-c", CHANGING,
                f"{VIDEO} ! tee name=t t. ! queue ! fakesink sync=true"]
    return ["gst-launch-1.0", "-q"] + PIPELINES[name].split()


def change_delays(changes, graphs):
    """For each change, the time until the first graph that shows it."""
    delays = []
    for kind, name, at in changes:
        tag = f'"{name}"'
        for got, text in graphs:
            if got < at:
                continue
            if (tag in text) == (kind == "add"):
                delays.append((got - at) * 1000)
                break
    return delays


def run_once(name, tracer, page, seconds):
    proc = subprocess.Popen(command(name), env=env_for(tracer),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    changes = []
    built = [0]

    # GLib writes debug lines ("graph built") to stdout.
    def read_changes():
        for line in proc.stdout:
            if "graph built" in line:
                built[0] += 1
                continue
            parts = line.split()
            if len(parts) == 3 and parts[0] in ("add", "remove"):
                changes.append((parts[0], parts[1], float(parts[2])))
    reader = threading.Thread(target=read_changes, daemon=True)
    reader.start()
    # Read while it runs: a full pipe would stall the program.
    err_lines = []
    err_reader = threading.Thread(
        target=lambda: err_lines.extend(proc.stderr), daemon=True)
    err_reader.start()

    client = None
    if page:
        client = Page(PORT)
        client.connect()
    time.sleep(seconds)
    proc.send_signal(signal.SIGINT)
    try:
        _, status, usage = os.wait4(proc.pid, 0)
    except ChildProcessError:
        raise
    proc.returncode = os.waitstatus_to_exitcode(status) \
        if hasattr(os, "waitstatus_to_exitcode") else status
    reader.join(2)
    err_reader.join(5)
    err = "".join(err_lines)
    if client:
        client.close()
        client.join(2)
    if tracer and "pipeline-monitor: http" not in err:
        sys.exit(f"{name}: the tracer did not start:\n{err[-500:]}")
    if not tracer and "pipeline-monitor" in err:
        sys.exit(f"{name}: the tracer ran in an 'off' run")
    rss = usage.ru_maxrss if sys.platform == "darwin" \
        else usage.ru_maxrss * 1024
    result = {
        "cpu_s": usage.ru_utime + usage.ru_stime,
        "rss_mb": rss / 1e6,
        "graphs_built_per_s": built[0] / seconds,
        "errors": [l for l in err.splitlines()
                   if "pipeline-monitor" in l and "http" not in l][:3],
    }
    if client:
        result["kb_per_s"] = client.bytes / 1000 / seconds
        result["messages"] = client.messages
        if changes:
            delays = change_delays(changes, client.graphs)
            result["changes"] = len(changes)
            result["seen"] = len(delays)
            result["delay_ms"] = delays
    return result


def summary(results):
    def med(key):
        values = [r[key] for r in results if key in r]
        return statistics.median(values) if values else None

    out = {"runs": len(results), "cpu_s": med("cpu_s"),
           "cpu_s_min": min(r["cpu_s"] for r in results),
           "cpu_s_max": max(r["cpu_s"] for r in results),
           "rss_mb": med("rss_mb"), "kb_per_s": med("kb_per_s"),
           "graphs_built_per_s": med("graphs_built_per_s")}
    delays = [d for r in results for d in r.get("delay_ms", [])]
    if delays:
        delays.sort()
        out["changes"] = sum(r.get("changes", 0) for r in results)
        out["seen"] = len(delays)
        out["delay_ms_median"] = statistics.median(delays)
        out["delay_ms_p95"] = delays[int(len(delays) * 0.95) - 1]
        out["delay_ms_max"] = delays[-1]
        out["delay_ms_min"] = delays[0]
        out["delay_ms_p5"] = delays[max(0, int(len(delays) * 0.05) - 1)]
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--pipelines", default="simple,busy,changing")
    parser.add_argument("--cases", default="off,no page,page")
    parser.add_argument("--label", default="")
    parser.add_argument("--out")
    args = parser.parse_args()

    cases = [c for c in (("off", False, False), ("no page", True, False),
                         ("page", True, True))
             if c[0] in args.cases.split(",")]
    report = {"label": args.label, "seconds": args.seconds, "results": {}}
    for name in args.pipelines.split(","):
        for case, tracer, page in cases:
            results = []
            # Interleaving would be fairer to a busy machine, but each case
            # in a row keeps the port free between runs.
            for _ in range(args.runs):
                results.append(run_once(name, tracer, page, args.seconds))
                time.sleep(1)
            s = summary(results)
            s["raw"] = results
            report["results"][f"{name} / {case}"] = s
            line = (f"{name:9} {case:8} cpu {s['cpu_s']:.2f} s "
                    f"({s['cpu_s_min']:.2f}-{s['cpu_s_max']:.2f})  "
                    f"rss {s['rss_mb']:.1f} MB")
            if s.get("graphs_built_per_s"):
                line += f"  graphs built {s['graphs_built_per_s']:.2f}/s"
            if s.get("kb_per_s") is not None:
                line += f"  page {s['kb_per_s']:.1f} kB/s"
            if "delay_ms_median" in s:
                line += (f"  change->graph min {s['delay_ms_min']:.0f} p5 "
                         f"{s['delay_ms_p5']:.0f} median "
                         f"{s['delay_ms_median']:.0f}"
                         f" p95 {s['delay_ms_p95']:.0f} max "
                         f"{s['delay_ms_max']:.0f} ms, seen {s['seen']}/"
                         f"{s['changes']}")
            errors = {e for r in results for e in r["errors"]}
            if errors:
                line += f"  errors: {sorted(errors)}"
            print(line, flush=True)
    if args.out:
        with open(args.out, "w") as f:
            json.dump(report, f, indent=1)


if __name__ == "__main__":
    main()
