#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""pipeline-monitor status: against a running program (the serve example
with the tracer) and against a recording.

Usage: test_status.py SERVE TOOL
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time


def status(tool, *args):
    r = subprocess.run([tool, "status", *args], capture_output=True,
                       timeout=60)
    # Windows ends lines with \r\n.
    return (r.returncode, r.stdout.decode("utf-8").replace("\r\n", "\n"),
            r.stderr.decode("utf-8").replace("\r\n", "\n"))


def start(serve, pipeline):
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
    end = time.time() + 60
    while "url" not in found and time.time() < end:
        time.sleep(0.05)
    assert "url" in found, "the program did not report its address"
    return proc, found["url"]


def check(condition, what, output):
    if not condition:
        print(f"FAILED: {what}\n--- output ---\n{output}")
        sys.exit(1)


def live(serve, tool):
    proc, url = start(serve, "videotestsrc is-live=true ! "
                      "queue name=q max-size-buffers=2 ! fakesink sync=true")
    try:
        # The first performance sample comes a second in.
        end = time.time() + 20
        while True:
            code, out, err = status(tool, "--url", url)
            if "PLAYING" in out and "Process: CPU" in out or \
                    time.time() > end:
                break
            time.sleep(0.25)
        check(code == 0, f"exit 0, not {code} ({err})", out)
        for text in ("PLAYING", "live", "elements", "Process: CPU",
                     "Problems: none found", "Log: no errors or warnings"):
            check(text in out, f"summary has {text!r}", out)

        code, out, _ = status(tool, "--url", url, "--element", "q")
        check(code == 0, "--element exits 0", out)
        for text in ("(queue, GstQueue) PLAYING", "src -> ",
                     "buffers ", " / 2,", "max-size-buffers = 2"):
            check(text in out, f"--element has {text!r}", out)

        code, out, _ = status(tool, "--url", url, "--element", "nothing")
        check(code == 2, "an unknown element exits 2", out)

        code, out, _ = status(tool, "--url", url, "--graph")
        check(re.search(r"^  q \(queue\) PLAYING$", out, re.M), "--graph "
              "lists q", out)
        check("video/x-raw" in out, "--graph shows caps", out)

        code, out, _ = status(tool, "--url", url, "--json")
        types = [json.loads(line)["type"] for line in out.splitlines()]
        check(types == ["hello", "graph", "levels", "findings",
                        "performance", "log"], f"--json types {types}", out)
    finally:
        proc.terminate()
        proc.wait()

    code, out, err = status(tool, "--url", url)
    check(code == 3, f"nothing at the address exits 3, not {code}", err)


