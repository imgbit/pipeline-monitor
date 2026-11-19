# "Internal data stream error" after decodebin: formats that don't fit

**Symptom.** You ask for a raw video format right after a decoder.
`gst-launch-1.0` ends at once with a warning and an error:

```
WARNING: from element /GstPipeline:pipeline0/GstDecodeBin:decodebin0: Delayed linking failed.
ERROR: from element /GstPipeline:pipeline0/GstVideoTestSrc:videotestsrc0: Internal data stream error.
```

```sh
gst-launch-1.0 videotestsrc is-live=true ! jpegenc ! decodebin ! \
  video/x-raw,format=RGBA ! autovideosink
```

**Why.** decodebin makes its output pad only once it knows the stream, and
links it then: that is the "delayed linking". The decoder offers formats
such as NV12 or I420; the caps filter after it accepts only RGBA. With no
format in common, the link fails and decodebin's output stays unlinked.
The next buffer pushed out of it comes back "not-linked", and the source
at the start of the pipeline stops with "Internal data stream error". The
error names the source, not the place that failed.

**What pipeline-monitor shows.** Turn it on with
`GST_TRACERS="pipeline-monitor(wait=true)"`: the program ends at once, so
`wait=true` holds it until the page is open, and the page keeps the last
state after it ends. Under **Problems**:

- **decodebin0:src_0 could not link to pipeline0/capsfilter1:sink: no
  common format**, with what each side handles: upstream offers NV12 and
  others (the list depends on the decoder, I420 on many Linux systems),
  downstream accepts RGBA, the field that differs marked.
- **Data stopped at pipeline0/decodebin0:src_0: not-linked** — Pushing
  from this pad returned not-linked: the pad is not linked to anything.

**Log** has GStreamer's own messages: "Delayed linking failed" and
"Internal data stream error".

**Fix.** Convert between the formats with videoconvert:

```sh
gst-launch-1.0 videotestsrc is-live=true ! jpegenc ! decodebin ! \
  videoconvert ! video/x-raw,format=RGBA ! autovideosink
```

pipeline-monitor then shows no problems, and it plays.

Whenever a source stops with "Internal data stream error", look for where
the data stopped: pipeline-monitor names the pad, and for a failed link it
shows what each side handles.

**See it yourself.** Open
[the recording of this pipeline](../#file=episodes/internal-data-stream-error.jsonl)
in the viewer, no install needed.
