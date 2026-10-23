// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Findings: problems the plugin found in the pipeline, the header counter
// and the findings panel.

import { parseCaps } from "./caps.js";

function add(parent, tag, content, cls) {
  const node = document.createElement(tag);
  if (content !== undefined)
    node.textContent = content;
  if (cls)
    node.className = cls;
  parent.appendChild(node);
  return node;
}

function plural(n, word) {
  return `${n} ${word}${n === 1 ? "" : "s"}`;
}

function ago(us) {
  const s = Math.max(0, Math.round((Date.now() * 1000 - us) / 1e6));
  return s < 60 ? `${s} s ago` : `${Math.round(s / 60)} min ago`;
}

// One side of a format problem: media type and fields, the ones that
// differ from the other side marked.
function capsColumn(title, caps, other) {
  const box = document.createElement("div");
  box.className = "finding-caps-side";
  add(box, "div", title, "finding-caps-title");
  if (!caps) {
    add(box, "div", "—", "muted");
    return box;
  }
  const mine = parseCaps(caps);
  const theirs = other ? parseCaps(other) : null;
  const media = add(box, "div", mine.features
    ? `${mine.media}(${mine.features})` : mine.media, "caps-media");
  if (theirs && theirs.media !== mine.media)
    media.classList.add("differs");
  const values = new Map(theirs ? theirs.fields : []);
  const dl = add(box, "dl");
  for (const [name, value] of mine.fields) {
    const differs = values.has(name) && values.get(name) !== value;
    add(dl, "dt", name, differs ? "differs" : undefined);
    add(dl, "dd", value, differs ? "differs" : undefined);
  }
  box.title = caps;
  return box;
}

export class FindingsView {
  // nodes: {counter, panel, rows, close}; handlers: select(elementId),
  // name(findingElementId) -> short element name for the place button.
  constructor(nodes, handlers) {
    this.nodes = nodes;
    this.handlers = handlers;
    this.list = [];
    nodes.counter.addEventListener("click",
      () => this.setOpen(nodes.panel.hidden));
    nodes.close.addEventListener("click", () => this.setOpen(false));
  }

  setOpen(open) {
    this.nodes.panel.hidden = !open;
    this.nodes.counter.setAttribute("aria-expanded", String(open));
  }

  // Findings by place, for the graph and tooltips: {pads: Map pad id ->
  // [finding], elements: Map element id -> [finding]}.
  byPlace() {
    const pads = new Map();
    const elements = new Map();
    for (const f of this.list) {
      const [map, key] = f.pad ? [pads, f.pad] : [elements, f.element];
      if (!map.has(key))
        map.set(key, []);
      map.get(key).push(f);
    }
    return { pads, elements };
  }

  apply(findings) {
    this.list = findings.findings;
    this.render();
  }

  render() {
    const { counter, rows } = this.nodes;
    const errors = this.list.filter((f) => f.severity === "error").length;
    const warnings = this.list.filter((f) => f.severity === "warning").length;
    counter.textContent = this.list.length
      ? plural(this.list.length, "problem") : "No problems";
    counter.className = "problems-counter" + (errors ? " has-errors"
      : warnings ? " has-warnings" : "");

    if (!this.list.length) {
      rows.replaceChildren();
      add(rows, "div", "No problems found.", "log-empty");
      return;
    }
    const fragment = document.createDocumentFragment();
    for (const f of this.list) {
      const row = add(fragment, "div", undefined, `finding ${f.severity}`);
      add(row, "span", f.severity, "finding-severity");
      const body = add(row, "div", undefined, "finding-body");
      const text = add(body, "div", undefined, "finding-text");
      const place = add(text, "button", f.text, "finding-place");
      place.title = "Show it in the graph";
      // On press: the list can be rebuilt between press and release.
      place.addEventListener("pointerdown", (e) => {
        if (e.button === 0) {
          e.preventDefault();
          this.handlers.select(f.element);
        }
      });
      place.addEventListener("click", (e) => {
        if (e.detail === 0)
          this.handlers.select(f.element);
      });
      if (f.hint)
        add(body, "div", f.hint, "finding-hint");
      if (f.caps) {
        const caps = add(body, "div", undefined, "finding-caps");
        caps.append(capsColumn("upstream offers", f.caps.upstream,
          f.caps.downstream), capsColumn("downstream accepts",
          f.caps.downstream, f.caps.upstream));
        if (f.caps.common === false && f.caps.upstream && f.caps.downstream)
          add(body, "div", "Nothing in common: add a converter " +
            "(videoconvert, audioconvert, …) or change the caps on one side.",
          "finding-hint");
      }
      add(row, "span", f.count ? `${f.count}× · ${ago(f.last)}` : "",
        "finding-count");
    }
    rows.replaceChildren(fragment);
  }
}
