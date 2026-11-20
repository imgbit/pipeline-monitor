#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""appsink -> application -> appsrc, the usual ways and the usual mistakes:
the programs pipeline-monitor's appsrc checks are written against.

Usage: tests/appsrc_scenarios.py SCENARIO [--seconds N]

Pipeline "camera" (videotestsrc ... ! appsink) feeds pipeline "display"
(appsrc ! videoconvert ! fakesink sync=true) through this program. Run it
with GST_TRACERS=pipeline-monitor to see what the page shows.

Scenarios:
  bytes-segment      appsrc as created: format=bytes, push_sample
  segment-ignored    format=time, push_sample of a segment that starts at
                     5 s (camera seeked), handle-segment-change off
  segment-handled    the same with handle-segment-change on
  copied-timestamps  format=time, is-live, the camera's timestamps pushed
                     as they are (the two pipelines started apart)
  no-timestamps      timestamps removed, do-timestamp off
  do-timestamp       timestamps removed, do-timestamp on, is-live
  do-timestamp-latency  the same with min-latency=20 ms
  own-running-time   the program stamps each buffer with the display
                     pipeline's running time now, min-latency=20 ms
  shared-clock       the camera's timestamps, both pipelines on one clock
                     and one base time (the correct way to forward)
  copied-data        the bytes copied into new buffers (nothing links them)
  two-sinks          two cameras into one appsrc, in turns
  stops              the program stops pushing after 3 s