def pads(serve, tool):
    proc, url = start(serve,
                      "audiotestsrc is-live=true volume=0.5 ! audio/x-raw,"
                      "format=S16LE,channels=2 ! identity name=st ! "
                      "fakesink sync=true "
                      "audiotestsrc is-live=true wave=silence ! audio/x-raw,"
                      "format=F32LE,channels=6,channel-mask=(bitmask)0x3f ! "
                      "identity name=six ! fakesink sync=true "
                      "videotestsrc is-live=true ! identity name=vid ! "
                      "fakesink sync=true")
    try:
        end = time.time() + 20
        while True:
            code, out, err = status(tool, "--url", url, "--pad", "st:src")
            if "Audio:" in out or time.time() > end:
                break
            time.sleep(0.25)
        check(code == 0, f"--pad exits 0, not {code} ({err})", out)
        for text in ("pipeline0/st:src, watched for 2.5 s",
                     "src -> fakesink0:sink", "Flow: ", " buffers/s",
                     "Last buffer: PTS ",
                     "Audio: S16LE, 44.1 kHz, 2 channels\n",
                     "  FL   peak -6.0 dBFS, RMS -9.0 dBFS, clipped 0\n",
                     "  FR   peak -6.0 dBFS, RMS -9.0 dBFS, clipped 0\n"):
            check(text in out, f"--pad has {text!r}", out)

        code, out, _ = status(tool, "--url", url, "--pad",
                              "pipeline0/six:src")
        for text in ("Audio: F32LE, 44.1 kHz, 6 channels (FL FR measured)",
                     "  FL   no signal, clipped 0, silent for ",
                     "(digital silence: every sample 0)"):
            check(text in out, f"--pad of 5.1 has {text!r}", out)

        code, out, _ = status(tool, "--url", url, "--pad", "vid:src")
        check(code == 0 and "Flow: " in out and "Audio" not in out,
              "a video pad has flow and no audio", out)
        check(re.search(r"^Picture: 320x240 \S+; brightness avg \d+ %, max "
                        r"\d+ %; changing$", out, re.M),
              "a video pad has picture facts", out)

        code, out, _ = status(tool, "--url", url, "--pad", "st:src", "--json")
        lines = [json.loads(line) for line in out.splitlines()]
        check([m["type"] for m in lines] == ["pads", "audio"],
              "--pad --json: pads and audio", out)
        audio = lines[1]["audio"][0]
        check(len(audio["levels"][0]["peak"]) >= 5,
              "--pad --json merges the windows", out)

        code, _, err = status(tool, "--url", url, "--pad", "nothing:src")
        check(code == 2 and "no pad nothing:src" in err,
              "an unknown pad exits 2", err)
        code, _, err = status(tool, "--url", url, "--pad", "st:src",
                              "--graph")
        check(code == 2, "--pad with --graph is wrong use", err)
    finally:
        proc.terminate()
        proc.wait()


