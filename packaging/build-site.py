#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Builds the published site: the viewer (the page files, as a viewer with
no server behind it) and its demo recordings.

Usage: packaging/build-site.py OUT [--demos DIR]

OUT gets index.html and the page files, and demos/ with the recordings
listed in DIR/demos.json (default: site/demos) and a demos.json for the
page with each one's kind, times and size read from the file.
"""

import argparse
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
META = '<meta name="pipeline-monitor-site" content="demos/demos.json">'


def summary(path):
    """kind, started, last (µs) of a recording, from its lines."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    header = json.loads(lines[0])
    if header.get("format") != "pipeline-monitor-recording":
        sys.exit(f"{path}: not a pipeline-monitor recording")
    last = header["started"]
    for line in lines[1:]:
        try:
            time = json.loads(line).get("time")
        except (ValueError, AttributeError):
            continue
        if isinstance(time, (int, float)):
            last = max(last, time)
    return header.get("kind"), header["started"], last


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out")
    parser.add_argument("--demos", default=os.path.join(ROOT, "site", "demos"))
    args = parser.parse_args()

    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    shutil.copytree(os.path.join(ROOT, "web"), args.out)
    index = os.path.join(args.out, "index.html")
    with open(index, encoding="utf-8") as f:
        page = f.read()
    if '<meta charset="utf-8">' not in page:
        sys.exit("index.html: no charset line to put the site line after")
    with open(index, "w", encoding="utf-8") as f:
        f.write(page.replace('<meta charset="utf-8">',
                             f'<meta charset="utf-8">\n{META}', 1))

    with open(os.path.join(args.demos, "demos.json"), encoding="utf-8") as f:
        wanted = json.load(f)
    out_demos = os.path.join(args.out, "demos")
    os.makedirs(out_demos)
    listed = []
    for demo in wanted:
        source = os.path.join(args.demos, demo["file"])
        shutil.copy(source, os.path.join(out_demos, demo["file"]))
        kind, started, last = summary(source)
        listed.append({"title": demo["title"], "file": demo["file"],
                       "kind": kind, "started": started, "last": last,
                       "size": os.path.getsize(source)})
    with open(os.path.join(out_demos, "demos.json"), "w",
              encoding="utf-8") as f:
        json.dump(listed, f, indent=1)
    print(f"site in {args.out}: {len(listed)} demo(s)")


if __name__ == "__main__":
    main()
