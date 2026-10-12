#!/bin/sh
# Usage: examples/run-example.sh [pipeline description]
# Prints the graph of a running pipeline as JSON. BUILD_DIR defaults to build.
set -e
build_dir="${BUILD_DIR:-build}"

GST_PLUGIN_PATH="$build_dir/src" "$build_dir/examples/print-graph" "$@"