"""

import argparse
import sys

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstApp", "1.0")
from gi.repository import GLib, Gst, GstApp  # noqa: E402,F401

CAMERA = ("videotestsrc name=cam{n} pattern={pattern} {live} ! "
          "video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! "
          "appsink name=out{n} sync={sync} max-buffers=4 drop=true")
DISPLAY = ("appsrc name=in caps=video/x-raw,format=I420,width=320,"
           "height=240,framerate=30/1 {props} ! videoconvert ! "
           "fakesink name=display sync=true")

SCENARIOS = {
    "bytes-segment": {"display": ""},
    "segment-ignored": {"display": "format=time", "seek": 5},
    "segment-handled": {"display": "format=time handle-segment-change=true",
                        "seek": 5},
    "copied-timestamps": {"display": "format=time is-live=true", "late": 2},
    "no-timestamps": {"display": "format=time", "strip": True},
    "do-timestamp": {"display": "format=time is-live=true do-timestamp=true",
                     "strip": True},
    "do-timestamp-latency": {"display": "format=time is-live=true "
                             "do-timestamp=true min-latency=20000000",
                             "strip": True},
    "own-running-time": {"display": "format=time is-live=true "
                         "min-latency=20000000", "strip": True,
                         "own": True},
    "shared-clock": {"display": "format=time is-live=true", "share": True},
    "copied-data": {"display": "format=time is-live=true do-timestamp=true",
                    "copy": True, "strip": True},
    "two-sinks": {"display": "format=time is-live=true do-timestamp=true",
                  "cameras": 2, "strip": True},
    "stops": {"display": "format=time is-live=true do-timestamp=true",
              "strip": True, "stop": 3},
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=sorted(SCENARIOS))
    parser.add_argument("--seconds", type=float, default=8)
    parser.add_argument("--observe", action="store_true",
                        help="print what leaves appsrc and when it plays")
    args = parser.parse_args()
    s = SCENARIOS[args.scenario]
    Gst.init(None)

    seek = s.get("seek")
    cameras = []
    for n in range(s.get("cameras", 1)):
        cameras.append(Gst.parse_launch(CAMERA.format(
            n=n, pattern=["ball", "smpte"][n],
            # Seeking needs a source that is not live; sync paces it.
            live="" if seek else "is-live=true",
            sync="true" if seek else "false")))
        cameras[-1].set_name(f"camera{n}")
    display = Gst.parse_launch(DISPLAY.format(props=s["display"]))
    display.set_name("display")
    appsrc = display.get_by_name("in")
    pushed = {"n": 0}
    started = GLib.get_monotonic_time()

    def forward(sink):
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.EOS
        if s.get("stop") and \
                GLib.get_monotonic_time() - started > s["stop"] * 1000000:
            return Gst.FlowReturn.OK
        buffer = sample.get_buffer()
        if s.get("copy"):
            ok, info = buffer.map(Gst.MapFlags.READ)
            data = bytes(info.data)
            buffer.unmap(info)
            buffer = Gst.Buffer.new_wrapped(data)
        if s.get("strip"):
            if not s.get("copy"):
                # A new buffer with the same memory, as an application
                # that changes timestamps makes one.
                fresh = Gst.Buffer.new()
                for i in range(buffer.n_memory()):
                    fresh.append_memory(buffer.get_memory(i))
                buffer = fresh
            buffer.pts = Gst.CLOCK_TIME_NONE
            buffer.dts = Gst.CLOCK_TIME_NONE
            if s.get("own"):
                # This pipeline's running time now: its clock's time minus
                # its base time.
                clock = display.get_clock()
                if clock is None:
                    return Gst.FlowReturn.OK
                buffer.pts = clock.get_time() - display.get_base_time()
        if "seek" in s or s["display"] == "":
            appsrc.emit("push-sample", Gst.Sample.new(
                buffer, sample.get_caps(), sample.get_segment(), None))
        else:
            appsrc.emit("push-buffer", buffer)
        pushed["n"] += 1
        return Gst.FlowReturn.OK

    for n, camera in enumerate(cameras):
        sink = camera.get_by_name(f"out{n}")
        sink.set_property("emit-signals", True)
        sink.connect("new-sample", forward)

    for camera in cameras:
        camera.set_state(Gst.State.PAUSED)
        camera.get_state(Gst.CLOCK_TIME_NONE)
        if seek:
            camera.seek_simple(Gst.Format.TIME, Gst.SeekFlags.FLUSH,
                               seek * Gst.SECOND)
            camera.get_state(Gst.CLOCK_TIME_NONE)
        camera.set_state(Gst.State.PLAYING)
    if s.get("late"):
        GLib.usleep(int(s["late"] * 1000000))
    if s.get("share"):
        # One clock and one base time: running times mean the same.
        camera = cameras[0]
        display.use_clock(camera.get_clock())
        display.set_start_time(Gst.CLOCK_TIME_NONE)
        display.set_base_time(camera.get_base_time())
    seen = {"segment": None, "pts": [], "early": [], "camera_pts": []}
    if args.observe:
        def at_appsrc(pad, info):
            if info.type & Gst.PadProbeType.EVENT_DOWNSTREAM:
                event = info.get_event()
                if event.type == Gst.EventType.SEGMENT:
                    seen["segment"] = event.parse_segment()
            elif info.type & Gst.PadProbeType.BUFFER:
                seen["pts"].append(info.get_buffer().pts)
            return Gst.PadProbeReturn.OK

        def at_sink(pad, info):
            # How early a buffer reaches the sink, against the moment the
            # sink plays it: running time + the pipeline's latency, minus
            # the running time now (negative: late, played at once).
            buffer = info.get_buffer()
            segment = seen["segment"]
            clock = display.get_clock()
            if segment and clock and buffer.pts != Gst.CLOCK_TIME_NONE \
                    and segment.format == Gst.Format.TIME:
                rt = segment.to_running_time(Gst.Format.TIME, buffer.pts)
                now = clock.get_time() - display.get_base_time()
                query = Gst.Query.new_latency()
                latency = query.parse_latency()[1] if display.query(query) \
                    and query.parse_latency()[0] else 0
                if rt != Gst.CLOCK_TIME_NONE:
                    seen["early"].append((rt + latency - now) / Gst.SECOND)
            return Gst.PadProbeReturn.OK

        def at_camera(pad, info):
            seen["camera_pts"].append(info.get_buffer().pts)
            return Gst.PadProbeReturn.OK

        appsrc.get_static_pad("src").add_probe(
            Gst.PadProbeType.EVENT_DOWNSTREAM | Gst.PadProbeType.BUFFER,
            at_appsrc)
        display.get_by_name("display").get_static_pad("sink").add_probe(
            Gst.PadProbeType.BUFFER, at_sink)
        cameras[0].get_by_name("out0").get_static_pad("sink").add_probe(
            Gst.PadProbeType.BUFFER, at_camera)
    display.set_state(Gst.State.PLAYING)

    loop = GLib.MainLoop()
    GLib.timeout_add(int(args.seconds * 1000), loop.quit)
    loop.run()
    print(f"{args.scenario}: pushed {pushed['n']} buffers")
    if args.observe:
        def t(v):
            return "none" if v == Gst.CLOCK_TIME_NONE else f"{v / 1e9:.3f}"
        seg = seen["segment"]
        print("  appsrc segment:", f"{seg.format.value_nick} start "
              f"{t(seg.start)} base {t(seg.base)}" if seg else "none")
        print("  camera PTS first:", t(seen["camera_pts"][0]) if
              seen["camera_pts"] else "-", "| appsrc PTS first/last:",
              t(seen["pts"][0]) if seen["pts"] else "-",
              t(seen["pts"][-1]) if seen["pts"] else "-")
        early = seen["early"]
        print("  at the sink: early by",
              f"{min(early):+.3f} .. {max(early):+.3f} s" if early
              else "(not in time)", f"({len(early)} buffers)")
        query = Gst.Query.new_latency()
        display.query(query)
        print("  latency query (live, min, max):", query.parse_latency())
        stats = display.get_by_name("display").get_property("stats")
        print("  sink rendered", stats.get_value("rendered"), "dropped",
              stats.get_value("dropped"))
    for p in [display, *cameras]:
        p.set_state(Gst.State.NULL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
