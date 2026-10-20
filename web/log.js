// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Errors and warnings: the browser's copy of the log, the header counter and
// the log panel. Clear and mute only affect this browser.

const MAX_ENTRIES = 500;
const MUTED_KEY = "pipeline-monitor.muted";

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

export class LogView {
  constructor(nodes, handlers) {
    this.nodes = nodes;
    this.handlers = handlers;
    this.enabled = true;
    this.entries = new Map();
    this.clearedCounts = new Map();
    this.clearedElements = new Map();
    this.muted = this.loadMuted();

    nodes.counter.addEventListener("click", () => this.setOpen(nodes.panel.hidden));
    nodes.close.addEventListener("click", () => this.setOpen(false));
    nodes.clear.addEventListener("click", () => handlers.clear());
  }

  loadMuted() {
    try {
      return new Set(JSON.parse(localStorage.getItem(MUTED_KEY)) || []);
    } catch {
      return new Set();
    }
  }

  saveMuted() {
    try {
      localStorage.setItem(MUTED_KEY, JSON.stringify([...this.muted]));
    } catch {
      // Storage may be unavailable; muting still works for this session.
    }
  }

  // A new connection starts from the server's full log.
  reset() {
    this.entries.clear();
    this.clearedCounts.clear();
    this.clearedElements.clear();
  }

  apply(log) {
    this.enabled = log.enabled;
    for (const entry of log.entries) {
      this.entries.delete(entry.id);
      this.entries.set(entry.id, entry);
    }
    while (this.entries.size > MAX_ENTRIES)
      this.entries.delete(this.entries.keys().next().value);
    this.render();
  }

  // Hides everything seen so far; elementCounts maps element id to its
  // current {errors, warnings} so badges also start again from zero.
  clear(elementCounts) {
    for (const entry of this.entries.values())
      this.clearedCounts.set(entry.id, entry.count);
    this.clearedElements = new Map(elementCounts);
    this.render();
  }

  isMuted(path) {
    return this.muted.has(path);
  }

  setMuted(path, muted) {
    if (muted)
      this.muted.add(path);
    else
      this.muted.delete(path);
    this.saveMuted();
    this.render();
    this.handlers.changed();
  }

  setOpen(open) {
    this.nodes.panel.hidden = !open || !this.enabled;
    this.nodes.counter.setAttribute("aria-expanded", String(open));
  }

  newCount(entry) {
    return entry.count - (this.clearedCounts.get(entry.id) || 0);
  }

  // Entries to show, newest first.
  visible() {
    return [...this.entries.values()]
      .filter((e) => this.newCount(e) > 0 && !this.muted.has(e.path))
      .sort((a, b) => b.last - a.last);
  }

  // An element's entries, newest first, counted since the last clear.
  entriesFor(elementId) {
    return [...this.entries.values()]
      .filter((e) => e.element === elementId && this.newCount(e) > 0)
      .sort((a, b) => b.last - a.last)
      .map((e) => ({ ...e, count: this.newCount(e) }));
  }

  // Badge counts for an element since the last clear, or null for none.
  badge(id, path, errors = 0, warnings = 0) {
    const base = this.clearedElements.get(id) || { errors: 0, warnings: 0 };
    const e = Math.max(0, errors - base.errors);
    const w = Math.max(0, warnings - base.warnings);
    if (e === 0 && w === 0)
      return null;
    return { errors: e, warnings: w, muted: this.muted.has(path) };
  }

  render() {
    const { counter, panel, rows, mutedList } = this.nodes;
    let errors = 0;
    let warnings = 0;
    const shown = this.visible();

    for (const e of shown) {
      if (e.level === "error")
        errors += this.newCount(e);
      else
        warnings += this.newCount(e);
    }

    counter.hidden = !this.enabled;
    if (!this.enabled)
      panel.hidden = true;
    counter.className = "log-counter" +
      (errors ? " has-errors" : warnings ? " has-warnings" : "");
    counter.textContent = errors || warnings
      ? [errors && plural(errors, "error"), warnings &&
          plural(warnings, "warning")].filter(Boolean).join(" · ")
      : "No errors";

    mutedList.replaceChildren();
    if (this.muted.size) {
      add(mutedList, "span", "Muted:", "muted");
      for (const path of this.muted) {
        const b = add(mutedList, "button", `${path.split("/").pop()} ×`,
          "unmute");
        b.title = `Unmute ${path}`;
        b.addEventListener("click", () => this.setMuted(path, false));
      }
    }

    rows.replaceChildren();
    if (shown.length === 0) {
      add(rows, "div", "Nothing to show.", "log-empty");
    } else {
      const head = add(rows, "div", undefined, "log-columns");
      for (const name of ["time", "level", "element", "message", "count", ""])
        add(head, "span", name);
    }
    for (const e of shown)
      rows.appendChild(this.row(e));
  }

  row(e) {
    const row = document.createElement("div");
    row.className = `log-row ${e.level}`;
    row.dataset.id = e.id;
    row.title = [e.debug, e.domain && `${e.domain} ${e.code}`,
      `${e.source}, first at ${new Date(e.first).toLocaleTimeString()}`]
      .filter(Boolean).join("\n");

    add(row, "span", new Date(e.last).toLocaleTimeString(), "log-time");
    add(row, "span", e.level, "log-level");
    const where = add(row, "button", e.path.split("/").pop(), "log-path");
    where.title = `${e.path}\nShow in the graph`;
    where.addEventListener("click", () => this.handlers.select(e.element));
    add(row, "span", e.text, "log-text");
    const n = this.newCount(e);
    add(row, "span", n > 1 ? `×${n}` : "", "log-count");
    const mute = add(row, "button", "mute", "log-mute");
    mute.title = `Hide messages from ${e.path}`;
    mute.addEventListener("click", () => this.setMuted(e.path, true));
    return row;
  }
}
