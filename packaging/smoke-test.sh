#!/bin/sh
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Runs a pipeline with the installed tracer and checks that its page and
# its graph are served. LAUNCH replaces gst-launch-1.0 (e.g. to run it
# under "arch -x86_64").
# Usage: packaging/smoke-test.sh
set -eu

launch=${LAUNCH:-gst-launch-1.0}
log=$(mktemp)
# shellcheck disable=SC2086
GST_TRACERS="pipeline-monitor(port=0)" $launch fakesrc sizetype=fixed ! \
  identity name=smoke sleep-time=10000 ! fakesink > "$log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null || true; rm -f "$log"' EXIT

url=
for i in $(seq 1 150); do
  url=$(sed -n 's/^pipeline-monitor: \(http:[^ ]*\).*/\1/p' "$log" | head -n 1)
  [ -n "$url" ] && break
  sleep 0.2
done
if [ -z "$url" ]; then
  echo "the tracer printed no address:"
  cat "$log"
  exit 1
fi

curl -fsS "$url" | grep -q "<title>pipeline-monitor</title>"
for i in $(seq 1 50); do
  if curl -fsS "${url}api/graph" | grep -q '"name":"smoke"'; then
    echo "smoke test passed: $url"
    exit 0
  fi
  sleep 0.2
done
echo "the pipeline is not in ${url}api/graph:"
cat "$log"
exit 1
