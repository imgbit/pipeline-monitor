# Live video seconds behind: x264enc holds the frames

**Symptom.** A live pipeline encodes with x264 and decodes again. It
plays, but the picture is about two seconds behind, and stays that far
behind. With a clock drawn into the picture when the frame is made, and
another when it is shown, the two are two seconds apart:

```sh
gst-launch-1.0 videotestsrc is-live=true pattern=ball ! \
  video/x-raw,width=1280,height=720,framerate=30/1 ! \
  clockoverlay valignment=top halignment=left text="made " \
    time-format="%H:%M:%S" font-desc="Sans 48" ! \
  x264enc ! h264parse ! avdec_h264 ! videoconvert ! \
  clockoverlay valignment=bottom halignment=left text="shown" \
    time-format="%H:%M:%S" font-desc="Sans 48" ! autovideosink
```

**Why.** By default x264 keeps frames before it encodes them: it looks
ahead at the frames that follow to decide how to compress each one, and
works on several frames at a time. That compresses better, and for a file
costs nothing. In a live pipeline every frame waits for the ones after it.
x264enc reports that delay as its latency, and the pipeline holds every
frame back by it, so the picture stays in step with itself, two seconds
late.

**What pipeline-monitor shows.** Turn it on with
`GST_TRACERS=pipeline-monitor` and open the page:

- The header: **latency 2.28 s live**, the delay the whole pipeline
  works with.
- **latency** under the graph measures every element. Nearly all take
  microseconds; **x264enc0 takes 2.16 s** (avdec_h264 about 70 ms).

**Fix.** Tell x264 the pipeline is live:

```sh
... ! x264enc tune=zerolatency ! h264parse ! ...
```

`tune=zerolatency` turns off the lookahead and B-frames and encodes each
frame as it comes. Measured again, x264enc takes a few milliseconds, the
pipeline's latency in the header drops from 2.28 s to about 50 ms, and the
two clocks show the same time.

**See it yourself.** Open
[the recording of this pipeline](../#file=episodes/live-video-seconds-behind.jsonl)
in the viewer, no install needed: press ▶ or move the slider to see the
latency under each element.
