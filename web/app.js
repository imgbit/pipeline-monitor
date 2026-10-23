// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

import { buildElkGraph, elementPath, keepPlaces } from "./layout.js";
import { GraphView, countElements } from "./render.js";
import { isTimeProperty, propertyText, showDetails, updateLatencyDetails,
  updateProblemsDetails, updateProperties, updateQueueDetails, valueText }
  from "./panel.js";
import { LogView } from "./log.js";
import { Tooltip } from "./tooltip.js";
import { linkCaps } from "./caps.js";
import { ElementPins, Pins } from "./pins.js";
import { TIME_UNITS, formatSpan } from "./units.js";
import { pipelineTimes, queueRows } from "./levels.js";
import { cannotMeasure, latencyLabel, latencyRows } from "./latency.js";
import { FindingsView } from "./findings.js";

const $ = (id) => document.getElementById(id);
const COLLAPSED_KEY = "pipeline-monitor.collapsed";
const CAPS_LABELS_KEY = "pipeline-monitor.caps-labels";
const TIME_UNIT_KEY = "pipeline-monitor.time-unit";

const elk = new ELK({ workerUrl: "vendor/elkjs/elk-worker.min.js" });

const state = {
  graph: { pipelines: [], links: [] },
  pipelineId: null,
  selected: null,
  collapsed: loadCollapsed(),
  capsLabels: loadFlag(CAPS_LABELS_KEY),
  known: new Map(),
  // From the "latency" message: entries by element id, and "measure all".
  latency: new Map(),
  latencyAll: { on: false, buffers_per_s: 0, stopped_at: null },
  // Elements this page asked to measure, and whether it wants all.
  measuring: new Set(),
  measureAll: false,
  // Pad to pad: "from>to" -> measured, and the keys sent to the server.
  pathLatency: new Map(),
  pathsSent: new Set(),
  // The pad card choosing its "measure from" pad, or null.
  picking: null,
  // From the "levels" message: queues and pipelines by element id.
  queues: new Map(),
  pipelineTimes: new Map(),
  // Where parts of the current pipeline were, see keepPlaces.
  places: { tops: new Map() },
};

const tooltip = new Tooltip();
// The pads of the current layout and lookups over the graph, for tooltips.
let shown = { info: null, index: null };

function padRows(p, index) {
  const pad = p.pad;
  const rows = [["direction", pad.direction]];
  if (pad.presence)
    rows.push(["presence", pad.presence]);
  if (pad.ghost)
    rows.push(["ghost", "yes"]);
  const peers = index.peers.get(pad.id) || [];
  rows.push(["linked to", peers.length
    ? peers.map((id) => index.padName(id)).join(", ") : "—"]);
  const stats = pins.statsFor(pad.id);
  if (stats?.window)
    rows.push(["buffers/s", stats.window.buffers_per_s.toFixed(1)]);
  if (stats?.last && stats.last.delay !== null)
    rows.push(["delay", formatSpan(stats.last.delay, pins.unit)]);
  for (const f of findings.byPlace().pads.get(pad.id) || [])
    rows.push([f.severity, f.text]);
  rows.push(["pinned", pins.has(index.padPath(pad.id))
    ? "yes, click to unpin" : "no, click to pin"]);
  return rows;
}

function showTooltip(target, x, y) {
  const { info, index } = shown;
  if (!target || !info) {
    tooltip.hide();
    return;
  }
  if (target.level) {
    const queue = state.queues.get(target.level);
    const element = index.elements.get(target.level)?.element;
    if (!queue || !element)
      return tooltip.hide();
    tooltip.show(x, y, `${element.name} (${queue.kind})`,
      queueRows(queue, pins.unit, (id) => index.padName(id).split(":").pop()));
  } else if (target.port) {
    const p = info.ports.get(target.port);
    if (!p)
      return tooltip.hide();
    tooltip.show(x, y, `${p.element.name}:${p.pad.name}`, padRows(p, index),
      p.pad.caps || "");
  } else {
    const src = info.ports.get(target.src);
    const sink = info.ports.get(target.sink);
    if (!src || !sink)
      return tooltip.hide();
    tooltip.show(x, y, `${src.element.name}:${src.pad.name} → ` +
      `${sink.element.name}:${sink.pad.name}`, [],
      linkCaps(info, target.src, target.sink));
  }
}

