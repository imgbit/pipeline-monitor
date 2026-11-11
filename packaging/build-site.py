#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Builds the published site: the viewer (the page files, as a viewer with
no server behind it), its demo recordings and the guide.

Usage: packaging/build-site.py OUT [--demos DIR] [--require-guide]

OUT gets index.html and the page files; demos/ with the recordings listed
in DIR/demos.json (default: site/demos) and a demos.json for the page with
each one's kind, times and size read from the file; guide/ with the pages
of site/guide/*.md as HTML (needs the Python "markdown" package: without
it the guide is left out, or with --require-guide the build fails).
"""

import argparse
import html
import json
import os
import re
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


GUIDE = [("index", "Get started"), ("tour", "Tour"), ("options", "Options"),
         ("troubleshooting", "Troubleshooting")]

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} - pipeline-monitor guide</title>
<link rel="icon" href="../icon.svg">
<link rel="stylesheet" href="guide.css">
</head>
<body>
<header>
<a class="brand" href="index.html">pipeline-monitor guide</a>
{nav}
<a class="viewer" href="../">Open the viewer</a>
</header>
<main>
{body}
</main>
<footer>pipeline-monitor {version} &middot;
<a href="https://github.com/imgbit/pipeline-monitor">source on GitHub</a>
</footer>
</body>
</html>
"""


def version():
    with open(os.path.join(ROOT, "meson.build"), encoding="utf-8") as f:
        found = re.search(r"version\s*:\s*'([^']+)'", f.read())
    return found.group(1) if found else ""


def build_guide(out, require):
    """guide/: site/guide/*.md as HTML pages, with their images."""
    try:
        import markdown
    except ImportError:
        if require:
            sys.exit("the guide needs the Python markdown package")
        print("guide left out: no Python markdown package")
        return False
    source = os.path.join(ROOT, "site", "guide")
    target = os.path.join(out, "guide")
    os.makedirs(target)
    shutil.copy(os.path.join(source, "guide.css"), target)
    if os.path.isdir(os.path.join(source, "images")):
        shutil.copytree(os.path.join(source, "images"),
                        os.path.join(target, "images"))
    for name, title in GUIDE:
        with open(os.path.join(source, f"{name}.md"), encoding="utf-8") as f:
            body = markdown.markdown(f.read(), extensions=["tables",
                                                           "fenced_code"])
        nav = "\n".join(
            f'<a href="{n}.html"' + (' aria-current="page"' if n == name
                                     else "") + f">{html.escape(t)}</a>"
            for n, t in GUIDE)
        with open(os.path.join(target, f"{name}.html"), "w",
                  encoding="utf-8") as f:
            f.write(PAGE.format(title=html.escape(title), nav=nav, body=body,
                                version=html.escape(version())))
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out")
    parser.add_argument("--demos", default=os.path.join(ROOT, "site", "demos"))
    parser.add_argument("--require-guide", action="store_true")
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
                       "description": demo.get("description", ""),
                       "kind": kind, "started": started, "last": last,
                       "size": os.path.getsize(source)})
    with open(os.path.join(out_demos, "demos.json"), "w",
              encoding="utf-8") as f:
        json.dump(listed, f, indent=1)
    guide = build_guide(args.out, args.require_guide)
    print(f"site in {args.out}: {len(listed)} demo(s)"
          + (", the guide" if guide else ""))


if __name__ == "__main__":
    main()
