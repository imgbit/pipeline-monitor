# Troubleshooting

- **No address is printed.** The tracer is not loaded.
  - Is `GST_TRACERS=pipeline-monitor` in the program's environment?
    (Some launchers and IDEs do not pass it on.)
  - Does `gst-inspect-1.0 pipeline-monitor` find it? With Homebrew,
    `GST_PLUGIN_PATH` must point to `$(brew --prefix)/lib/gstreamer-1.0`;
    with a zip, to its folder.
  - On macOS with a zip: remove the quarantine mark (see
    [Get started](index.html)).
- **The address has another port.** 8765 was taken (often by another
  program with the tracer): the next free port is used. Every GStreamer
  process with the tracer has its own page; child processes inherit
  `GST_TRACERS`.
- **"cannot parse options …, using defaults".** The options are not
  valid, e.g. a missing bracket or comma; check the quotes in your shell.
- **The page says "Waiting for a pipeline…".** The program has not built
  a pipeline yet. It appears when it does. For a program that fails at
  once, use `wait=true` ([Options](options.html)).
- **The program runs on another machine.** The page only accepts
  connections from the same machine (it has no login). Forward the port:
  `ssh -L 8765:127.0.0.1:8765 that-machine`, then open
  `http://localhost:8765/` here.
- **A recording does not open.** Files that are not recordings, or are
  damaged, are refused with a message; a cut-off last line (after a
  crash) is skipped.
- **Something else.** Ask in GitHub Discussions, or open an issue and
  attach a recording of the problem: it holds what the page showed. Check
  it first: passwords in addresses and secret properties are hidden, but
  element names, file paths and messages are kept.