const view = new GraphView($("graph"), {
  hover: showTooltip,
  select(id) {
    state.selected = id;
    update();
  },
  toggle(path) {
    if (state.collapsed.has(path))
      state.collapsed.delete(path);
    else
      state.collapsed.add(path);
    saveCollapsed();
    state.places = { tops: new Map() };
    update();
  },
  autoFit(on) {
    $("auto-fit").setAttribute("aria-pressed", String(on));
  },
  pin(port) {
    const path = shown.index?.padPath(Number(port.slice(1)));
    if (!path)
      return;
    if (state.picking) {
      if (path !== state.picking.path)
        state.picking.from = path;
      stopPicking();
      pins.save();
      updatePaths();
      return;
    }
    pins.toggle(path);
  },
  moved() {
    pins.place();
    elementPins.place();
  },
});

let socket = null;
const pins = new Pins($("pins"), $("pin-lines"), {
  send(command) {
    if (socket?.readyState === WebSocket.OPEN)
      socket.send(JSON.stringify(command));
  },
  portPoint: (port) => view.portPoint(port),
  highlight: (port) => view.highlight(port),
  reveal: (port) => view.reveal(port),
  changed() {
    pins.sync(buildIndex(state.graph).padPaths);
    updatePaths();
    update();
  },
  pickFrom(pin) {
    // A second click goes back to automatic.
    if (pin.from) {
      delete pin.from;
      pins.save();
      updatePaths();
      return;
    }
    state.picking = pin;
    $("pick-hint").hidden = false;
  },
  extraRows: (pin) => extraRows(pin),
});
const elementPins = new ElementPins($("pins"), $("pin-lines"), {
  send(command) {
    if (socket?.readyState === WebSocket.OPEN)
      socket.send(JSON.stringify(command));
  },
  portPoint: (target) => view.portPoint(target),
  highlight: (target) => view.highlight(target),
  reveal: (target) => view.reveal(target),
  changed() {
    elementPins.sync(buildIndex(state.graph).elementPaths);
    update();
  },
}, { valueText, propertyText, isTime: isTimeProperty });

pins.setUnit(TIME_UNITS.includes(loadString(TIME_UNIT_KEY))
  ? loadString(TIME_UNIT_KEY) : "auto");

const log = new LogView({
  counter: $("log-counter"),
  panel: $("log-panel"),
  rows: $("log-rows"),
  mutedList: $("log-muted"),
  clear: $("log-clear"),
  close: $("log-close"),
}, {
  select(id) {
    if (buildIndex(state.graph).elements.has(id))
      view.handlers.select(id);
  },
  clear() {
    log.clear(elementCounts(state.graph));
    update();
  },
  changed() {
    update();
  },
});

const findings = new FindingsView({
  counter: $("problems-counter"),
  panel: $("findings-panel"),
  rows: $("findings-rows"),
  close: $("findings-close"),
}, {
  select(id) {
    if (buildIndex(state.graph).elements.has(id))
      view.handlers.select(id);
  },
});

// The worst severity per place, for the graph.
const RANK = { info: 0, warning: 1, error: 2 };
function worst(list) {
  return list.reduce((a, f) => (RANK[f.severity] > RANK[a] ? f.severity : a),
    "info");
}

// Findings of an element and of its pads.
function problemsOf(element) {
  const ids = new Set(element.pads.map((p) => p.id));
  return findings.list.filter((f) => f.element === element.id ||
    ids.has(f.pad));
}

function showProblems() {
  const selection = buildIndex(state.graph).elements.get(state.selected);
  if (selection)
    updateProblemsDetails($("details"), problemsOf(selection.element));
  const { pads, elements } = findings.byPlace();
  const sev = (map) => new Map([...map].map(([id, list]) => [id,
    worst(list)]).filter(([, s]) => s !== "info"));
  view.setProblems(sev(pads), sev(elements));
}

// Current error and warning counters of every element, by id.
function elementCounts(graph) {
  const counts = new Map();
  const visit = (e) => {
    counts.set(e.id, { errors: e.errors || 0, warnings: e.warnings || 0 });
    e.children.forEach(visit);
  };
  graph.pipelines.forEach(visit);
  return counts;
}

function loadFlag(key) {
  try {
    return localStorage.getItem(key) === "true";
  } catch {
    return false;
  }
}

