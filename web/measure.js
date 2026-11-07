// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

import { latencyLabel } from "./latency.js";

const MEASURING_KEY = "pipeline-monitor.measuring";
const MEASURE_ALL_KEY = "pipeline-monitor.measure-all";

function load(key, fallback) {
  try {
    return JSON.parse(localStorage.getItem(key)) ?? fallback;
  } catch {
    return fallback;
  }
}

function save(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // Storage may be unavailable; measuring still works for this page.
  }
}

// The pads data comes from into pad: those linked into it, else (for an
// element's src pad) the element's sink pads.
function upstreamOf(index, id) {
  const into = index.linksInto.get(id);
  if (into?.length)
    return into;
  const p = index.pads.get(id);
  return p?.pad.direction === "src"
    ? p.element.pads.filter((x) => x.direction === "sink").map((x) => x.id)
    : [];
}

// Walks upstream from pad id; returns the first pad for which found(pad)
// is true, nearest first, or null.
function searchUpstream(index, id, found) {
  const seen = new Set([id]);
  let level = [id];
  while (level.length) {
    const next = [];
    for (const pad of level)
      for (const up of upstreamOf(index, pad)) {
        if (seen.has(up))
          continue;
        if (found(up))
          return up;
        seen.add(up);
        next.push(up);
      }
    level = next;
  }
  return null;
}

// What this page measures and the results. Elements are kept by path, so
// they survive reloads and new runs; each connection gets them with that
// run's element ids. send(command) talks to the server.
export class Measuring {
  constructor(send) {
    this.send = send;
    this.paths = new Set(load(MEASURING_KEY, []));
    this.all = load(MEASURE_ALL_KEY, false) === true;
    // Element path -> id sent on this connection; "from>to" path keys sent.
    this.sent = new Map();
    this.pathsSent = new Set();
    // From the "latency" message.
    this.results = new Map();
    this.allState = { on: false, buffers_per_s: 0, stopped_rate: null };
    this.pathResults = new Map();
  }

  // A new connection knows nothing of what this page measures; elements
  // are sent again when this run's graph comes (sync).
  connected() {
    this.sent = new Map();
    this.pathsSent = new Set();
    if (this.all)
      this.send({ type: "measure-all" });
  }

  disconnected() {
    this.results = new Map();
  }

  has(path) {
    return this.paths.has(path);
  }

  toggle(path) {
    if (!this.paths.delete(path))
      this.paths.add(path);
    save(MEASURING_KEY, [...this.paths]);
  }

  toggleAll() {
    this.all = !this.all;
    save(MEASURE_ALL_KEY, this.all);
    this.allState.stopped_rate = null;
    this.send({ type: this.all ? "measure-all" : "unmeasure-all" });
  }

  // Sends what changed for this run's elements (elementPaths: path ->
  // {id}). Only with the run's graph, never with an older one.
  sync(elementPaths) {
    for (const [path, id] of this.sent) {
      const now = elementPaths.get(path)?.id;
      if (this.paths.has(path) && now === id)
        continue;
      if (now === id)
        this.send({ type: "unmeasure-element", element: id });
      this.sent.delete(path);
    }
    for (const path of this.paths) {
      const id = elementPaths.get(path)?.id;
      if (id !== undefined && !this.sent.has(path)) {
        this.send({ type: "measure-element", element: id });
        this.sent.set(path, id);
      }
    }
  }

  apply(latency) {
    this.results = new Map(latency.elements.map((e) => [e.element, e]));
    this.pathResults = new Map(latency.paths.map((p) =>
      [`${p.from}>${p.to}`, p.measured]));
    this.allState = latency.all;
    if (this.all && !latency.all.on && latency.all.stopped_rate) {
      this.all = false;
      save(MEASURE_ALL_KEY, false);
    }
  }

  result(element) {
    return this.results.get(element);
  }

  path(from, to) {
    return this.pathResults.get(`${from}>${to}`);
  }

  // Labels under measured elements in the graph: id -> label.
  labels(unit) {
    const labels = new Map();
    for (const [id, entry] of this.results) {
      const label = latencyLabel(entry, unit);
      if (label)
        labels.set(id, label);
    }
    return labels;
  }

  // The "measure all" button: {pressed, text, title}.
  button(connected) {
    const all = this.allState;
    if (this.all && !connected)
      return { pressed: true, text: "latency · starts when the program runs",
        title: "Measures every element once a program is connected; click " +
          "to stop" };
    if (this.all && all.on)
      return { pressed: true,
        text: `latency · ${all.buffers_per_s.toLocaleString()} buf/s`,
        title: "Measuring every element; click to stop" };
    if (all.stopped_rate)
      return { pressed: this.all, text: "latency stopped: too busy",
        title: `Stopped by itself at ${all.stopped_rate.toLocaleString()} ` +
          "buffers/s, to protect the pipeline. Measure single elements " +
          "from their details instead." };
    return { pressed: this.all, text: "latency",
      title: "Measure how long data takes through every element" };
  }

  // Which pad each pad card measures from: its own choice, else the
  // nearest pinned pad upstream (pins: an array of pad cards). Asks the
  // server for the paths needed; pads on different branches are measured
  // both ways.
  updatePaths(index, pins) {
    const pinned = new Set(pins.filter((p) => p.id !== null)
      .map((p) => p.id));
    const wanted = new Set();
    for (const pin of pins) {
      pin.source = null;
      if (pin.id === null)
        continue;
      const chosen = pin.from ? index.padPaths.get(pin.from)?.id : null;
      const from = chosen ??
        searchUpstream(index, pin.id, (p) => pinned.has(p));
      if (from === null || from === undefined || from === pin.id)
        continue;
      const upstream = from === chosen
        ? searchUpstream(index, pin.id, (p) => p === from) !== null : true;
      pin.source = { id: from, name: index.padName(from), upstream };
      wanted.add(`${from}>${pin.id}`);
      if (!upstream)
        wanted.add(`${pin.id}>${from}`);
    }
    for (const key of wanted)
      if (!this.pathsSent.has(key)) {
        const [from, to] = key.split(">").map(Number);
        this.send({ type: "measure-path", from, to });
      }
    for (const key of this.pathsSent)
      if (!wanted.has(key)) {
        const [from, to] = key.split(">").map(Number);
        this.send({ type: "unmeasure-path", from, to });
      }
    this.pathsSent = wanted;
  }
}
