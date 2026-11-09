#!/bin/sh
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Downloads URL to FILE and checks its SHA-256. A dropped or stalled
# transfer resumes where it stopped (the GStreamer download server drops
# long transfers); a FILE already there with the right checksum (e.g. from
# a cache) is kept.
# Usage: packaging/fetch.sh URL SHA256 FILE
set -eu

url=$1
sum=$2
file=$3

ok() {
  [ -f "$file" ] && echo "$sum  $file" | shasum -a 256 -c --status
}

if ok; then
  echo "$file: already there, checksum OK"
  exit 0
fi
mismatches=0
for try in $(seq 1 30); do
  if curl -fsSL -C - --speed-limit 100000 --speed-time 60 -o "$file" "$url"; then
    if ok; then
      echo "$file: OK after $try transfer(s)"
      exit 0
    fi
    rm -f "$file"
    mismatches=$((mismatches + 1))
    if [ $mismatches -ge 2 ]; then
      echo "$file: wrong checksum twice; expected $sum"
      exit 1
    fi
    echo "$file: checksum mismatch, downloading again"
  else
    echo "$file: transfer $try stopped at $(wc -c < "$file" 2>/dev/null || echo 0) bytes, resuming"
    sleep 5
  fi
done
echo "$file: could not download $url"
exit 1
