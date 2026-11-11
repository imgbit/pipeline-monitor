#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Makes the demo recordings of the published site with pipeline-monitor
itself.

Usage: packaging/make-demos.py [--build DIR] [--out DIR] [DEMO...]

Runs each demo's pipeline with gst-launch-1.0 and the tracer from the build,
records it from the start with its pads pinned, and writes it to OUT
(default: site/demos) with its entry in OUT/demos.json. gst-launch-1.0 and
the GStreamer plugins come from PATH. Without DEMO, makes them all.
"""

import argparse
import getpass
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from test_ui import WebSocket  # noqa: E402

# Files the demos play are made here: the path is kept in the recording.
MEDIA = "/tmp/pipeline-monitor-demo"


def launch(args, env=None):
    subprocess.run(["gst-launch-1.0", "-q", *args], check=True, env=env,
                   stdout=subprocess.DEVNULL)


def make_movie():
    """20 s of H.264 video and AAC audio in an MP4."""
    os.makedirs(MEDIA, exist_ok=True)
    movie = f"{MEDIA}/movie.mp4"
    if not os.path.exists(movie):
        launch(("videotestsrc num-buffers=600 pattern=ball ! "
                "video/x-raw,format=I420,width=1280,height=720,"
                "framerate=30/1 ! x264enc ! h264parse ! queue ! mux. "
                "audiotestsrc num-buffers=940 ! audio/x-raw,rate=48000 ! "
                "avenc_aac ! aacparse ! queue ! mux. "
                f"mp4mux name=mux ! filesink location={movie}").split())


DEMOS = {
    "uridecodebin3": {
        "title": "What's inside uridecodebin3?",
        "description": ("Bins inside bins, pads that appear while it plays, a "
                        "queue per stream."),
        "prepare": make_movie,
        "pipeline": (f"uridecodebin3 uri=file://{MEDIA}/movie.mp4 "
                     "name=player "
                     "player. ! videoconvert ! "
                     "fakesink name=video-sink sync=true "
                     "player. ! audioconvert ! audioresample ! "
                     "fakesink name=audio-sink sync=true"),
        # What Linux users get, not the platform's own decoder.
        "env": {"GST_PLUGIN_FEATURE_RANK": "avdec_h264:MAX"},
        # (factory, pad): pinned as soon as such a pad appears.
        "pins": [("avdec_h264", "src")],
    },
    "slow-encoder": {
        "title": ("Which element is slowing the pipeline? "
                  "(answer: x264enc, its input queue stays full)"),
        "description": ("video-queue is full in front of x264enc, the "
                        "slowest element; audio-queue is full only because "
                        "mp4mux waits for the video."),
        "prepare": lambda: os.makedirs(MEDIA, exist_ok=True),
        # 50 s of 1080p, encoded as fast as the encoder can: about 15 s.
        # zerolatency: the encoder takes frames one by one, at its pace
        # (with lookahead it takes them in bursts and the queue empties).
        "pipeline": ("videotestsrc num-buffers=1500 pattern=ball ! "
                     "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
                     "queue name=video-queue ! "
                     "x264enc speed-preset=slower tune=zerolatency ! "
                     "h264parse ! mux. "
                     "audiotestsrc num-buffers=2344 ! "
                     "audio/x-raw,rate=48000 ! queue name=audio-queue ! "
                     "avenc_aac ! aacparse ! mux. "
                     "mp4mux name=mux ! "
                     f"filesink location={MEDIA}/transcoded.mp4"),
        "pins": [("x264enc", "sink")],
    },
    "live-encode": {
        "title": ("Why does my live encode take seconds to start? "
                  "(answer: x264enc holds frames back before its first "
                  "output)"),
        "description": ("Frames go in, none come out for over 3 s; the "
                        "pipeline waits in PAUSED."),
        "prepare": lambda: os.makedirs(MEDIA, exist_ok=True),
        # x264enc's default lookahead holds frames back; on a slow preset
        # with few threads the sink waits over 3 s for its first buffer.
        "pipeline": ("videotestsrc is-live=true num-buffers=360 "
                     "pattern=ball ! "
                     "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
                     "x264enc speed-preset=veryslow threads=2 ! h264parse ! "
                     "queue ! mux. "
                     "audiotestsrc is-live=true num-buffers=560 ! "
                     "audio/x-raw,rate=48000 ! avenc_aac ! aacparse ! "
                     "queue ! mux. "
                     "mp4mux name=mux ! "
                     f"filesink location={MEDIA}/live.mp4"),
        "pins": [("x264enc", "sink"), ("x264enc", "src")],
    },
    "not-negotiated": {
        "title": "\"not-negotiated\": which link failed?",
        "description": ("No videoconvert after the decoder: the video stops, "
                        "the audio waits, and nothing says why."),
        "prepare": make_movie,
        # A decoder straight into a format it does not make: no videoconvert.
        "pipeline": (f"filesrc location={MEDIA}/movie.mp4 ! "
                     "qtdemux name=demux "
                     "demux.video_0 ! queue ! h264parse ! avdec_h264 ! "
                     "video/x-raw,format=RGBA ! "
                     "fakesink name=video-sink sync=true "
                     "demux.audio_0 ! queue ! fakesink name=audio-sink"),
        "env": {"GST_PLUGIN_FEATURE_RANK": "avdec_h264:MAX"},
        # The audio waits for the video that never comes: it never ends.
        "seconds": 8,
    },
    "tee-without-queues": {
        "title": "Pipeline hangs: the tee without queues",
        "description": ("The first branch holds the tee's thread; the second "
                        "never gets a buffer."),
        "prepare": lambda: os.makedirs(MEDIA, exist_ok=True),
        # Both branches run in the tee's thread: the first sink waits to
        # preroll and holds it, so the second never gets a buffer.
        "pipeline": ("videotestsrc pattern=ball ! "
                     "video/x-raw,width=1280,height=720,framerate=30/1 ! "
                     "tee name=split "
                     "split. ! videoconvert ! fakesink name=display sync=true "
                     "split. ! x264enc ! h264parse ! mp4mux ! "
                     f"filesink location={MEDIA}/tee.mp4"),
        "seconds": 8,
    },
    "playbin3-snapshot": {
        "title": "A snapshot of playbin3: every element, every property",
        "description": ("One moment of a whole player: bins inside bins, "
                        "and each element's settings to look up."),
        "prepare": make_movie,
        "pipeline": ["playbin3", f"uri=file://{MEDIA}/movie.mp4",
                     'video-sink="fakesink sync=true"',
                     'audio-sink="fakesink sync=true"'],
        "env": {"GST_PLUGIN_FEATURE_RANK": "avdec_h264:MAX"},
        # Once it has built everything and plays.
        "snapshot_at": 3,
    },
}


def pads(graph):
    """(factory, pad name, pad id, pad path) of every pad in a graph."""
    def walk(element):
        for pad in element["pads"]:
            yield element["factory"], pad["name"], pad["id"], pad["path"]
        for child in element["children"]:
            yield from walk(child)
    for pipeline in graph.get("pipelines", []):
        yield from walk(pipeline)


def record(name, demo, build, folder):
    """Runs the demo with a recording from the start; returns its file."""
    env = dict(os.environ, **demo.get("env", {}),
               GST_PLUGIN_PATH=os.path.join(build, "src"),
               GST_TRACERS=f'pipeline-monitor(port=0,wait=true,'
                           f'recordings="{folder}")')
    # A list when an argument has spaces (quoted for gst-launch).
    pipeline = demo["pipeline"]
    args = pipeline if isinstance(pipeline, list) else pipeline.split()
    proc = subprocess.Popen(["gst-launch-1.0", "-q", *args],
                            env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    url = None
    for line in proc.stderr:
        found = re.match(r"pipeline-monitor: http://(\S+?)/", line)
        if found:
            url = found.group(1)
            break
    if url is None:
        proc.wait()
        sys.exit(f"{name}: no address from the tracer")
    threading.Thread(target=lambda: proc.stderr.read(), daemon=True).start()

    # The program goes on once this connects: record before it builds
    # anything, so the recording shows the pipeline being made. A snapshot
    # is one moment, taken once the pipeline is built and playing.
    ws = WebSocket(f"ws://{url}/ws")
    wanted = list(demo.get("pins", []))
    paths = {}
    view = {"pins": [], "element_pins": [], "measuring": [],
            "measure_all": False, "selected": None, "collapsed": []}
    if "snapshot_at" in demo:
        time.sleep(demo["snapshot_at"])
        ws.send(json.dumps({"type": "snapshot", "view": view}))
        time.sleep(2)
        proc.send_signal(signal.SIGINT)
    else:
        ws.send(json.dumps({"type": "record-start", "view": view}))

    def follow():
        while wanted:
            try:
                message = json.loads(ws.recv())
            except (OSError, ValueError):
                return
            if message.get("type") != "graph":
                continue
            for factory, pad, pad_id, path in pads(message):
                if (factory, pad) in wanted:
                    wanted.remove((factory, pad))
                    paths[path] = pad_id
                    ws.send(json.dumps({"type": "watch-pad", "pad": pad_id}))

    threading.Thread(target=follow, daemon=True).start()
    try:
        proc.wait(timeout=demo.get("seconds"))
    except subprocess.TimeoutExpired:
        # A program that never ends by itself: interrupted like Ctrl-C, it
        # stops its pipeline and exits.
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=30)
    ws.close()
    if wanted:
        sys.exit(f"{name}: never saw a pad to pin: {wanted}")
    made = [n for n in os.listdir(folder) if n.endswith(".jsonl")]
    if len(made) != 1:
        sys.exit(f"{name}: expected one recording, found {made}")
    return os.path.join(folder, made[0]), list(paths)


def pin_view(path, pinned):
    """Puts the pinned pads in the recording's header, as the page would."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    header = json.loads(lines[0])
    header["view"]["pins"] = [{"path": p} for p in pinned]
    lines[0] = json.dumps(header, separators=(",", ":"), ensure_ascii=False)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def private(path):
    """Things in the file that say who or where the author is."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    words = {os.path.expanduser("~"), getpass.getuser(),
             socket.gethostname().split(".")[0]}
    return [w for w in words if len(w) > 2 and w.lower() in text.lower()]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default=os.path.join(ROOT, "build"))
    parser.add_argument("--out", default=os.path.join(ROOT, "site", "demos"))
    parser.add_argument("demos", nargs="*", default=list(DEMOS))
    args = parser.parse_args()

    listing = os.path.join(args.out, "demos.json")
    with open(listing, encoding="utf-8") as f:
        entries = json.load(f)
    for name in args.demos:
        demo = DEMOS[name]
        demo.get("prepare", lambda: None)()
        folder = tempfile.mkdtemp(prefix="pm-demo-")
        try:
            start = time.time()
            path, pinned = record(name, demo, args.build, folder)
            pin_view(path, pinned)
            found = private(path)
            if found:
                sys.exit(f"{name}: the recording names {found}")
            target = os.path.join(args.out, f"{name}.jsonl")
            shutil.copy(path, target)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        entries = [e for e in entries if e["file"] != f"{name}.jsonl"]
        entries.append({"title": demo["title"],
                        "description": demo["description"],
                        "file": f"{name}.jsonl"})
        print(f"{name}: {os.path.getsize(target)} bytes, "
              f"{time.time() - start:.0f} s, pinned {pinned}")
    # The demos in the order above, then any others.
    order = [f"{n}.jsonl" for n in DEMOS]
    entries.sort(key=lambda e: order.index(e["file"])
                 if e["file"] in order else len(order))
    with open(listing, "w", encoding="utf-8") as f:
        f.write("[\n" + ",\n".join(" " + json.dumps(e, ensure_ascii=False)
                                   for e in entries) + "\n]\n")


if __name__ == "__main__":
    main()
