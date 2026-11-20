# Use with an AI assistant

AI coding assistants that run commands in your terminal (Claude Code,
Cursor, Codex, and others) can read your pipeline the way you see it in
the page: run your program with the tracer, then ask the assistant to run
`pipeline-monitor status`.

### What it prints

```
my-player (pid 4242) at http://127.0.0.1:8765/
  GStreamer 1.28.8, pipeline-monitor 0.13.0
display  PLAYING  live  running 0:00:04.602  latency 0 ns  clock GstSystemClock
  3 elements; queues, fullest first: in 8582 % bytes
Process: CPU 1 % of a core, memory 40.9 MiB, 6 threads
Problems
  warning  display/in has held more than its max-bytes for 8 s (86×), and keeps queueing
           More is pushed into it than leaves it. Past its limits appsrc
           only emits enough-data; to stop there, set block=true (the push
           waits) or leaky-type (it drops).
Log: no errors or warnings
```

Every line is something pipeline-monitor observed in the running
program: states, levels, the formats negotiated, what elements reported.
Hints explain what an observation means; none is a guess.

### Going deeper

- `--element NAME`: one element's pads with their caps and links, its
  queue, the latency it reports, the properties changed from their
  defaults, its problems and log entries.
- `--graph`: every element, link and negotiated format.
- `--log` (`--last N`): errors and warnings with GStreamer's details.
- `--watch`: again every second.
- A recording: `pipeline-monitor status FILE.jsonl`, or `--at 12.5` for
  the moment 12.5 s in. Send a recording with a bug report, and anyone
  (or their assistant) can read it the same way.
- `--json`: the plugin's own messages, one per line
  ([format](https://github.com/imgbit/pipeline-monitor/blob/main/docs/protocol.md)).

`pipeline-monitor status` exits with 1 when it finds an error (warnings do
not count), 3 when no program is running with the tracer.

### Asking well

- "Run `pipeline-monitor status`. Why does the picture freeze after a few
  seconds?"
- "Run `pipeline-monitor status --element mysink` and tell me why it
  drops frames."
- "Here is a recording of the failure: `pipeline-monitor status
  bug.jsonl`. What stopped the data?"

### Assistants that cannot run commands

Chat-only assistants need an MCP server to reach the program. There is
none yet. If you would use one, say so in
[Discussions](https://github.com/imgbit/pipeline-monitor/discussions).
