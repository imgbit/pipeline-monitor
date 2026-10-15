#!/bin/sh
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
# Usage: examples/run-example.sh [--serve] [pipeline description]
# Prints the graph of a running pipeline as JSON, or with --serve keeps it
# running for the web server. BUILD_DIR defaults to build.
set -e
build_dir="${BUILD_DIR:-build}"
program=print-graph
if [ "$1" = "--serve" ]; then
  program=serve
  shift
fi

GST_PLUGIN_PATH="$build_dir/src" exec "$build_dir/examples/$program" "$@"