function loadString(key) {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

function saveFlag(key, value) {
  try {
    localStorage.setItem(key, String(value));
  } catch {
    // Storage may be unavailable; the setting still works for this session.
  }
}

function loadCollapsed() {
  try {
    return new Set(JSON.parse(localStorage.getItem(COLLAPSED_KEY)) || []);
  } catch {
    return new Set();
  }
}

function saveCollapsed() {
  try {
    localStorage.setItem(COLLAPSED_KEY, JSON.stringify([...state.collapsed]));
  } catch {
    // Storage may be unavailable; collapsing still works for this session.
  }
}

function currentPipeline() {
  const { pipelines } = state.graph;
  return pipelines.find((p) => p.id === state.pipelineId) || pipelines[0];
}

// Lookups over the whole graph for the details panel and edge highlighting.
function buildIndex(graph) {
  const pads = new Map();
  const padPaths = new Map();
  const elementPaths = new Map();
  const elements = new Map();
  const peers = new Map();
  const linksInto = new Map();
  const visit = (element, parents) => {
    const path = elementPath(element, parents);
    elements.set(element.id, { element, path });
    elementPaths.set(path, { id: element.id });
    for (const pad of element.pads) {
      pads.set(pad.id, { pad, element, path: `${path}:${pad.name}` });
      padPaths.set(`${path}:${pad.name}`, { id: pad.id, caps: pad.caps });
    }
    for (const child of element.children)
      visit(child, [...parents, element.name]);
  };
  graph.pipelines.forEach((p) => visit(p, []));
  for (const { src, sink } of graph.links) {
    if (!linksInto.has(sink))
      linksInto.set(sink, []);
    linksInto.get(sink).push(src);
    if (!peers.has(src))
      peers.set(src, []);
    if (!peers.has(sink))
      peers.set(sink, []);
    peers.get(src).push(sink);
    peers.get(sink).push(src);
  }
  return {
    elements,
    peers,
    padPaths,
    elementPaths,
    pads,
    linksInto,
    padPath: (id) => pads.get(id)?.path,
    padName(id) {
      const p = pads.get(id);
      return p ? `${p.element.name}:${p.pad.name}` : `#${id}`;
    },
  };
}

function updateHeader(pipeline) {
  const select = $("pipeline-select");
  const { pipelines } = state.graph;

  select.hidden = pipelines.length < 2;
  $("pipeline-name").hidden = pipelines.length >= 2;
  if (pipelines.length >= 2) {
    select.replaceChildren(...pipelines.map((p) => {
      const o = document.createElement("option");
      o.value = p.id;
      o.textContent = p.name;
      o.selected = pipeline && p.id === pipeline.id;
      return o;
    }));
  }
  $("pipeline-name").textContent = pipeline ? pipeline.name : "";
  $("pipeline-state").textContent = pipeline ? pipeline.state : "";
  $("pipeline-state").className = `state ${pipeline ? pipeline.state : ""}`;
  $("element-count").textContent = pipeline
    ? `${countElements(pipeline)} elements` : "";
  showTimes(pipeline);
  $("empty").hidden = Boolean(pipeline);
}

function showTimes(pipeline) {
  const times = pipeline && state.pipelineTimes.get(pipeline.id);
  const node = $("pipeline-times");
  node.hidden = !times;
  if (times) {
    const { text, title } = pipelineTimes(times, pins.unit);
    node.textContent = text;
    node.title = title;
  }
}

// Elements not seen in the previous layout of the same pipeline.
function appearedIds(pipeline, index) {
  const ids = [...index.elements.keys()];
  const previous = state.known.get(pipeline.id);
  state.known.set(pipeline.id, new Set(ids));
  if (!previous)
    return new Set();
  return new Set(ids.filter((id) => !previous.has(id)));
}

let running = false;
let again = false;

async function update() {
  if (running) {
    again = true;
    return;
  }
  running = true;
  try {
    await layoutAndRender();
  } catch (err) {
    console.error(err);
  } finally {
    running = false;
    if (again) {
      again = false;
      update();
    }
  }
}

async function layoutAndRender() {
  const pipeline = currentPipeline();
  const index = buildIndex(state.graph);

  updateHeader(pipeline);
  if (!pipeline) {
    view.clear();
    pins.place();
    showDetails($("details"), null);
    return;
  }
  if (pipeline.id !== state.pipelineId) {
    state.pipelineId = pipeline.id;
    state.places = { tops: new Map() };
    view.setAutoFit(true);
  }

  const selection = index.elements.get(state.selected);
  if (!selection)
    state.selected = null;

  const { graph, info } = buildElkGraph(pipeline, state.graph.links,
    state.collapsed, state.capsLabels);
  const layout = await elk.layout(graph);
  keepPlaces(layout, state.places);
  const selectedPads = new Set(selection
    ? selection.element.pads.map((p) => `p${p.id}`) : []);
  const badges = new Map();
  for (const [id, { element, path }] of index.elements) {
    const badge = log.badge(id, path, element.errors, element.warnings);
    if (badge)
      badges.set(id, badge);
  }

  shown = { info, index };
  view.render(layout, info, {
    selected: state.selected,
    selectedPads,
    appeared: appearedIds(pipeline, index),
    badges,
    pinned: new Set([...pins.ports(), ...elementPins.ports()]),
  });
  view.setLevels(state.queues);
  showLatencyLabels();
  const details = selection && log.enabled ? {
    entries: log.entriesFor(selection.element.id),
    muted: log.isMuted(selection.path),
    toggleMute: () => log.setMuted(selection.path,
      !log.isMuted(selection.path)),
  } : null;
  showDetails($("details"), selection, index, () => view.handlers.select(null),
    details, queueDetails(selection, index));
  updateLatencyDetails($("details"), latencyDetails(selection));
  if (selection)
    updateProblemsDetails($("details"), problemsOf(selection.element));
  // The panel was rebuilt: refill it from the last answer, or ask now for a
  // newly selected element.
  if (selection && propsLast.id === selection.element.id)
    updateProperties($("details"), propsLast.id, propsLast.data,
      pinningFor(propsLast.id));
  else if (selection)
    refreshProperties();
}

// Properties of the selected element, read once per second while the
// details panel is open.
let propsTimer = null;
// The last answer, to fill a rebuilt panel at once: {id, data}.
let propsLast = { id: null, data: null };

async function refreshProperties() {
  clearTimeout(propsTimer);
  const id = state.selected;
  if (id !== null) {
    let data = null;
    try {
      const r = await fetch(`/api/properties?element=${id}`);
      data = r.ok ? await r.json() : null;
    } catch {
      // Disconnected; the next round tries again.
    }
    if (state.selected === id) {
      propsLast = { id, data };
      updateProperties($("details"), id, data, pinningFor(id));
    }
  }
  propsTimer = setTimeout(refreshProperties, 1000);
}

// Pinning the selected element's properties from the panel.
function pinningFor(id) {
  const path = buildIndex(state.graph).elements.get(id)?.path;
  return path ? {
    has: (name) => elementPins.hasProperty(path, name),
    toggle: (name) => elementPins.toggleProperty(path, name),
  } : null;
}

// The selected element's queue rows for the details panel, or null.
function queueDetails(selection, index) {
  const queue = selection && state.queues.get(selection.element.id);
  return queue ? queueRows(queue, pins.unit,
    (id) => index.padName(id).split(":").pop()) : null;
}

function send(command) {
  if (socket?.readyState === WebSocket.OPEN)
    socket.send(JSON.stringify(command));
}

function showLatencyLabels() {
  const labels = new Map();
  for (const [id, entry] of state.latency) {
    const label = latencyLabel(entry, pins.unit);
    if (label)
      labels.set(id, label);
  }
  view.setLatency(labels);
}

function latencyDetails(selection) {
  if (!selection)
    return null;
  const id = selection.element.id;
  return {
    on: state.measuring.has(id),
    toggle() {
      const on = !state.measuring.has(id);
      if (on)
        state.measuring.add(id);
      else
        state.measuring.delete(id);
      send({ type: on ? "measure-latency" : "unmeasure-latency", element: id });
      updateLatencyDetails($("details"), latencyDetails(selection));
    },
    rows: latencyRows(state.latency.get(id), pins.unit,
      selection.element.pads),
    // A source still has a reported latency; a sink has nothing at all.
    measurable: selection.element.pads.some((p) => p.direction === "src"),
  };
}

function showMeasureAll() {
  const button = $("measure-all");
  const all = state.latencyAll;
  button.setAttribute("aria-pressed", String(state.measureAll));
  if (state.measureAll && all.on) {
    button.textContent = `latency · ${all.buffers_per_s.toLocaleString()} ` +
      "buf/s";
    button.title = "Measuring every element; click to stop";
  } else if (all.stopped_at) {
    button.textContent = "latency stopped: too busy";
    button.title = `Stopped by itself at ${all.stopped_at.toLocaleString()} ` +
      "buffers/s, to protect the pipeline. Measure single elements from " +
      "their details instead.";
  } else {
    button.textContent = "latency";
    button.title = "Measure how long data takes through every element";
  }
}

function stopPicking() {
  state.picking = null;
  $("pick-hint").hidden = true;
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

// Which pad each pad card measures from: its own choice, else the nearest
// pinned pad upstream. Asks the server for the paths needed; pads on
// different branches are measured both ways.
function updatePaths() {
  const index = buildIndex(state.graph);
  const pinned = new Set([...pins.pins.values()].filter((p) => p.id !== null)
    .map((p) => p.id));
  const wanted = new Set();
  for (const pin of pins.pins.values()) {
    pin.source = null;
    if (pin.id === null)
      continue;
    const chosen = pin.from ? index.padPaths.get(pin.from)?.id : null;
    const from = chosen ?? searchUpstream(index, pin.id, (p) => pinned.has(p));
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
    if (!state.pathsSent.has(key)) {
      const [from, to] = key.split(">").map(Number);
      send({ type: "measure-path", from, to });
    }
  for (const key of state.pathsSent)
    if (!wanted.has(key)) {
      const [from, to] = key.split(">").map(Number);
      send({ type: "unmeasure-path", from, to });
    }
  state.pathsSent = wanted;
}

const AV_SAME = 5e6;

// Extra rows of a pad card: time from its source pad, and A/V sync with
// another pinned sink pad of the other kind.
function extraRows(pin) {
  const rows = {};
  const src = pin.source;
  if (src) {
    const there = state.pathLatency.get(`${src.id}>${pin.id}`);
    const back = state.pathLatency.get(`${pin.id}>${src.id}`);
    const span = (m) => `${formatSpan(m.avg, pins.unit)} (` +
      `${formatSpan(m.min, pins.unit)} – ${formatSpan(m.max, pins.unit)})`;
    const short = src.name.split("/").pop();
    if (src.upstream) {
      rows.path = { label: `from ${short}`, labelTitle:
        `Time from ${src.name} to this pad, last second (avg, min – max)`,
      text: !there ? "waiting…" : cannotMeasure(there)
        ? "cannot measure: timestamps change"
        : there.avg === null ? "waiting…" : span(there) };
    } else {
      const label = `vs ${short}`;
      const labelTitle = `${src.name} is on another branch: when the same ` +
        "moment of the stream arrives here compared to there";
      if (there?.avg != null)
        rows.path = { label, labelTitle, text:
          `${formatSpan(there.avg, pins.unit)} later` };
      else if (back?.avg != null)
        rows.path = { label, labelTitle, text:
          `${formatSpan(back.avg, pins.unit)} earlier` };
      else
        rows.path = { label, labelTitle, text: "waiting…" };
    }
  }

  const kind = sinkKind(pin);
  const other = kind && [...pins.pins.values()].find((p) => p !== pin &&
    sinkKind(p) === (kind === "audio" ? "video" : "audio"));
  const mine = pin.stats?.last?.delay;
  const theirs = other?.stats?.last?.delay;
  if (other && mine != null && theirs != null) {
    const audio = kind === "audio" ? mine : theirs;
    const video = kind === "audio" ? theirs : mine;
    const diff = Math.max(video, 0) - Math.max(audio, 0);
    rows.av = { label: "A/V", labelTitle: "From the delays of the audio and " +
      "video sink pads: a sink that is late plays behind the other",
    text: Math.abs(diff) < AV_SAME ? "in sync"
      : `${diff > 0 ? "video" : "audio"} ${formatSpan(Math.abs(diff),
        pins.unit)} behind ${diff > 0 ? "audio" : "video"}` };
  }
  return rows;
}

// "audio" or "video" for a pinned pad of a sink element, else null.
function sinkKind(pin) {
  if (pin.id === null)
    return null;
  const p = (shown.index ?? buildIndex(state.graph)).pads.get(pin.id);
  if (!p || p.pad.direction !== "sink" ||
      p.element.pads.some((x) => x.direction === "src"))
    return null;
  const caps = p.pad.caps || "";
  return caps.startsWith("audio/") ? "audio"
    : caps.startsWith("video/") ? "video" : null;
}

function applyLatency(latency) {
  state.latency = new Map(latency.elements.map((e) => [e.element, e]));
  state.pathLatency = new Map(latency.paths.map((p) => [`${p.from}>${p.to}`,
    p.measured]));
  state.latencyAll = latency.all;
  if (state.measureAll && !latency.all.on && latency.all.stopped_at)
    state.measureAll = false;
  showLatencyLabels();
  showMeasureAll();
  const index = buildIndex(state.graph);
  updateLatencyDetails($("details"), latencyDetails(
    index.elements.get(state.selected)));
}

function applyLevels(levels) {
  state.queues = new Map(levels.queues.map((q) => [q.id, q]));
  state.pipelineTimes = new Map(levels.pipelines.map((p) => [p.id, p]));
  view.setLevels(state.queues);
  showTimes(currentPipeline());
  const index = buildIndex(state.graph);
  updateQueueDetails($("details"), queueDetails(
    index.elements.get(state.selected), index));
}

function connect(delay = 1000) {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  const status = $("connection");
  socket = ws;

  ws.onopen = () => {
    status.textContent = "connected";
    status.className = "connection connected";
    delay = 1000;
    log.reset();
    pins.connected();
    elementPins.connected();
    // A new connection knows nothing of what this page measures.
    for (const id of state.measuring)
      send({ type: "measure-latency", element: id });
    if (state.measureAll)
      send({ type: "measure-all" });
    state.pathsSent = new Set();
  };
  ws.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "graph") {
      state.graph = message.graph;
      const index = buildIndex(state.graph);
      pins.sync(index.padPaths);
      elementPins.sync(index.elementPaths);
      updatePaths();
      update();
    } else if (message.type === "findings") {
      findings.apply(message.findings);
      showProblems();
    } else if (message.type === "latency") {
      applyLatency(message.latency);
    } else if (message.type === "levels") {
      applyLevels(message.levels);
    } else if (message.type === "properties") {
      elementPins.apply(message);
    } else if (message.type === "pad-stats") {
      pins.apply(message.stats);
    } else if (message.type === "log") {
      log.apply(message.log);
      if (state.selected !== null)
        update();
    }
  };
  ws.onclose = () => {
    status.textContent = "disconnected, retrying";
    status.className = "connection disconnected";
    setTimeout(() => connect(Math.min(delay * 2, 5000)), delay);
  };
}

$("pipeline-select").addEventListener("change", (e) => {
  state.pipelineId = Number(e.target.value);
  state.selected = null;
  update();
});
$("zoom-in").addEventListener("click", () => view.zoomCenter(1.25));
$("zoom-out").addEventListener("click", () => view.zoomCenter(0.8));
$("auto-fit").addEventListener("click", () => view.setAutoFit(!view.autoFit));
$("caps-labels").setAttribute("aria-pressed", String(state.capsLabels));
$("caps-labels").addEventListener("click", () => {
  state.capsLabels = !state.capsLabels;
  saveFlag(CAPS_LABELS_KEY, state.capsLabels);
  $("caps-labels").setAttribute("aria-pressed", String(state.capsLabels));
  state.places = { tops: new Map() };
  update();
});
window.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && state.picking)
    stopPicking();
});
$("measure-all").addEventListener("click", () => {
  state.measureAll = !state.measureAll;
  state.latencyAll.stopped_at = null;
  send({ type: state.measureAll ? "measure-all" : "unmeasure-all" });
  showMeasureAll();
});
$("time-unit").textContent = `time: ${pins.unit}`;
$("time-unit").addEventListener("click", () => {
  const unit = TIME_UNITS[(TIME_UNITS.indexOf(pins.unit) + 1) %
    TIME_UNITS.length];
  pins.setUnit(unit);
  saveFlag(TIME_UNIT_KEY, unit);
  $("time-unit").textContent = `time: ${unit}`;
});
window.addEventListener("resize", () => {
  if (view.autoFit)
    view.fit();
  pins.place();
  elementPins.place();
});

connect();
refreshProperties();
