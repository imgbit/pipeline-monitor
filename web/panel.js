// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Details panel for the selected element.

import { countElements } from "./render.js";
import { summarizeCaps } from "./caps.js";
import { capsList } from "./tooltip.js";

// Pads whose full caps are open in the panel, kept across updates.
const expanded = new Set();
// Properties (element id + name) whose structure is open, and the last text
// of every value, to flash the ones that change.
const openProps = new Set();
const lastText = new Map();

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

// The queue section, refreshed with every "levels" message.
export function updateQueueDetails(aside, rows) {
  const section = aside.querySelector(".queue-levels");
  if (!section)
    return;
  const dl = document.createElement("dl");
  for (const [k, v] of rows || []) {
    add(dl, "dt", k);
    add(dl, "dd", v);
  }
  section.replaceChildren(dl);
}

// log: { entries, muted, toggleMute } for the selected element, or null;
// queue: rows of its levels if it is a queue, or null.
export function showDetails(aside, selection, index, onClose, log, queue) {
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
  if (queue) {
    add(aside, "h3", "Queue");
    add(aside, "div", undefined, "queue-levels");
    updateQueueDetails(aside, queue);
  }

  if (element.pads.length > 0)
    showPads(aside, element, index);
  if (log)
    showLog(aside, log);
  add(aside, "h3", "Properties");
  const props = add(aside, "div", "loading…", "properties");
  props.dataset.element = element.id;
}

// One line for any value from /api/properties.
function valueText(v) {
  if (v === null || v === undefined)
    return "—";
  if (Array.isArray(v))
    return v.length ? v.map(valueText).join(", ") : "—";
  if (typeof v !== "object")
    return String(v);
  if ("nick" in v)
    return v.nick;
  if ("flags" in v)
    return v.flags.length ? v.flags.join(", ") : "none";
  if ("caps" in v && !("type" in v))
    return v.caps === null ? "—" : summarizeCaps(v.caps) || v.caps;
  if ("structure" in v)
    return `${v.structure} (${Object.keys(v.fields).length} fields)`;
  if ("object" in v)
    return v.object;
  if (v.type === "GstSample")
    return v.caps ? `sample: ${summarizeCaps(v.caps)}` : "sample";
  return `(${v.type})`;
}

// The fields of a structure, one per line, nested ones indented.
function fieldsList(fields) {
  const dl = document.createElement("dl");
  dl.className = "fields";
  for (const [k, v] of Object.entries(fields)) {
    add(dl, "dt", k);
    const dd = add(dl, "dd");
    if (v && typeof v === "object" && "structure" in v)
      dd.append(v.structure, fieldsList(v.fields));
    else
      dd.textContent = valueText(v);
  }
  return dl;
}

function propertyTitle(p) {
  const lines = [p.blurb, p.type];
  if ("default" in p)
    lines.push(`default: ${valueText(p.default)}`);
  if ("min" in p)
    lines.push(`range: ${p.min} … ${p.max}`);
  if (p.choices)
    lines.push(`choices: ${p.choices.join(", ")}`);
  lines.push(p.writable ? "writable" : "read-only");
  return lines.filter(Boolean).join("\n");
}

// Fills the properties section from a /api/properties answer for the
// element shown; called every second while the panel is open.
export function updateProperties(aside, id, data) {
  const section = aside.querySelector(".properties");
  if (!section || Number(section.dataset.element) !== id)
    return;
  if (!data) {
    section.textContent = "not available";
    return;
  }
  if (data.not_answering) {
    section.textContent = `"${data.not_answering}" did not answer in time; ` +
      "it is skipped from now on.";
    return;
  }
  const table = document.createElement("table");
  table.className = "props";
  for (const p of data.properties) {
    const key = `${id}:${p.name}`;
    const tr = add(table, "tr");
    const name = add(tr, "td", p.name, p.changed ? "changed" : undefined);
    name.title = propertyTitle(p) + (p.changed ? "\nchanged from default"
      : "");
    const cell = add(tr, "td");
    if (p.not_answering) {
      cell.textContent = "not answering";
      cell.className = "muted";
      cell.title = "Its getter did not return in time; skipped from now on";
      continue;
    }
    const text = valueText(p.value);
    const isStruct = p.value && typeof p.value === "object" &&
      "structure" in p.value;
    if (isStruct) {
      const toggle = add(cell, "button", text, "caps-toggle");
      toggle.setAttribute("aria-expanded", String(openProps.has(key)));
      toggle.addEventListener("click", () => {
        if (openProps.has(key))
          openProps.delete(key);
        else
          openProps.add(key);
        updateProperties(aside, id, data);
      });
      if (openProps.has(key))
        cell.appendChild(fieldsList(p.value.fields));
    } else {
      cell.textContent = text;
      if (typeof p.value === "object" && p.value?.caps)
        cell.title = p.value.caps;
    }
    // Flash what changed since the last refresh.
    const full = JSON.stringify(p.value);
    if (lastText.has(key) && lastText.get(key) !== full)
      cell.classList.add("flash");
    lastText.set(key, full);
  }
  section.replaceChildren(table);
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
