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
    return r.returncode, r.stdout.decode("utf-8"), r.stderr.decode("utf-8")


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
                     "bin": False, "state": "PLAYING", "pads": [],
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

    code, _, err = status(tool, path, "--at", "soon")
    check(code == 2, "a wrong --at exits 2", err)
    code, _, err = status(tool, os.path.join(folder, "none.jsonl"))
    check(code == 3, "a missing file exits 3", err)
    code, _, err = status(tool, os.path.join(folder, "p.dot"))
    check(code == 2 and "pipeline-monitor open" in err, ".dot points to "
          "open", err)


def main():
    serve, tool = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory() as folder:
        recording(tool, folder)
    live(serve, tool)
    print("status: all checks passed")


if __name__ == "__main__":
    main()
