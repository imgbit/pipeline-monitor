#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Builds the published site: the viewer (the page files, as a viewer with
no server behind it), its demo recordings and the guide.

Usage: packaging/build-site.py OUT [--demos DIR] [--require-guide]

OUT gets index.html and the page files; demos/ with the recordings listed
in DIR/demos.json (default: site/demos) and a demos.json for the page with
each one's kind, times and size read from the file; episodes/ with
the recordings of site/episodes (opened by a link, not listed); guide/ with
the pages of site/guide/*.md as HTML (needs the Python "markdown" package: without
it the guide is left out, or with --require-guide the build fails); and for
search engines and link previews: descriptions, social tags, sitemap.xml and
llms.txt.
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
# Where the site is published: links in tags, the sitemap and llms.txt.
SITE = "https://imaginarybit.com/pipeline-monitor/"
SITE_TITLE = "pipeline-monitor: see a running GStreamer pipeline live"
SITE_DESCRIPTION = (
    "A GStreamer tracer that shows a running pipeline live in the browser: "
    "the graph, states and caps, problems such as failed negotiation or "
    "blocked data, queue levels, latency and CPU per thread. Try the demo "
    "recordings, no install needed.")


# Visits counted without cookies (GoatCounter; it does not count localhost).
# Only on the published site: the packages' page loads nothing from outside.
COUNTER = ('<script data-goatcounter="https://imaginarybit.goatcounter.com/'
           'count" async src="https://gc.zgo.at/count.js"></script>')
PRIVACY = ("Visits to this site are counted without cookies (GoatCounter). "
           "Recordings you open stay in your browser and are never sent "
           "anywhere.")


def social(title, description, url):
    """Description, canonical address and link-preview tags for a page."""
    t = html.escape(title, quote=True)
    d = html.escape(description, quote=True)
    return "\n".join([
        f'<meta name="description" content="{d}">',
        f'<link rel="canonical" href="{url}">',
        '<meta property="og:type" content="website">',
        '<meta property="og:site_name" content="pipeline-monitor">',
        f'<meta property="og:title" content="{t}">',
        f'<meta property="og:description" content="{d}">',
        f'<meta property="og:url" content="{url}">',
        f'<meta property="og:image" content="{SITE}social.png">',
        '<meta name="twitter:card" content="summary_large_image">',
    ])


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


GUIDE = [
    ("index", "Get started", "Install pipeline-monitor (Ubuntu, Debian, "
     "macOS, Windows), turn it on with GST_TRACERS=pipeline-monitor and open "
     "a running GStreamer pipeline in your browser."),
    ("tour", "Tour", "What every part of pipeline-monitor's page shows: the "
     "graph, threads, problems, queue levels, pinned pads, latency, "
     "performance and recordings."),
    ("options", "Options", "pipeline-monitor's options: port, recordings, "
     "recording from the start, waiting for the page, and more."),
    ("troubleshooting", "Troubleshooting", "When pipeline-monitor shows no "
     "address, no page or no graph: the usual causes and their fixes."),
]
# Common pipeline problems, one page each (site/guide/problems/NAME.md),
# listed on the guide's "Problems" page.
PROBLEMS = [
    ("tee-without-queues", "Stuck in PAUSED: a tee without queues",
     "A GStreamer pipeline with a tee hangs in PAUSED with no error: why "
     "each branch needs a queue, how to see it, and the fix."),
]

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} - pipeline-monitor guide</title>
{social}
{counter}
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
<br>{privacy}
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
    pages = [(name, title, description, os.path.join(source, f"{name}.md"))
             for name, title, description in GUIDE]
    pages += [(name, title, description,
               os.path.join(source, "problems", f"{name}.md"))
              for name, title, description in PROBLEMS]
    listing = "# Problems\n\nCommon pipeline problems: what you see, why, " \
        "what pipeline-monitor shows, and the fix.\n\n" + "\n".join(
            f"- [{t}]({n}.html): {d}" for n, t, d in PROBLEMS) + "\n"
    pages.append(("problems", "Problems", "Common GStreamer pipeline "
                  "problems: the symptom, the cause, what pipeline-monitor "
                  "shows and the fix.", listing))
    menu = [(n, t) for n, t, _ in GUIDE] + [("problems", "Problems")]
    problem_names = {n for n, _, _ in PROBLEMS}
    for name, title, description, text in pages:
        if os.path.exists(text):
            with open(text, encoding="utf-8") as f:
                text = f.read()
        body = markdown.markdown(text, extensions=["tables", "fenced_code"])
        current = "problems" if name in problem_names else name
        nav = "\n".join(
            f'<a href="{n}.html"' + (' aria-current="page"' if n == current
                                     else "") + f">{html.escape(t)}</a>"
            for n, t in menu)
        with open(os.path.join(target, f"{name}.html"), "w",
                  encoding="utf-8") as f:
            f.write(PAGE.format(
                title=html.escape(title), nav=nav, body=body,
                version=html.escape(version()),
                social=social(f"{title} - pipeline-monitor guide",
                              description, f"{SITE}guide/{name}.html"),
                counter=COUNTER, privacy=html.escape(PRIVACY)))
    return True


def write_sitemap(out, guide):
    """sitemap.xml: the viewer, the guide's pages and the problems."""
    names = [n for n, _, _ in GUIDE] + ["problems"] + \
        [n for n, _, _ in PROBLEMS]
    urls = [SITE] + ([f"{SITE}guide/{n}.html" for n in names] if guide
                     else [])
    with open(os.path.join(out, "sitemap.xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'
                "\n")
        for url in urls:
            f.write(f"  <url><loc>{url}</loc></url>\n")
        f.write("</urlset>\n")


def write_llms(out):
    """llms.txt: a short summary for AI tools (llmstxt.org)."""
    guide = "\n".join(f"- [{t}]({SITE}guide/{n}.html): {d}"
                      for n, t, d in GUIDE)
    problems = "\n".join(f"- [{t}]({SITE}guide/{n}.html): {d}"
                         for n, t, d in PROBLEMS)
    with open(os.path.join(out, "llms.txt"), "w", encoding="utf-8") as f:
        f.write(f"""# pipeline-monitor

> {SITE_DESCRIPTION}

pipeline-monitor is a GStreamer tracer plugin (C, LGPL-2.1-or-later). Turn
it on with the environment variable GST_TRACERS=pipeline-monitor: the
program needs no changes. It serves a page on 127.0.0.1 (the address is
printed) that shows the running pipeline live. It can record a run to a
file that opens in the same viewer, also on this site without installing.

Install: packages for Ubuntu 24.04 and Debian 12 (amd64, arm64), Homebrew
(brew install imgbit/tap/pipeline-monitor), and the official GStreamer on
macOS and Windows; or build from source (GStreamer 1.22 or newer).

## Docs

{guide}

## Common pipeline problems

{problems}

## Links

- [Viewer and demo recordings]({SITE})
- [Source and releases](https://github.com/imgbit/pipeline-monitor)
- [Videos: Fix your GStreamer pipeline](https://www.youtube.com/playlist?list=PLHYOr70akpmI)
""")


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
    page = page.replace('<meta charset="utf-8">',
                        f'<meta charset="utf-8">\n{META}\n'
                        f'{social(SITE_TITLE, SITE_DESCRIPTION, SITE)}', 1)
    page = page.replace("<title>pipeline-monitor</title>",
                        f"<title>{html.escape(SITE_TITLE)}</title>\n{COUNTER}",
                        1)
    license = '<p class="muted">LGPL-2.1-or-later</p>'
    if license not in page:
        sys.exit("index.html: no license line to put the privacy note before")
    page = page.replace(license, f'<p class="muted">{html.escape(PRIVACY)}'
                        f"</p>\n  {license}", 1)
    with open(index, "w", encoding="utf-8") as f:
        f.write(page)
    shutil.copy(os.path.join(ROOT, ".github", "social-preview.png"),
                os.path.join(args.out, "social.png"))

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
    # Recordings shown in the videos, opened by a link (#file=), not listed.
    episodes = os.path.join(ROOT, "site", "episodes")
    if os.path.isdir(episodes):
        shutil.copytree(episodes, os.path.join(args.out, "episodes"))
    guide = build_guide(args.out, args.require_guide)
    write_sitemap(args.out, guide)
    write_llms(args.out)
    print(f"site in {args.out}: {len(listed)} demo(s)"
          + (", the guide" if guide else ""))


if __name__ == "__main__":
    main()
