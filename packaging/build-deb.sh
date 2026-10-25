#!/bin/sh
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Builds a binary .deb of the plugin for the Debian or Ubuntu release it
# runs on, from a build configured with --prefix=/usr.
# Usage: packaging/build-deb.sh BUILD_DIR OUT_DIR
set -eu

build=$1
out=$2
version=$(meson introspect "$build" --projectinfo |
  python3 -c 'import json, sys; print(json.load(sys.stdin)["version"])')
. /etc/os-release
distro="$ID$VERSION_ID"
arch=$(dpkg --print-architecture)
stage=$(mktemp -d)
root="$stage/root"

DESTDIR="$root" meson install -C "$build" --no-rebuild --strip
plugin=$(cd "$root" && find . -name 'libgstpipelinemonitor.so')
[ -n "$plugin" ]

# Depends from the libraries the plugin links; dpkg-shlibdeps wants a
# debian/control next to it.
mkdir -p "$stage/debian"
printf 'Source: pipeline-monitor\n\nPackage: pipeline-monitor\nArchitecture: any\n' \
  > "$stage/debian/control"
depends=$(cd "$stage" && dpkg-shlibdeps -O "root/$plugin" |
  sed -n 's/^shlibs:Depends=//p')
# The symbols alone ask for less than the GStreamer meson.build requires.
gst=$(sed -n "s/^gst_req = '>= \(.*\)'$/\1/p" meson.build)
depends=$(echo "$depends" |
  sed "s/libgstreamer1\.0-0 ([^)]*)/libgstreamer1.0-0 (>= $gst)/")

mkdir -p "$root/DEBIAN" "$root/usr/share/doc/pipeline-monitor"
cp LICENSE "$root/usr/share/doc/pipeline-monitor/copyright"
cat > "$root/DEBIAN/control" <<EOF
Package: pipeline-monitor
Version: $version-1~$distro
Architecture: $arch
Maintainer: Łukasz Huńczak <lukasz.hunczak@imaginarybit.com>
Depends: $depends
Section: devel
Priority: optional
Homepage: https://github.com/imgbit/pipeline-monitor
Description: See a running GStreamer pipeline live in your browser
 A GStreamer tracer plugin: run a program with GST_TRACERS=pipeline-monitor
 and open the address it prints to see the pipeline graph, caps, live pad
 values, queues, properties, latency, errors and pipeline issues.
EOF

# "." in the file name, not "~": GitHub replaces it in release files.
mkdir -p "$out"
dpkg-deb --root-owner-group --build "$root" \
  "$out/pipeline-monitor_$version-1.${distro}_$arch.deb"
rm -rf "$stage"
