#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Writes GStreamer's own .dot files of a few pipelines, as test data for
the page's .dot reader.

Usage: tests/make-dot-fixtures.py OUT_DIR

Each pipeline is first written at READY, before anything is negotiated
(<name>.ready.dot: its links say what each side accepts), then run until
it plays (or fails) and written at several detail levels:
<name>.<level>.dot. Made by the GStreamer on PATH, so the files show what
that version writes.
"""

import os
import sys

import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib  # noqa: E402

PIPELINES = {
    # Request pads on the tee, two branches.
    "tee": "videotestsrc is-live=true ! tee name=t "
           "t. ! queue ! fakesink t. ! queue ! fakesink",
    # Sometimes pads, ghost pads, a link that cannot be made.
    "failed-link": "videotestsrc ! jpegenc ! decodebin ! "
                   "video/x-raw,format=RGBA ! fakesink",
    # Bins inside bins, many properties, a muxer's request pads.
    "mux": "videotestsrc is-live=true ! x264enc ! h264parse ! queue ! "
           "mp4mux name=m ! fakesink audiotestsrc is-live=true ! "
           "avenc_aac ! queue ! m.",
    # Names GStreamer has to escape.
    "names": 'videotestsrc ! identity name="a b \\"c\\" ł" ! fakesink',
}

LEVELS = {
    "media-type": Gst.DebugGraphDetails.MEDIA_TYPE,
    "states": Gst.DebugGraphDetails.STATES,
    "all": Gst.DebugGraphDetails.ALL,
    "verbose": Gst.DebugGraphDetails.VERBOSE,
}


def settle(pipeline):
    """Plays the pipeline and waits until it plays, fails or 3 s pass."""
    pipeline.set_state(Gst.State.PLAYING)
    bus = pipeline.get_bus()
    end = GLib.get_monotonic_time() + 3 * 1000000
    while GLib.get_monotonic_time() < end:
        msg = bus.timed_pop_filtered(100 * Gst.MSECOND,
                                     Gst.MessageType.ERROR |
                                     Gst.MessageType.STATE_CHANGED)
        if msg is None:
            continue
        if msg.type == Gst.MessageType.ERROR:
            return
        if msg.src == pipeline and \
                msg.parse_state_changed()[1] == Gst.State.PLAYING:
            GLib.usleep(300000)
            return


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    Gst.init(None)
    for name, description in PIPELINES.items():
        pipeline = Gst.parse_launch(description)
        pipeline.set_state(Gst.State.READY)
        pipeline.get_state(Gst.CLOCK_TIME_NONE)
        with open(os.path.join(out, f"{name}.ready.dot"), "w",
                  encoding="utf-8") as f:
            f.write(Gst.debug_bin_to_dot_data(pipeline,
                                              Gst.DebugGraphDetails.ALL))
        settle(pipeline)
        for level, details in LEVELS.items():
            path = os.path.join(out, f"{name}.{level}.dot")
            with open(path, "w", encoding="utf-8") as f:
                f.write(Gst.debug_bin_to_dot_data(pipeline, details))
        pipeline.set_state(Gst.State.NULL)
    print(f"{len(PIPELINES) * (len(LEVELS) + 1)} files in {out}, GStreamer "
          f"{Gst.version_string()}")


if __name__ == "__main__":
    main()
