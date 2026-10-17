# pipeline-monitor

See a running GStreamer pipeline live in your browser.

![pipeline-monitor showing a pipeline with decodebin3 and a tee](.github/screenshot.png)

pipeline-monitor is a GStreamer tracer plugin. Enable it with one environment
variable, without changing your application, and open http://127.0.0.1:8765/.

- Elements, pads and links as a graph, including nested bins and ghost pads
- Updates live as elements, pads, links and states change
- Element details, collapsible bins, light and dark theme
- Local only: listens on 127.0.0.1 and loads nothing from the internet

## Build

Requires GStreamer 1.22 or newer, libsoup 3 and Meson.

```sh
meson setup build
meson compile -C build
meson test -C build
```

## Use

```sh
GST_PLUGIN_PATH=$PWD/build/src GST_TRACERS=pipeline-monitor \
  gst-launch-1.0 videotestsrc is-live=true ! videoconvert ! autovideosink
```

Then open the address it prints, by default http://127.0.0.1:8765/.

- In your own application, set `GST_TRACERS=pipeline-monitor` in the
  environment or with `setenv()` before `gst_init()`.
- Choose the port with `GST_TRACERS="pipeline-monitor(port=9000)"`. If the
  port is taken, the next free one is used.
- Try the example, which adds and removes a branch every two seconds:
  `./examples/run-example.sh --serve`

The server has no authentication. It only accepts connections from the same
machine and only from its own page.

## Status

Version 0.1 shows the pipeline graph. It is tested on macOS (Apple Silicon)
with GStreamer 1.28.8, and in CI on Ubuntu 24.04 (GStreamer 1.24), Debian 12
(GStreamer 1.22) and macOS (Homebrew). Windows is not tested yet.

## License

LGPL-2.1-or-later, see [LICENSE](LICENSE).

A commercial license, support and GStreamer consulting are available from the
author, Łukasz Huńczak: lukasz.hunczak@imaginarybit.com,
[imaginarybit.com](https://imaginarybit.com)

The web page includes [elkjs](https://github.com/kieler/elkjs) for graph
layout, under the Eclipse Public License 2.0. See
[web/vendor/elkjs](web/vendor/elkjs).
