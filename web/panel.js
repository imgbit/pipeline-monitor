// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Details panel for the selected element.

import { countElements } from "./render.js";
import { summarizeCaps } from "./caps.js";
import { capsList } from "./tooltip.js";

// Pads whose full caps are open in the panel, kept across updates.
const expanded = new Set();

function add(parent, tag, content, cls) {
  const node = document.createElement(tag);
  if (content !== undefined)
    node.textContent = content;
  if (cls)
    node.className = cls;
  parent.appendChild(node);
  return node;
}

function showLog(aside, log) {
  const button = add(aside, "button", log.muted ? "Unmute" : "Mute",
    "mute-toggle");
  button.title = log.muted ? "Show this element's errors and warnings again"
    : "Hide this element's errors and warnings (this browser only)";
  button.addEventListener("click", log.toggleMute);
  if (log.entries.length === 0)
    return;
  const list = add(aside, "ul", undefined, "log-list");
  for (const e of log.entries.slice(0, 20)) {
    const li = add(list, "li", undefined, e.level);
    li.textContent = `${new Date(e.last).toLocaleTimeString()} ${e.text}` +
      (e.count > 1 ? ` ×${e.count}` : "");
    li.title = e.debug || "";
  }
}

// log: { entries, muted, toggleMute } for the selected element, or null.
export function showDetails(aside, selection, index, onClose, log) {
  if (!selection) {
    aside.hidden = true;
    aside.replaceChildren();
    return;
  }

  const { element, path } = selection;
  aside.replaceChildren();
  aside.hidden = false;

  const close = add(aside, "button", "×", "close");
  close.title = "Close";
  close.addEventListener("click", onClose);
  add(aside, "h2", element.name);

  const dl = add(aside, "dl");
  const row = (k, v) => {
    add(dl, "dt", k);
    add(dl, "dd", v);
  };
  row("factory", element.factory || "—");
  row("type", element.type);
  row("state", element.pending ? `${element.state} → ${element.pending}`
    : element.state);
  row("path", path);
  if (element.bin)
    row("contains", `${countElements(element)} elements`);

  if (element.pads.length > 0)
    showPads(aside, element, index);
  if (log)
    showLog(aside, log);
}

function showPads(aside, element, index) {
  const table = add(aside, "table");
  const head = add(table, "tr");
  for (const h of ["pad", "dir", "presence", "linked to", "caps"])
    add(head, "th", h);
  for (const pad of element.pads) {
    const tr = add(table, "tr");
    add(tr, "td", pad.ghost ? `${pad.name} (ghost)` : pad.name);
    add(tr, "td", pad.direction);
    add(tr, "td", pad.presence || "—");
    const peers = index.peers.get(pad.id) || [];
    const linked = add(tr, "td", peers.length ? undefined : "—");
    peers.forEach((id, i) => {
      // Lets a long "element:pad" wrap after the colon.
      const [element, padName] = index.padName(id).split(/:(?=[^:]*$)/);
      if (i > 0)
        linked.append(", ");
      linked.append(`${element}:`, document.createElement("wbr"), padName);
    });
    const cell = add(tr, "td", undefined, "caps");
    if (!pad.caps) {
      cell.textContent = "—";
      continue;
    }
    const toggle = add(cell, "button", summarizeCaps(pad.caps), "caps-toggle");
    toggle.title = "Show the full caps";
    const full = document.createElement("tr");
    full.className = "caps-full";
    const td = add(full, "td");
    td.colSpan = 5;
    td.appendChild(capsList(pad.caps));
    const set = (open) => {
      toggle.setAttribute("aria-expanded", String(open));
      if (open)
        tr.after(full);
      else
        full.remove();
    };
    toggle.addEventListener("click", () => {
      if (expanded.has(pad.id))
        expanded.delete(pad.id);
      else
        expanded.add(pad.id);
      set(expanded.has(pad.id));
    });
    set(expanded.has(pad.id));
  }
}
