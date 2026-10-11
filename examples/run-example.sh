#!/bin/sh
# Usage: examples/run-example.sh [build-dir]
set -e
build_dir="${1:-build}"

GST_PLUGIN_PATH="$build_dir/src" \
GST_TRACERS=pipeline-monitor \
GST_DEBUG=pipeline-monitor:5 \
GST_DEBUG_NO_COLOR=1 \
  gst-launch-1.0 -q videotestsrc num-buffers=30 ! queue ! fakesink