def recording(tool, folder):
    started = 1_000_000_000_000_000

    def at(s):
        return started + int(s * 1_000_000)

    graph = {"type": "graph", "protocol": 1, "time": at(0.1), "links": [],
             "threads": [], "pipelines": [{
                 "id": 1, "name": "p", "path": "p", "factory": "pipeline",
                 "type": "GstPipeline", "bin": True, "state": "PLAYING",
                 "pads": [], "children": [{
                     "id": 2, "name": "src", "path": "p/src",
                     "factory": "videotestsrc", "type": "GstVideoTestSrc",
                     "bin": False, "state": "PLAYING", "pads": [{
                         "id": 3, "name": "src", "path": "p/src:src",
                         "direction": "src", "presence": "always",
                         "caps": "audio/x-raw, format=(string)S16LE"}],
                     "children": []}]}]}
    error = {"id": "flow:p/src:src", "severity": "error", "element": 2,
             "path": "p/src", "text": "Data stopped at p/src:src: "
             "not-negotiated", "hint": "The formats did not match."}
    lines = [
        {"format": "pipeline-monitor-recording", "version": 1,
         "protocol": 1, "kind": "recording", "started": started,
         "hello": {"version": "0.13.0", "gstreamer": "1.28.0",
                   "program": "player", "pid": 7}},
        graph,
        {"type": "findings", "protocol": 1, "time": at(0.2),
         "findings": []},
        {"type": "pads", "protocol": 1, "time": at(0.5), "pads": [{
            "pad": 3, "pad_path": "p/src:src", "buffers": 20,
            "bytes": 80000, "lists": 0, "list_buffers": 0,
            "last": {"pts": 480000000, "dts": None, "duration": 23000000,
                     "running_time": 480000000, "delay": 2000000,
                     "size": 4000, "offset": None, "offset_end": None,
                     "flags": [], "age": 1000000},
            "segment": None,
            "window": {"seconds": 0.5, "buffers_per_s": 40.0,
                       "bytes_per_s": 160000.0, "gap_min": 20000000,
                       "gap_avg": 25000000, "gap_max": 30000000},
            "events": {"segment": 0, "flush": 0, "eos": 0, "caps": 0,
                       "gap": 0}}]},
        {"type": "findings", "protocol": 1, "time": at(1),
         "findings": [error]},
        {"type": "log", "protocol": 1, "time": at(1), "entries": [
            {"id": 1, "level": "error", "source": "message", "element": 2,
             "path": "p/src", "text": "Internal data stream error.",
             "debug": "basesrc.c(1): loop (): /p/src:\nstreaming stopped, "
             "reason not-negotiated (-4)", "count": 1, "first": at(1),
             "last": at(1)}]},
        {"type": "graph", "protocol": 1, "time": at(1.5), "links": [],
         "threads": [], "pipelines": []},
        {"type": "end", "protocol": 1, "time": at(2), "reason": "stopped"},
    ]
    path = os.path.join(folder, "player.jsonl")
    with open(path, "w") as f:
        for line in lines:
            f.write(json.dumps(line) + "\n")

    code, out, _ = status(tool, path)
    check(code == 1, f"an error exits 1, not {code}", out)
    for text in ("of player (pid 7)", "at 0:00:02.000 of 0:00:02.000",
                 "ended: stopped", "as last seen, at 0:00:00.100",
                 "p  PLAYING", "1 element\n", "error    Data stopped at "
                 "p/src:src: not-negotiated", "The formats did not match.",
                 "Log: 1 error, 0 warnings",
                 "streaming stopped, reason not-negotiated (-4)"):
        check(text in out, f"the recording's summary has {text!r}", out)

    code, out, _ = status(tool, path, "--at", "0.5")
    check(code == 0, f"before the error exits 0, not {code}", out)
    check("Problems: none found" in out, "no problems at 0.5 s", out)

    code, out, _ = status(tool, path, "--log")
    check("error   p/src: Internal data stream error." in out, "--log", out)

    code, out, _ = status(tool, path, "--pad", "src:src")
    for text in ("p/src:src\n", "Flow: 40.0 buffers/s, 156.2 KiB/s; gaps "
                 "20 ms / 25 ms / 30 ms (min / avg / max)",
                 "Last buffer: PTS 0:00:00.480, 3.9 KiB, 2 ms late",
                 "Audio levels: not kept in recordings"):
        check(text in out, f"--pad in a recording has {text!r}", out)
    code, out, _ = status(tool, path, "--pad", "src:src", "--at", "0.3")
    check("Flow: the pad was not watched while recording" in out,
          "--pad before the pad was watched", out)

    code, _, err = status(tool, path, "--at", "soon")
    check(code == 2, "a wrong --at exits 2", err)
    code, _, err = status(tool, os.path.join(folder, "none.jsonl"))
    check(code == 3, "a missing file exits 3", err)
    code, _, err = status(tool, os.path.join(folder, "p.dot"))
    check(code == 2 and "pipeline-monitor open" in err, ".dot points to "
          "open", err)


