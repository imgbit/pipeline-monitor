# Options

Options go in brackets after the tracer's name, separated by commas:

```sh
GST_TRACERS="pipeline-monitor(port=9000,record=on)" ./my-program
```

| Option | Default | |
|---|---|---|
| `port=N` | `8765` | the page's port; if taken, the next free one is used |
| `log=false` | on | do not collect errors and warnings (the program still gets them) |
| `flow=false` | on | no data flow tracking: where data stopped, which thread runs each element (saves about 10 ns per push; any GStreamer tracer costs about 90 ns per push on its own) |
| `wait=true` | off | hold the program at start until a page is open |
| `record=on` | off | record from the start, also without a page, until the program ends |
| `recordings=PATH` | `~/.local/share/pipeline-monitor/recordings` (Windows: `%LOCALAPPDATA%\pipeline-monitor\recordings`) | where recordings go (on Windows write the path with `/`) |
| `record-file-max=SIZE` | `100M` | a recording ends at this size |
| `record-total-max=SIZE` | `1G` | above this, the oldest recordings are deleted |
| `ui-dir=PATH` | – | for working on the page itself: serve its files from this folder |

Sizes take `K`, `M` or `G`.

**The `pipeline-monitor` command** opens recordings without a program:

| | |
|---|---|
| `pipeline-monitor` | the recordings in the folder |
| `pipeline-monitor open FILE.jsonl` | that recording |
| `--recordings DIR` | another folder |
| `--port N` | the port (default 8766, or the next free one) |
| `--no-browser` | only print the address |
| `pipeline-monitor status` | what the page shows, as text ([below](#status)) |
| `--version` | the version |

<h3 id="status">In the terminal: <code>pipeline-monitor status</code></h3>

With your program running with the tracer, `pipeline-monitor status`
prints what the page shows: each program, its pipelines and states, queue
fill, CPU, the problems found with their hints, and errors and warnings.

```
appsrc-scenarios (pid 10168) at http://127.0.0.1:8765/
  GStreamer 1.28.8, pipeline-monitor 0.14.0
camera0  PLAYING  live  running 0:00:08.204  latency 0 ns  clock GstSystemClock
  3 elements; queues, fullest first: out0 0 %
display  PLAYING  live  running 0:00:08.203  latency 0 ns  clock GstSystemClock
  3 elements; queues, fullest first: in 8640 % bytes
Process: CPU 1 % of a core, memory 42.8 MiB, 6 threads
Problems
  warning  display/in has held more than its max-bytes for 7 s (86×), and keeps queueing
           More is pushed into it than leaves it. Past its limits
           appsrc only emits enough-data; to stop there, set
           block=true (the push waits) or leaky-type (it drops).
  warning  display/in sends camera0/out0's buffers in a segment from 0:00:00.000, not the one they arrived in (from 0:00:05.000)
           Their timestamps are read against another start, so they
           play at another time. appsrc keeps its own segment
           unless samples are pushed (push-sample) with
           handle-segment-change=true and format=time.
Log: no errors or warnings
```

Every line is something pipeline-monitor observed in the running
program: states, levels, the formats negotiated, what elements reported.
Hints explain what an observation means; none is a guess.

| | |
|---|---|
| `--element NAME` | one element: its pads with their caps and links, its queue, the latency it reports, the properties changed from their defaults, its problems and log entries |
| `--pad ELEMENT:PAD` | one pad, watched for 2.5 s: buffers per second, bytes, gaps, the last buffer; for raw audio the peak and RMS level of each channel in dBFS, clipped samples and silence; for raw video the picture's brightness, whether it is black, and how long it has not changed |
| `--graph` | every element, link and negotiated format |
| `--log` (`--last N`) | errors and warnings with GStreamer's details |
| `--watch` | again every second |
| `FILE.jsonl` (`--at 12.5`) | a recording, or the moment 12.5 s in |
| `--json` | the plugin's own messages, one per line ([format](https://github.com/imgbit/pipeline-monitor/blob/main/docs/protocol.md)) |

It exits with 1 when it finds an error (warnings do not count), 3 when no
program is running with the tracer, so scripts can use it as it is.
