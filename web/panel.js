// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Details panel for the selected element.

import { countElements } from "./render.js";
import { summarizeCaps } from "./caps.js";
import { capsList, tip } from "./tooltip.js";
import { formatSpan } from "./units.js";
import { appStreamRows, appsinkLinks, appsrcLinks, linkRows }
  from "./levels.js";

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
    li.textContent = `${new Date(e.last / 1000).toLocaleTimeString()} ${e.text}` +
      (e.count > 1 ? ` ×${e.count}` : "");
    tip(li, e.debug);
  }
}

// The element's problems (its own and its pads'), under its name; list:
// findings, empty to hide the section.
export function updateProblemsDetails(aside, list) {
  const section = aside.querySelector(".element-problems");
  if (!section)
    return;
  section.hidden = !list.length;
  section.replaceChildren();
  for (const f of list) {
    const row = add(section, "div", undefined, `element-problem ${f.severity}`);
    add(row, "div", f.text, "element-problem-text");
    if (f.hint)
      add(row, "div", f.hint, "element-problem-hint");
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

// appsrc's and appsink's "Stream" section, refreshed with every "levels"
// message.
export function updateAppStream(aside, queue) {
  const section = aside.querySelector(".app-stream");
  if (!section || Number(section.dataset.element) !== queue?.element ||
      !("segment" in queue))
    return;
  const dl = document.createElement("dl");
  for (const [k, v] of appStreamRows(queue)) {
    add(dl, "dt", k);
    add(dl, "dd", v);
  }
  section.replaceChildren(dl);
}

// The appsink an appsrc was set to be compared with, by appsrc id.
const comparing = new Map();

// sink, src: the two elements' paths, for the column headers' tooltips.
function linkTable(parent, rows, sink, src) {
  const table = add(parent, "table", undefined, "app-pair");
  const head = add(table, "tr");
  add(head, "th", "");
  tip(add(head, "th", "into appsink"), "As the buffer went into " +
    `${sink ?? "the appsink"}: read as it was pushed into its sink pad.`);
  tip(add(head, "th", "out of appsrc"), "As the same buffer left " +
    `${src ?? "the appsrc"}: read as appsrc pushed it out of its src pad.`);
  for (const [label, a, b, differs, meaning] of rows) {
    const tr = add(table, "tr", undefined, differs ? "differs" : undefined);
    tip(add(tr, "td", label, "muted"), meaning);
    add(tr, "td", a);
    add(tr, "td", b);
  }
}

function ago(us) {
  const s = Math.max(0, us) / 1e6;
  return s < 1 ? "just now" : `${s < 10 ? s.toFixed(1) : Math.round(s)} s ago`;
}

// "Data from appsink" (appsrc) and "Data to appsrc" (appsink), refreshed
// with every "levels" message. now: the message's time; jump(id) shows
// that element.
export function updateAppLink(aside, queue, queues, now, jump,
  pipelines = []) {
  const pipelineOf = (path) => pipelines.find((p) => p.path ===
    (path ?? "").split("/")[0]);
  const section = aside.querySelector(".app-link");
  if (!section || Number(section.dataset.element) !== queue?.element)
    return;
  let body = section.querySelector(".app-link-body");
  if (!body)
    body = add(section, "div", undefined, "app-link-body");
  const place = (parent, element, path) => {
    const button = add(parent, "button", path ?? "an element now gone",
      "app-link-place");
    button.title = "Show it in the graph";
    onPress(button, () => jump(element));
  };

  if (section.dataset.kind === "appsink") {
    body.replaceChildren();
    const found = appsinkLinks(queue.element, queues, now);
    if (!found.length)
      tip(add(body, "div", "none found", "muted"), "No appsrc was seen " +
        "sending this appsink's buffers on: the program may copy the data, " +
        "or not pass it on.");
    for (const f of found) {
      const row = add(body, "div");
      place(row, f.element, f.path);
      add(row, "span", " · detected from the data", "muted");
    }
    return;
  }

  const { current, old } = appsrcLinks(queue, now);
  const shown = current.length ? current : old.slice(0, 1);
  const sinks = [...queues.values()].filter((q) => q.kind === "appsink");
  let compare = section.querySelector(".app-compare");
  if (!compare) {
    compare = add(section, "label", "Compare with ", "app-compare");
    const select = add(compare, "select");
    select.addEventListener("change", () => {
      comparing.set(queue.element, Number(select.value) || null);
      updateAppLink(aside, queue, queues, now, jump, pipelines);
    });
  }
  const select = compare.querySelector("select");
  // Rebuilt only when the appsinks change: it would close under the
  // pointer.
  const options = sinks.map((s) => s.element).join();
  if (select.dataset.options !== options) {
    select.dataset.options = options;
    select.replaceChildren();
    add(select, "option", "—").value = "";
    for (const s of sinks)
      add(select, "option", s.path).value = s.element;
  }
  // Found from the data, or chosen: never both.
  compare.hidden = shown.length > 0 || !sinks.length;
  const chosen = compare.hidden ? null : comparing.get(queue.element);
  select.value = chosen ?? "";

  body.replaceChildren();
  // Shared links of the same buffer (a tee): one table, all its appsinks.
  const groups = [];
  for (const f of shown) {
    const same = f.shared && groups.find((g) => g[0].shared &&
      g[0].in_pts === f.in_pts && g[0].out_pts === f.out_pts);
    if (same)
      same.push(f);
    else
      groups.push([f]);
  }
  for (const group of groups) {
    const f = group[0];
    const row = add(body, "div");
    group.forEach((g, i) => {
      if (i)
        add(row, "span", ", ");
      place(row, g.element, g.path);
    });
    add(row, "span", ` · detected from the data, seen ${ago(now - f.last)}`,
      "muted");
    if (f.shared) {
      add(row, "span", " · ", "muted");
      tip(add(row, "span", "shared", "muted"), "The same buffers went " +
        `into ${group.length > 1 ? "these appsinks" : "another appsink too"}` +
        ": which one the program read them from is not known.");
    }
    linkTable(body, [...linkRows({ pts: f.in_pts, segment: f.in_segment,
      pipeline: pipelineOf(f.path) }, { pts: f.out_pts,
      segment: f.out_segment, pipeline: pipelineOf(queue.path) }, true),
    ["in between", "", formatSpan(f.delay, "auto"), false,
      "How long the buffer took from the appsink to leaving appsrc: the " +
      "time in the program and in appsrc's queue."]],
    group.map((g) => g.path).join(", "), queue.path);
  }
  if (!shown.length) {
    const none = add(body, "div", undefined, "muted");
    tip(add(none, "span", "none found"), sinks.length
      ? "No appsink's buffers were seen leaving this appsrc: the program " +
        "may copy the data, or make its own. Choose an appsink below to " +
        "compare with it."
      : "This program has no appsink.");
    const sink = chosen && queues.get(chosen);
    if (sink && queue.segment !== undefined) {
      const chosenRow = add(body, "div", `${sink.path} · `, "app-chosen");
      tip(add(chosenRow, "span", "chosen by you"), "Its last buffer and " +
        "segment against appsrc's segment: not shown to be the same data.");
      linkTable(body, linkRows({ pts: sink.last_pts, segment: sink.segment,
        pipeline: pipelineOf(sink.path) }, { segment: queue.segment,
        pipeline: pipelineOf(queue.path) }, false), sink.path, queue.path);
    }
  }
}

// Runs fn when the button is pressed. Parts of the panel are rebuilt
// several times per second; a click (press and release on the same
// button) would be lost when the button is replaced in between.
function onPress(button, fn) {
  button.addEventListener("pointerdown", (e) => {
    if (e.button === 0) {
      e.preventDefault();
      fn();
    }
  });
  // Keyboard (Enter, Space) still clicks.
  button.addEventListener("click", (e) => {
    if (e.detail === 0)
      fn();
  });
}

// The latency section: a measure switch and the rows of latencyRows.
// latency: {on, toggle(), rows, measurable, all} for the element shown;
// with all (every element measured) the switch is off and greyed. The
// button is kept between updates (4 per second) and only changes its text.
export function updateLatencyDetails(aside, latency) {
  const section = aside.querySelector(".latency-section");
  if (!section || !latency)
    return;
  let button = section.querySelector(".measure-toggle");
  if (!button) {
    button = document.createElement("button");
    button.className = "measure-toggle";
    onPress(button, () => button.toggle());
    section.appendChild(button);
  }
  button.toggle = latency.toggle;
  button.hidden = !latency.measurable;
  button.disabled = Boolean(latency.all);
  const on = latency.on && !latency.all;
  button.textContent = on ? "Stop measuring" : "Measure";
  button.title = latency.all ? "Every element is measured: latency under " +
    "the graph is on"
    : on ? "Remove the probes again"
      : "Time how long data takes through this element (adds pad probes)";
  button.setAttribute("aria-pressed", String(on));
  const dl = document.createElement("dl");
  for (const [k, v] of latency.rows) {
    if (k === null) {
      add(dl, "dd", v, "latency-note");
      continue;
    }
    add(dl, "dt", k);
    add(dl, "dd", v);
  }
  section.querySelector("dl")?.remove();
  section.appendChild(dl);
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
  add(aside, "div", undefined, "element-problems").hidden = true;

  const dl = add(aside, "dl");
  const row = (k, v) => {
    add(dl, "dt", k);
    add(dl, "dd", v);
  };
  row("factory", element.factory ? element.factory_looked_up
    ? `${element.factory} (looked up from the type)` : element.factory
    : "—");
  row("type", element.type);
  row("state", element.pending ? `${element.state} → ${element.pending}`
    : element.state);
  if (element.preroll)
    row("preroll", PREROLL[element.preroll] || element.preroll);
  row("path", path);
  if (element.bin)
    row("contains", `${countElements(element)} elements`);
  if (APP_SETTINGS[element.factory]) {
    add(aside, "h3", "Stream");
    add(aside, "div", undefined, "app-stream").dataset.element = element.id;
    add(aside, "h3", element.factory === "appsrc" ? "Data from appsink"
      : "Data to appsrc");
    const link = add(aside, "div", undefined, "app-link");
    link.dataset.element = element.id;
    link.dataset.kind = element.factory;
  }
  if (queue) {
    add(aside, "h3", ["appsrc", "appsink"].includes(element.factory)
      ? `${element.factory} queue` : "Queue");
    add(aside, "div", undefined, "queue-levels");
    updateQueueDetails(aside, queue);
  }

  if (element.pads.length > 0)
    showPads(aside, element, index);
  if (log)
    showLog(aside, log);
  add(aside, "h3", "Latency");
  const lat = add(aside, "div", undefined, "latency-section");
  lat.dataset.element = element.id;
  add(aside, "h3", "Properties");
  const props = add(aside, "div", "loading…", "properties");
  props.dataset.element = element.id;
  props.dataset.factory = element.factory ?? "";
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

export { valueText };

// 64-bit integers named like a time hold nanoseconds (GstClockTime).
const TIME_NAME = /(^|-)(time|latency|duration|delay|deadline)$|^ts-offset$/;

export function isTimeProperty(p) {
  return (p.type === "guint64" || p.type === "gint64") &&
    TIME_NAME.test(p.name) && typeof p.value === "number";
}

// A property's value as text; times in a readable unit.
export function propertyText(p) {
  if (!isTimeProperty(p))
    return valueText(p.value);
  // -1 and the largest value both mean "none" (GST_CLOCK_TIME_NONE).
  if (p.value === -1 || p.value >= 1.8e19)
    return "none";
  return formatSpan(p.value, "auto");
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

// A sink's (or a bin with sinks) preroll, from its own async messages.
const PREROLL = {
  waiting: "waiting for data",
  done: "done",
  eos: "done on end of stream",
  live: "not needed (live)",
  "no-wait": "not waited for",
};

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

const PIN_ICON = '<svg viewBox="0 0 12 12" aria-hidden="true">' +
  '<path d="M4.5 1.5h3M5 1.5v3.5L3 7h6L7 5V1.5M6 7v3.5"/></svg>';

// Fills the properties section from a /api/properties answer for the
// element shown; called every second while the panel is open. pinning:
// {has(name), toggle(name)} for pinning properties to a card.
// The settings that decide how appsrc and appsink behave, each with what
// it does (read in GStreamer's source, measured with
// tests/appsrc_scenarios.py). meaning(value as text, raw value) -> text.
const APP_SETTINGS = {
  appsrc: [
    ["format", (v) => v === "bytes" ? "segments in bytes: buffer " +
      "timestamps are read as byte positions (for audio and video: time)"
      : v === "time" ? "segments in time" : `segments in ${v}`],
    ["is-live", (v) => v === "true" ? "live: its data runs with the clock"
      : "not live"],
    ["do-timestamp", (v) => v === "true" ? "fills in missing timestamps " +
      "with the running time when it pushes; never changes ones you set"
      : "leaves timestamps as pushed: a buffer without one cannot be placed " +
      "in time"],
    ["handle-segment-change", (v) => v === "true" ? "uses the segment of " +
      "samples pushed with push-sample (with format=time)"
      : "ignores the segment of samples pushed: its own starts at 0"],
    ["min-latency", (v, raw) => raw === -1 ? "not set: it reports a latency of " +
      "0, which leaves a sink no room for its own processing deadline"
      : "the latency it reports"],
    ["block", (v) => v === "true" ? "the program's push waits while the " +
      "queue is full" : "the program's push never waits"],
    ["leaky-type", (v) => v === "none" ? "keeps every buffer"
      : `drops buffers (${v}) when the queue is full`],
    ["caps", () => "what it announces; a pushed sample's caps replace it"],
  ],
  appsink: [
    ["sync", (v) => v === "true" ? "waits for each buffer's time on the " +
      "clock before the program gets it" : "gives buffers as they come"],
    ["drop", (v) => v === "true" ? "drops the oldest when the queue is full"
      : "when the queue is full, waits for the program to pull"],
    ["leaky-type", (v) => v === "none" ? "keeps every buffer"
      : `drops buffers (${v}) when the queue is full`],
    ["wait-on-eos", (v) => v === "true" ? "at the end, waits until the " +
      "program has pulled everything" : "ends without waiting"],
    ["caps", () => "what it accepts"],
  ],
};

// The appsrc or appsink settings section, from the element's properties.


export function updateProperties(aside, id, data, pinning) {
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
    const pinCell = add(tr, "td", undefined, "pin-cell");
    if (pinning && !p.not_answering) {
      const pin = add(pinCell, "button", undefined, "pin-prop");
      pin.innerHTML = PIN_ICON;
      const on = pinning.has(p.name);
      pin.classList.toggle("on", on);
      pin.title = on ? "Unpin" : "Pin: show it live next to the element";
      onPress(pin, () => {
        pinning.toggle(p.name);
        updateProperties(aside, id, data, pinning);
      });
    }
    const name = add(tr, "td", p.name, p.changed ? "changed" : undefined);
    // For appsrc's and appsink's settings that decide timing and queueing:
    // what this value does, in the same tooltip.
    const meaning = !p.not_answering && APP_SETTINGS[section.dataset.factory]
      ?.find(([n]) => n === p.name)?.[1];
    tip(name, propertyTitle(p) + (p.changed ? "\nchanged from default"
      : "") + (meaning ? `\n\n${p.name} ${propertyText(p)}: ` +
      meaning(propertyText(p), p.value) : ""));
    const cell = add(tr, "td");
    if (p.not_answering) {
      cell.textContent = "not answering";
      cell.className = "muted";
      tip(cell, "Its getter did not return in time; skipped from now on");
      continue;
    }
    const text = valueText(p.value);
    const isStruct = p.value && typeof p.value === "object" &&
      "structure" in p.value;
    if (isStruct) {
      const toggle = add(cell, "button", text, "caps-toggle");
      toggle.setAttribute("aria-expanded", String(openProps.has(key)));
      onPress(toggle, () => {
        if (openProps.has(key))
          openProps.delete(key);
        else
          openProps.add(key);
        updateProperties(aside, id, data, pinning);
      });
      if (openProps.has(key))
        cell.appendChild(fieldsList(p.value.fields));
    } else {
      cell.textContent = propertyText(p);
      if (isTimeProperty(p))
        tip(cell, `${p.value} ns`);
      else if (typeof p.value === "object" && p.value?.caps)
        tip(cell, p.value.caps);
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
  add(aside, "h3", "Pads");
  const table = add(aside, "table");
  const head = add(table, "tr");
  for (const h of ["pad", "dir", "mode", "presence", "linked to", "caps"])
    add(head, "th", h);
  for (const pad of element.pads) {
    const tr = add(table, "tr");
    add(tr, "td", pad.ghost ? `${pad.name} (ghost)` : pad.name);
    add(tr, "td", pad.direction);
    add(tr, "td", pad.mode || "—");
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
      if (pad.mode === "pull")
        tip(cell, "No caps: data is pulled");
      continue;
    }
    const toggle = add(cell, "button", summarizeCaps(pad.caps), "caps-toggle");
    toggle.title = "Show the full caps";
    const full = document.createElement("tr");
    full.className = "caps-full";
    const td = add(full, "td");
    td.colSpan = 6;
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