# A recording with appsink -> appsrc, a multiqueue, a bin, two elements of
# one name and recorded properties: what --element and --graph show.
def details(tool, folder):
    started = 1_000_000_000_000_000

    def at(s):
        return started + int(s * 1_000_000)

    def element(eid, name, path, factory, kind, pads=(), children=(),
                state="PLAYING", is_bin=False):
        return {"id": eid, "name": name, "path": path, "factory": factory,
                "type": kind, "bin": is_bin, "state": state,
                "pads": list(pads), "children": list(children)}

    def pad(pid, owner, name, direction, caps=None, presence="always"):
        p = {"id": pid, "name": name, "path": f"{owner}:{name}",
             "direction": direction, "presence": presence, "ghost": False,
             "mode": "push"}
        if caps:
            p["caps"] = caps
        return p

    def seg(start):
        return {"format": "time", "start": start, "base": 0, "rate": 1}

    video = "video/x-raw, format=(string)I420"
    graph = {"type": "graph", "protocol": 1, "time": at(0.1), "threads": [],
             "links": [{"src": 11, "sink": 21}, {"src": 22, "sink": 31}],
             "pipelines": [
                 element(1, "camera", "camera", "pipeline", "GstPipeline",
                         is_bin=True, children=[
                             element(10, "cam", "camera/cam", "videotestsrc",
                                     "GstVideoTestSrc",
                                     [pad(11, "camera/cam", "src", "src",
                                          video)]),
                             element(20, "mq", "camera/mq", "multiqueue",
                                     "GstMultiQueue",
                                     [pad(21, "camera/mq", "sink_0", "sink",
                                          video, "request"),
                                      pad(22, "camera/mq", "src_0", "src",
                                          video, "sometimes")]),
                             element(30, "out", "camera/out", "appsink",
                                     "GstAppSink",
                                     [pad(31, "camera/out", "sink", "sink",
                                          video)]),
                             element(40, "q", "camera/q", "queue",
                                     "GstQueue")]),
                 element(2, "display", "display", "pipeline", "GstPipeline",
                         is_bin=True, children=[
                             element(50, "in", "display/in", "appsrc",
                                     "GstAppSrc",
                                     [pad(51, "display/in", "src", "src")]),
                             element(60, "box", "display/box", "bin",
                                     "GstBin", is_bin=True, children=[
                                         element(70, "q", "display/box/q",
                                                 "queue", "GstQueue")])])]}
    levels = {"type": "levels", "protocol": 1, "time": at(1), "pipelines": [
        {"element": 1, "path": "camera", "state": "PLAYING",
         "clock": "GstSystemClock", "base_time": 0, "running_time": 1000000000,
         "latency": {"live": True, "min": 20000000, "max": None}}],
        "queues": [
            {"element": 20, "path": "camera/mq", "kind": "multiqueue",
             "limits": {"buffers": 5, "bytes": 0, "time": 2000000000},
             "overruns": 1, "underruns": 2, "buffering": None,
             "levels": [{"pad": 21, "pad_path": "camera/mq:sink_0",
                         "buffers": 5, "bytes": 1000, "time": 1500000000}]},
            {"element": 30, "path": "camera/out", "kind": "appsink",
             "at_limit": "blocks", "leaky": "none",
             "limits": {"buffers": 3, "bytes": None, "time": None},
             "counts": {"in": 9, "out": 6, "dropped": 0},
             "levels": [], "segment": seg(5000000000), "last_pts": 6000000000},
            {"element": 50, "path": "display/in", "kind": "appsrc",
             "at_limit": "signals", "leaky": "downstream",
             "limits": {"buffers": 0, "bytes": 200000, "time": 0},
             "counts": {"in": 9, "out": 9, "dropped": 0},
             "levels": [{"buffers": 0, "bytes": 0, "time": 0}],
             "segment": None,
             "fed_by": [{"element": 30, "path": "camera/out", "samples": 4,
                         "shared": True, "last": at(0.9),
                         "in_pts": 6000000000, "out_pts": 1000000000,
                         "delay": 3000000, "in_segment": seg(5000000000),
                         "out_segment": seg(0)}]}]}
    properties = {"type": "properties", "protocol": 1, "time": at(1),
                  "elements": [{"element": 50, "path": "display/in",
                                "properties": [
        {"name": "format", "type": "GstFormat", "changed": True,
         "value": {"nick": "time", "value": 3}},
        {"name": "caps", "type": "GstCaps", "changed": True,
         "value": {"caps": video}},
        {"name": "parent", "type": "GstObject", "changed": True,
         "value": {"object": "display", "type": "GstPipeline"}},
        {"name": "clock", "type": "GstClock", "changed": True,
         "value": {"object": None}},
        {"name": "uri", "type": "gchararray", "changed": True, "value": None},
        {"name": "name", "type": "gchararray", "changed": True,
         "value": "in"},
        {"name": "block", "type": "gboolean", "changed": True,
         "value": True},
        {"name": "max-bytes", "type": "guint64", "changed": False,
         "value": 200000}]}]}
    lines = [
        {"format": "pipeline-monitor-recording", "version": 1, "protocol": 1,
         "kind": "snapshot", "started": started,
         "hello": {"version": "0.13.0", "gstreamer": "1.28.0",
                   "program": "relay", "pid": 9}},
        graph, levels, properties,
        "not json: skipped",
    ]
    path = os.path.join(folder, "relay.jsonl")
    with open(path, "w") as f:
        for line in lines:
            f.write((line if isinstance(line, str) else json.dumps(line)) +
                    "\n")

    code, out, _ = status(tool, path, "--element", "in")
    check(code == 0, "--element in exits 0", out)
    for text in ("display/in (appsrc, GstAppSrc) PLAYING",
                 "Queue (appsrc)", "at a limit: only signals enough-data",
                 "in 9, out 9, dropped 0", "leaky: downstream",
                 "segment: none yet",
                 "Fed by camera/out (detected from the data, seen 0.1 s "
                 "ago)", "PTS 0:00:06.000 into the appsink, 0:00:01.000 out",
                 "segment from 0:00:05.000 into the appsink, from "
                 "0:00:00.000 out", "the same buffers also went into another",
                 "Properties changed from their defaults", "format = time",
                 "caps = video/x-raw", "parent = display", "clock = none",
                 "uri = none", "block = true"):
        check(text in out, f"--element in has {text!r}", out)
    check("max-bytes" not in out, "unchanged properties left out", out)

    code, out, _ = status(tool, path, "--element", "out")
    for text in ("Queue (appsink)", "level: not available",
                 "at a limit: waits for the program to pull",
                 "segment received: time, from 0:00:05.000",
                 "Feeds display/in (detected from the data)",
                 "Properties: not in the recording"):
        check(text in out, f"--element out has {text!r}", out)

    code, out, _ = status(tool, path, "--element", "mq")
    for text in ("sink_0: buffers 5 / 5", "time 1.5 s / 2 s",
                 "overruns 1, underruns 2", "sink_0 <- cam:src [request]",
                 "src_0 -> out:sink [sometimes]"):
        check(text in out, f"--element mq has {text!r}", out)

    # Two of one name: which ones, and the path picks one.
    code, out, err = status(tool, path, "--element", "q")
    check(code == 2 and "2 elements named q" in err and
          "display/box/q" in err, "two named q: asked for the path", err)
    code, out, _ = status(tool, path, "--element", "box/q")
    check(code == 0 and "display/box/q (queue, GstQueue)" in out,
          "a path's end picks one", out)
    code, out, _ = status(tool, path, "--element", "nothing-here")
    check(code == 2, "an unknown element exits 2", out)

    code, out, _ = status(tool, path, "--graph")
    for text in ("camera (pipeline) PLAYING", "  box (bin) PLAYING",
                 "    q (queue) PLAYING", "src -> mq:sink_0"):
        check(text in out, f"--graph has {text!r}", out)

    code, out, _ = status(tool, path)
    check("queues, fullest first: mq 100 % buffers" in out,
          "the fullest queue first", out)

    code, _, err = status(tool, "--no-such-option")
    check(code == 2, "a wrong option exits 2", err)
    code, _, err = status(tool, path, "--url", "http://127.0.0.1:1/")
    check(code == 2, "a file and --url together exit 2", err)


# --watch prints again every second until stopped.
def watch(serve, tool):
    proc, url = start(serve, "videotestsrc is-live=true ! fakesink")
    try:
        w = subprocess.Popen([tool, "status", "--watch", "--url",
                              url.rstrip("/")], stdout=subprocess.PIPE)
        time.sleep(3.5)
        w.terminate()
        out = w.communicate(timeout=10)[0].decode("utf-8")
        check(out.count("pipeline0  PLAYING") >= 2,
              "--watch prints again (and --url takes no slash)", out)
    finally:
        proc.terminate()
        proc.wait()


def main():
    serve, tool = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory() as folder:
        recording(tool, folder)
        details(tool, folder)
    live(serve, tool)
    pads(serve, tool)
    watch(serve, tool)
    print("status: all checks passed")


if __name__ == "__main__":
    main()
