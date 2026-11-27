# Stuck in PAUSED: a tee without queues

**Symptom.** You show a video and record it to a file at the same time,
with a tee. `gst-launch-1.0` stops after `Pipeline is PREROLLING ...`: no
error, no warning. The window shows one still frame, and the file stays
empty.

```sh
gst-launch-1.0 videotestsrc ! tee name=split \
  split. ! videoconvert ! autovideosink name=display \
  split. ! x264enc ! h264parse ! mp4mux ! filesink location=out.mp4
```

**Why.** A tee sends each buffer to its branches one after the other, in
the thread that pushes into it. The display branch gets the first buffer;
its sink keeps it until the pipeline plays, so that push does not return.
The recording branch never gets a buffer, its sink cannot preroll, and the
pipeline never gets past PAUSED: each side waits for the other.

**What pipeline-monitor shows.** Turn it on with
`GST_TRACERS=pipeline-monitor` and open the address it prints. Within a few
seconds, under **Problems**:

- **Data flow blocked at pipeline0/videoconvert0:src → pipeline0/display:sink
  for 9 s** (counting up) — The push into pipeline0/display:sink has not returned. display is in
  PAUSED and prerolled.
- **pipeline0/filesink0:sink has no data** — The nearest pad before it that
  has received data is pipeline0/x264enc0:sink.
- **pipeline0 has not reached PAUSED for 9 s (it is READY)** — Still changing
  state: filesink0 (READY → PAUSED). Prerolled: display. Waiting for
  preroll: filesink0.

On the graph, the link into the display is marked, and the recording
branch's sink is drawn dashed: still changing state.

**Fix.** Put a queue at the start of each branch. Each queue starts a
thread of its own, so a branch that waits no longer holds the others:

```sh
gst-launch-1.0 videotestsrc ! tee name=split \
  split. ! queue ! videoconvert ! autovideosink name=display \
  split. ! queue ! x264enc ! h264parse ! mp4mux ! filesink location=out.mp4
```

pipeline-monitor then shows no problems, and the pipeline plays.

The same holds for any element that feeds several branches from one
thread, such as a demuxer: give each branch a queue (decodebin and playbin
do this for you, with a multiqueue).

**See it yourself.** Open
[the recording of this pipeline](../#file=demos/tee-without-queues.jsonl)
in the viewer, no install needed, or watch the
[video](https://www.youtube.com/watch?v=xHvHI7xv9cs).
