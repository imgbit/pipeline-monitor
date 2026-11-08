#!/bin/sh
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Runs the installed pipeline-monitor command (no program) and checks that
# it serves the viewer: its page, "viewer" in hello, the recordings list.
# COMMAND replaces "pipeline-monitor" (e.g. a path, or under
# "arch -x86_64").
# Usage: packaging/command-test.sh
set -eu

command=${COMMAND:-pipeline-monitor}
# shellcheck disable=SC2086
$command --version
dir=$(mktemp -d)
log=$(mktemp)
# shellcheck disable=SC2086
$command --no-browser --port 0 --recordings "$dir" > "$log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null || true; rm -rf "$log" "$dir"' EXIT

url=
for i in $(seq 1 150); do
  url=$(sed -n 's/^pipeline-monitor: \(http:[^#]*\).*/\1/p' "$log" | head -n 1)
  [ -n "$url" ] && break
  sleep 0.2
done
if [ -z "$url" ]; then
  echo "the command printed no address:"
  cat "$log"
  exit 1
fi

curl -fsS "$url" | grep -q "<title>pipeline-monitor</title>"
curl -fsS "${url}api/hello" | grep -q '"viewer":true'
curl -fsS "${url}api/recordings" | grep -q '"recordings":\[\]'
echo "command test passed: $url"
