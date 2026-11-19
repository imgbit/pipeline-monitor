#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Łukasz Huńczak
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Writes the table the page uses to name an element's factory in a .dot
file, which holds only its type: web/factories.json, from the registry of
the GStreamer on PATH (every plugin it has).

Usage: packaging/make-factory-table.py [OUT]

A type made by exactly one factory maps to its name; a type several
factories make maps to null (not known which); types no factory makes are
not listed.
"""

import json
import os
import sys
from collections import defaultdict

import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "web", "factories.json")
    Gst.init(None)
    by_type = defaultdict(set)
    for feature in Gst.Registry.get().get_feature_list(Gst.ElementFactory):
        factory = feature.load()
        if factory is None:
            continue
        by_type[factory.get_element_type().name].add(factory.get_name())
    types = {t: (next(iter(f)) if len(f) == 1 else None)
             for t, f in sorted(by_type.items())}
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"gstreamer": Gst.version_string(), "types": types}, f,
                  ensure_ascii=False, separators=(",", ":"))
        f.write("\n")
    shared = sum(1 for v in types.values() if v is None)
    print(f"{out}: {len(types)} types, {shared} shared by several factories")


if __name__ == "__main__":
    main()
