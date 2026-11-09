// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

import { buildElkGraph, keepPlaces } from "./layout.js";
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
import { cannotMeasure, latencyRows } from "./latency.js";
import { Measuring } from "./measure.js";
import { FindingsView } from "./findings.js";
import { RecordingsView } from "./recordings.js";
import { Dock } from "./dock.js";

const $ = (id) => document.getElementById(id);
const COLLAPSED_KEY = "pipeline-monitor.collapsed";
const CAPS_LABELS_KEY = "pipeline-monitor.caps-labels";
const TIME_UNIT_KEY = "pipeline-monitor.time-unit";
const AUTO_FIT_KEY = "pipeline-monitor.auto-fit";

// On the published site (no pipeline-monitor server behind the page) the
// site's build adds <meta name="pipeline-monitor-site" content="LIST">,
// LIST being its demo recordings; the page is then a viewer and never
// connects anywhere.
const siteList = document.querySelector('meta[name="pipeline-monitor-site"]')
  ?.content ?? null;

// Each tab is live (connected to the program) or a viewer of recordings,
// chosen when it opens: "#viewer", "#viewer=<name>" or "#file=<url>".
const viewerMode = siteList !== null || location.hash.startsWith("#viewer") ||
  location.hash.startsWith("#file=");

const elk = new ELK({ workerUrl: "vendor/elkjs/elk-worker.min.js" });

const state = {
  graph: { pipelines: [], links: [] },
  pipelineId: null,
  selected: null,
  // Its path, to find it again in a new run (ids are per run), and whether
  // a new run has started and it is still to be found.
  selectedPath: null,
  reselect: false,
  collapsed: loadCollapsed(),
  capsLabels: loadFlag(CAPS_LABELS_KEY),
  known: new Map(),
  // A program is connected: measurements and pins are applied.
  connected: false,
  // The pad card choosing its "measure from" pad, or null.
  picking: null,
  // From the "levels" message: queues and pipelines by element id.
  queues: new Map(),
  pipelineTimes: new Map(),
  // Where parts of the current pipeline were, see keepPlaces.
  places: { tops: new Map() },
};

const measure = new Measuring(send);
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
  // A recording cannot pin pads.
  if (!state.viewing)
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
    // A problem where data leaves the src pad is a problem of the link.
    const rows = (findings.byPlace().pads.get(Number(target.src.slice(1))) ||
      []).map((f) => [f.severity, f.text]);
    tooltip.show(x, y, `${src.element.name}:${src.pad.name} → ` +
      `${sink.element.name}:${sink.pad.name}`, rows,
    linkCaps(info, target.src, target.sink));
  }
}

const view = new GraphView($("graph"), {
  hover: showTooltip,
  select(id) {
    state.selected = id;
    state.selectedPath = id === null ? null
      : buildIndex(state.graph).elements.get(id)?.path ?? null;
    state.reselect = false;
    update();
  },
  toggle(path) {
    if (state.collapsed.has(path))
      state.collapsed.delete(path);
    else
      state.collapsed.add(path);
    // A recording's view is not this browser's.
    if (!state.viewing)
      saveCollapsed();
    state.places = { tops: new Map() };
    update();
  },
  autoFit(on) {
    $("auto-fit").setAttribute("aria-pressed", String(on));
  },
  pin(port) {
    const path = shown.index?.padPath(Number(port.slice(1)));
    if (!path || state.viewing)
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
// In a viewer, pin cards are a recording's: never stored, never sent to a
// program (a viewer tab may be connected to one, for the list).
const pins = new Pins($("pins"), $("pin-lines"), {
  send(command) {
    if (!viewerMode && socket?.readyState === WebSocket.OPEN)
      socket.send(JSON.stringify(command));
  },
  portPoint: (port) => view.portPoint(port),
  highlight: (port) => view.highlight(port),
  reveal: (port) => view.reveal(port),
  changed() {
    if (state.picking && !pins.has(state.picking.path))
      stopPicking();
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
}, viewerMode ? null : undefined);
const elementPins = new ElementPins($("pins"), $("pin-lines"), {
  send(command) {
    if (!viewerMode && socket?.readyState === WebSocket.OPEN)
      socket.send(JSON.stringify(command));
  },
  portPoint: (target) => view.portPoint(target),
  highlight: (target) => view.highlight(target),
  reveal: (target) => view.reveal(target),
  changed() {
    elementPins.sync(buildIndex(state.graph).elementPaths);
    update();
  },
}, { valueText, propertyText, isTime: isTimeProperty },
viewerMode ? null : undefined);

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

// What this page shows, saved with a recording.
function pageView() {
  return {
    pins: [...pins.pins.values()].map((pin) => pins.saved(pin)),
    element_pins: [...elementPins.pins.values()].map((pin) =>
      elementPins.saved(pin)),
    measuring: [...measure.paths],
    measure_all: measure.all,
    selected: state.selectedPath,
    collapsed: [...state.collapsed],
  };
}

const recordings = new RecordingsView({
  counter: $("recordings-counter"),
  panel: $("recordings-panel"),
  rows: $("recordings-rows"),
  record: $("record"),
  snapshot: $("snapshot"),
  close: $("recordings-close"),
  error: $("recordings-error"),
  note: $("recordings-note"),
  addFile: $("add-file"),
  fileInput: $("file-input"),
}, { send, view: pageView,
  open: (name) => viewerMode ? openStored(name) : openInViewer(name),
  openFile, openDemo, viewer: viewerMode, site: siteList !== null });

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

// One panel at the bottom at a time, opened from the header.
const dock = new Dock({
  findings: { button: $("problems-counter"), panel: $("findings-panel"),
    close: $("findings-close") },
  log: { button: $("log-counter"), panel: $("log-panel"),
    close: $("log-close") },
  recordings: { button: $("recordings-counter"),
    panel: $("recordings-panel"), close: $("recordings-close") },
}, viewerMode ? "pipeline-monitor.viewer-panel" : "pipeline-monitor.panel");
dock.open(dock.saved());

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
  // Paths come with the graph; they stay the same across runs, ids do not.
  const visit = (element) => {
    elements.set(element.id, { element, path: element.path });
    elementPaths.set(element.path, { id: element.id });
    for (const pad of element.pads) {
      pads.set(pad.id, { pad, element, path: pad.path });
      padPaths.set(pad.path, { id: pad.id, caps: pad.caps });
    }
    element.children.forEach(visit);
  };
  graph.pipelines.forEach(visit);
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
  // Without a connection it is the last state known, not the current one.
  $("pipeline-state").textContent = pipeline
    ? `${pipeline.state}${state.connected || state.viewing ? ""
      : " (last known)"}` : "";
  $("pipeline-state").className = `state ${pipeline ? pipeline.state : ""}` +
    (state.connected ? "" : " stale");
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
    // A new pipeline is fitted once; then auto-fit is as last chosen.
    view.setAutoFit(true);
    state.fitOnce = loadString(AUTO_FIT_KEY) === "false";
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
  if (state.fitOnce) {
    state.fitOnce = false;
    view.setAutoFit(false);
  }
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
    if (state.viewing) {
      data = state.viewing.properties.get(id) ?? null;
    } else {
      try {
        const r = await fetch(`/api/properties?element=${id}`);
        data = r.ok ? (await r.json()).elements[0] : null;
      } catch {
        // Disconnected; the next round tries again.
      }
    }
    if (state.selected === id) {
      propsLast = { id, data };
      updateProperties($("details"), id, data, pinningFor(id));
      const selection = buildIndex(state.graph).elements.get(id);
      updateLatencyDetails($("details"), latencyDetails(selection));
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
  view.setLatency(measure.labels(pins.unit));
}

function latencyDetails(selection) {
  if (!selection)
    return null;
  const id = selection.element.id;
  const on = measure.has(selection.path);
  return {
    on,
    toggle() {
      measure.toggle(selection.path);
      syncMeasuring();
      updateLatencyDetails($("details"), latencyDetails(selection));
    },
    rows: latencyRows(measure.result(id), pins.unit,
      selection.element.pads, propsLast.id === id ? propsLast.data?.reported
        : null, on && !state.connected),
    // Only with data going in and out is there a time in between.
    measurable: selection.element.pads.some((p) => p.direction === "sink") &&
      selection.element.pads.some((p) => p.direction === "src"),
  };
}

function showMeasureAll() {
  const button = $("measure-all");
  const { pressed, text, title } = measure.button(state.connected);
  button.setAttribute("aria-pressed", String(pressed));
  button.textContent = text;
  button.title = title;
}

function syncMeasuring() {
  if (socket?.readyState === WebSocket.OPEN)
    measure.sync(buildIndex(state.graph).elementPaths);
}

function stopPicking() {
  state.picking = null;
  $("pick-hint").hidden = true;
}

function updatePaths() {
  measure.updatePaths(buildIndex(state.graph), [...pins.pins.values()]);
}

const AV_SAME = 5e6;

// Extra rows of a pad card: time from its source pad, and A/V sync with
// another pinned sink pad of the other kind.
function extraRows(pin) {
  const rows = {};
  const src = pin.source;
  if (src) {
    const there = measure.path(src.id, pin.id);
    const back = measure.path(pin.id, src.id);
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
  measure.apply(latency);
  showLatencyLabels();
  showMeasureAll();
  const index = buildIndex(state.graph);
  updateLatencyDetails($("details"), latencyDetails(
    index.elements.get(state.selected)));
}

function applyLevels(levels) {
  state.queues = new Map(levels.queues.map((q) => [q.element, q]));
  state.pipelineTimes = new Map(levels.pipelines.map((p) =>
    [p.element, p]));
  view.setLevels(state.queues);
  showTimes(currentPipeline());
  const index = buildIndex(state.graph);
  updateQueueDetails($("details"), queueDetails(
    index.elements.get(state.selected), index));
}

// Recordings shown read-only: the program's state is set aside meanwhile.

function clearState() {
  state.graph = { pipelines: [], links: [] };
  lastAlive.graph = null;
  lastAlive.findings = null;
  state.selected = null;
  findings.apply({ findings: [] });
  showProblems();
  log.reset();
  measure.disconnected();
  applyLevels({ queues: [], pipelines: [] });
  showLatencyLabels();
}

// Properties of an element from a recording: the latest value of each.
function keepProperties(entry) {
  const known = state.viewing.properties.get(entry.element);
  const byName = new Map((known?.properties ?? []).map((p) => [p.name, p]));
  for (const p of entry.properties ?? [])
    byName.set(p.name, p);
  state.viewing.properties.set(entry.element, { element: entry.element,
    path: entry.path, properties: [...byName.values()] });
}

// A file whose content does not fit (edited, or from a newer version that
// changed a message) is closed again, with a word why.
function damaged(name) {
  closeRecording();
  recordings.showError(`${name} cannot be shown: its content is damaged.`);
}

// source: the list's row, {name} or {file}, marked as shown.
function openRecording(text, name, source) {
  try {
    if (showRecording(text, name))
      recordings.setShown(source);
  } catch {
    damaged(name);
  }
}

function showRecording(text, name) {
  const lines = text.split("\n").filter((line) => line.trim());
  let header = null;
  try {
    header = JSON.parse(lines[0]);
  } catch {
    // Not JSON: said below.
  }
  if (header?.format !== "pipeline-monitor-recording" ||
      header.version !== 1 || !Number.isFinite(header.started)) {
    recordings.showError(`${name} is not a recording this page can open.`);
    return;
  }
  const messages = [];
  for (const line of lines.slice(1)) {
    try {
      messages.push(JSON.parse(line));
    } catch {
      // A last line cut off by a crash.
    }
  }
  const byType = { graph: [], log: [], findings: [], levels: [], latency: [],
    properties: [], pads: [] };
  // Only messages of the types shown, with their times.
  for (const message of messages)
    if (Object.hasOwn(byType, message?.type) &&
        Number.isFinite(message.time))
      byType[message.type].push(message);
  // A graph that is not one makes the whole file damaged, found now
  // rather than when the slider reaches it.
  if (byType.graph.some((g) => !Array.isArray(g.pipelines)))
    throw new Error("damaged graph");
  const start = header.started;
  const end = messages.reduce((latest, m) =>
    Number.isFinite(m?.time) ? Math.max(latest, m.time) : latest, start);
  stopPlaying();
  state.viewing = { name, header, properties: new Map(), byType, start, end,
    at: start, shownGraph: undefined, speed: 1, timer: null };
  clearState();
  const recorded = header.view ?? {};
  state.collapsed = new Set(Array.isArray(recorded.collapsed)
    ? recorded.collapsed : []);
  // The page's pins when it recorded, as read-only cards.
  const saved = (list) => (Array.isArray(list) ? list : [])
    .filter((p) => typeof p?.path === "string");
  pins.replaceAll(saved(recorded.pins));
  elementPins.replaceAll(saved(recorded.element_pins).map((p) => ({ ...p,
    props: Array.isArray(p.props) ? p.props.filter((n) =>
      typeof n === "string") : [] })));
  state.selectedPath = typeof recorded.selected === "string"
    ? recorded.selected : null;
  state.reselect = state.selectedPath !== null;
  state.places = { tops: new Map() };
  document.body.classList.add("viewing");
  const program = header.hello?.program ?? "a program";
  const when = new Date(header.started / 1000).toLocaleString();
  $("viewing-text").textContent = header.kind === "snapshot"
    ? `Snapshot of ${program}, ${when}` : `Recording of ${program}, ${when}`;
  $("playback").hidden = header.kind === "snapshot";
  $("speed").textContent = "1×";
  $("viewing").hidden = false;
  seekTo(start);
  $("ended").hidden = true;
  $("lost").hidden = true;
  recordings.showError(null);
  return true;
}

// The message shown at t: the recorded one, or, where that has no pipeline
// (before the program built one, after it removed it), the nearest one
// with a pipeline: the last before t, else the first after it. For the
// graph and the levels (the header's clock line).
function shownAt(list, t) {
  const message = latestAt(list, t);
  if (message?.pipelines?.length)
    return message;
  const before = list.findLast((m) => m.time <= t && m.pipelines?.length);
  return before ?? list.find((m) => m.pipelines?.length) ?? message;
}

// The last message of a list (in time order) at or before t, or null.
function latestAt(list, t) {
  let lo = 0;
  let hi = list.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (list[mid].time <= t)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo > 0 ? list[lo - 1] : null;
}

const NO_LATENCY = { elements: [], paths: [],
  all: { on: false, buffers_per_s: 0, stopped_rate: null } };

function clock(us) {
  const s = Math.max(0, Math.floor(us / 1e6));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

// Shows the recording as it was at t (µs since 1970).
function seek(t) {
  try {
    seekTo(t);
  } catch {
    damaged(state.viewing.name);
  }
}

function seekTo(t) {
  const v = state.viewing;
  v.at = Math.max(v.start, Math.min(t, v.end));
  const at = v.at;
  const graph = shownAt(v.byType.graph, at);
  if (graph !== v.shownGraph) {
    v.shownGraph = graph;
    if (graph)
      handleMessage(graph);
    else {
      state.graph = { pipelines: [], links: [] };
      update();
    }
  }
  findings.apply(latestAt(v.byType.findings, at) ?? { findings: [] });
  showProblems();
  applyLevels(shownAt(v.byType.levels, at) ?? { queues: [], pipelines: [] });
  applyLatency(latestAt(v.byType.latency, at) ?? NO_LATENCY);
  log.reset();
  for (const message of v.byType.log) {
    if (message.time > at)
      break;
    log.apply(message);
  }
  v.properties = new Map();
  for (const message of v.byType.properties) {
    if (message.time > at)
      break;
    message.elements.filter((e) => !e.gone).forEach(keepProperties);
  }
  pins.showStats(latestAt(v.byType.pads, at));
  elementPins.showStats({ time: at, elements: [...v.properties.values()] });
  refreshProperties();
  const span = v.end - v.start;
  $("seek").value = span > 0 ? String(Math.round((at - v.start) / span *
    1000)) : "1000";
  $("play-time").textContent = `${clock(at - v.start)} / ${clock(span)}`;
}

function stopPlaying() {
  if (state.viewing?.timer) {
    clearInterval(state.viewing.timer);
    state.viewing.timer = null;
  }
  $("play").textContent = "▶";
  $("play").title = "Play";
}

$("play").addEventListener("click", () => {
  const v = state.viewing;
  if (!v || v.timer) {
    stopPlaying();
    return;
  }
  if (v.at >= v.end)
    seek(v.start);
  $("play").textContent = "❚❚";
  $("play").title = "Pause";
  v.timer = setInterval(() => {
    seek(v.at + 250000 * v.speed);
    if (v.at >= v.end)
      stopPlaying();
  }, 250);
});
$("seek").addEventListener("input", (e) => {
  const v = state.viewing;
  if (v)
    seek(v.start + (v.end - v.start) * Number(e.target.value) / 1000);
});
$("speed").addEventListener("click", () => {
  const v = state.viewing;
  if (!v)
    return;
  v.speed = v.speed === 1 ? 4 : v.speed === 4 ? 16 : 1;
  $("speed").textContent = `${v.speed}×`;
});

// Back to the viewer with nothing open.
function closeRecording() {
  if (!state.viewing)
    return;
  stopPlaying();
  state.viewing = null;
  recordings.setShown(null);
  pins.replaceAll([]);
  elementPins.replaceAll([]);
  $("viewing").hidden = true;
  clearState();
  state.places = { tops: new Map() };
  history.replaceState(null, "", "#viewer");
  update();
  dock.open("recordings");
}

// A recording in the program's folder (or "opened": the file the command
// was given).
async function openStored(name) {
  const url = name === "opened" ? "api/opened"
    : `api/recordings/${encodeURIComponent(name)}`;
  try {
    const r = await fetch(url);
    if (!r.ok)
      throw new Error(r.statusText);
    openRecording(await r.text(), name, { name });
    history.replaceState(null, "", `#viewer=${encodeURIComponent(name)}`);
  } catch {
    recordings.showError(`Cannot read ${name}.`);
  }
}

// Recordings open in a viewer tab: the live tab keeps monitoring. Tabs
// open only on a click (Open in the list), never later.
function openTab(hash) {
  const tab = window.open(`${location.pathname}${hash}`, "_blank");
  if (!tab)
    recordings.showError("The browser blocked the new tab: allow pop-ups " +
      "for this page.");
  return tab;
}

function openInViewer(name) {
  openTab(`#viewer=${encodeURIComponent(name)}`);
}

// A demo recording of the site, read when it is opened.
async function openDemo(demo) {
  try {
    const r = await fetch(demo.url);
    if (!r.ok)
      throw new Error(r.statusText);
    openRecording(await r.text(), demo.title, { url: demo.url });
  } catch {
    recordings.showError(`Cannot read ${demo.title}.`);
  }
}

// A recording from a link (#file=URL): added to the list like a file from
// this computer (marked as from the link), then shown. Only hosts that allow other sites to read
// their files (e.g. GitHub raw files, gists) can be opened this way.
async function openLink(url) {
  dock.open("recordings");
  let text;
  try {
    const r = await fetch(url);
    if (!r.ok)
      throw new Error(r.statusText);
    text = await r.text();
  } catch {
    recordings.showError(`Cannot read ${url}: its site does not let other ` +
      "pages read it. Download the file and drop it on this page.");
    return;
  }
  const name = decodeURIComponent(new URL(url, location.href).pathname
    .split("/").pop() || "recording.jsonl");
  const file = new File([text], name);
  await recordings.addFile(file, url);
  if (recordings.files.some((f) => f.file === file))
    openFile(file);
}

// The site's demo recordings: [{title, file, kind, started, last, size}].
async function loadDemos() {
  try {
    const r = await fetch(siteList);
    const list = await r.json();
    recordings.setDemos(list.map((d) => ({ ...d,
      url: new URL(d.file, new URL(siteList, location.href)).href })));
  } catch {
    recordings.showError("The demo recordings could not be read.");
  }
}

// Added files by the viewer tab they were opened in: a tab asks for its
// file once it has loaded (also after a reload).
const tabFiles = new Map();

// A file added from this computer: shown here in a viewer, in a new viewer
// tab from the live page.
function openFile(file) {
  if (viewerMode) {
    file.text().then((text) => openRecording(text, file.name, { file }));
    return;
  }
  const tab = openTab("#viewer-file");
  if (tab)
    tabFiles.set(tab, file);
}

$("viewing-close").addEventListener("click", closeRecording);
// A recording file dropped on the page.
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => {
  const file = e.dataTransfer?.files?.[0];
  if (!file)
    return;
  e.preventDefault();
  dock.open("recordings");
  recordings.addFile(file);
});
window.addEventListener("message", (e) => {
  if (e.origin !== location.origin)
    return;
  if (e.data?.type === "pipeline-monitor-file-wanted" && tabFiles.has(e.source))
    e.source.postMessage({ type: "pipeline-monitor-file",
      file: tabFiles.get(e.source) }, location.origin);
  else if (viewerMode && e.data?.type === "pipeline-monitor-file" &&
      e.data.file instanceof File) {
    recordings.addFile(e.data.file);
    openFile(e.data.file);
  }
});

// A message from the program, or from a recording being shown.
function handleMessage(message) {
  // The pipeline-monitor command serves recordings only: show the viewer.
  if (message.type === "hello" && message.viewer) {
    location.replace(`${location.pathname}#viewer`);
    location.reload();
    return;
  }
  if (message.type === "graph") {
    state.graph = { pipelines: message.pipelines, links: message.links };
    if (state.graph.pipelines.length)
      lastAlive.graph = state.graph;
    const index = buildIndex(state.graph);
    if (state.reselect) {
      state.selected = index.elementPaths.get(state.selectedPath)?.id ??
        null;
      state.reselect = state.selected === null;
    }
    // A recording's pins find their pads by path (a viewer sends nothing).
    pins.sync(index.padPaths);
    elementPins.sync(index.elementPaths);
    if (!state.viewing) {
      updatePaths();
      syncMeasuring();
    }
    update();
  } else if (message.type === "findings") {
    findings.apply(message);
    if (state.graph.pipelines.length)
      lastAlive.findings = message;
    showProblems();
  } else if (message.type === "latency") {
    applyLatency(message);
  } else if (message.type === "levels") {
    applyLevels(message);
  } else if (message.type === "properties") {
    elementPins.apply(message);
  } else if (message.type === "pads") {
    pins.apply(message);
  } else if (message.type === "log") {
    log.apply(message);
    if (state.selected !== null)
      update();
  } else if (message.type === "recordings") {
    recordings.apply(message);
  } else if (message.type === "error") {
    recordings.applyError(message);
  } else if (message.type === "bye") {
    ended = true;
    // A program usually destroys its pipeline before it exits: show the
    // last state in which there was one.
    if (!state.graph.pipelines.length && lastAlive.graph) {
      state.graph = lastAlive.graph;
      if (lastAlive.findings)
        findings.apply(lastAlive.findings);
      showProblems();
      update();
    }
  }
}

function connect(delay = 1000) {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  const status = $("connection");
  socket = ws;

  ws.onopen = () => {
    status.textContent = "connected";
    status.className = "connection connected";
    $("ended").hidden = true;
    $("lost").hidden = true;
    ended = false;
    state.connected = true;
    delay = 1000;
    log.reset();
    pins.connected();
    elementPins.connected();
    measure.connected();
    recordings.setConnected(true);
    state.reselect = state.selectedPath !== null;
    showMeasureAll();
  };
  ws.onmessage = (event) => {
    const message = JSON.parse(event.data);
    handleMessage(message);
  };
  ws.onclose = () => {
    // Its last state stays on screen; what the page measures and pins is
    // applied again when a program connects.
    state.connected = false;
    measure.disconnected();
    recordings.setConnected(false);
    showMeasureAll();
    update();
    // After "bye" the program has ended, and the page quietly looks for a
    // new run on the same address.
    if (ended) {
      status.textContent = "program ended";
      status.className = "connection ended";
      $("ended").hidden = false;
      setTimeout(() => connect(5000), 5000);
      return;
    }
    status.textContent = "disconnected, retrying";
    status.className = "connection disconnected";
    $("lost").hidden = !state.graph.pipelines.length;
    setTimeout(() => connect(Math.min(delay * 2, 5000)), delay);
  };
}

// A viewer tab talks to the server only about recordings: the list, kept
// up to date, and Delete. The program's live messages are not for it.
function connectViewer(delay = 1000) {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  socket = ws;
  ws.onopen = () => {
    delay = 1000;
    recordings.setConnected(true);
  };
  ws.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "recordings")
      recordings.apply(message);
    else if (message.type === "error")
      recordings.applyError(message);
  };
  ws.onclose = () => {
    recordings.setConnected(false);
    setTimeout(() => connectViewer(Math.min(delay * 2, 5000)), delay);
  };
}

// The program said "bye" before the connection closed.
let ended = false;
// The graph and findings of the last moment a pipeline existed.
const lastAlive = { graph: null, findings: null };

$("pipeline-select").addEventListener("change", (e) => {
  state.pipelineId = Number(e.target.value);
  state.selected = null;
  state.selectedPath = null;
  update();
});
$("zoom-in").addEventListener("click", () => view.zoomCenter(1.25));
$("zoom-out").addEventListener("click", () => view.zoomCenter(0.8));
$("auto-fit").addEventListener("click", () => {
  view.setAutoFit(!view.autoFit);
  saveFlag(AUTO_FIT_KEY, view.autoFit);
});
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
  if (e.key === "Escape")
    setAbout(false);
});

// The About box under the name in the header.
function setAbout(open) {
  $("about").hidden = !open;
  $("brand").setAttribute("aria-expanded", String(open));
}
$("brand").addEventListener("click", () => setAbout($("about").hidden));
document.addEventListener("pointerdown", (e) => {
  if (!$("about").hidden && !$("about").contains(e.target) &&
      !$("brand").contains(e.target))
    setAbout(false);
});
$("measure-all").addEventListener("click", () => {
  measure.toggleAll();
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

function startViewer() {
  document.body.classList.add("viewing", "viewer-mode");
  $("connection").textContent = "viewer";
  $("connection").className = "connection viewing";
  $("empty").textContent = "Open a recording from the list; add one with " +
    "Add file…, or drop a file on the page.";
  // With nothing open yet, the list to open one from.
  if (!location.hash.startsWith("#viewer=") &&
      location.hash !== "#viewer-file")
    dock.open("recordings");
  // On the site there is no server: demos and added files only.
  if (siteList !== null)
    loadDemos();
  else
    connectViewer();
  const name = location.hash.match(/^#viewer=(.+)$/)?.[1];
  if (name)
    openStored(decodeURIComponent(name));
  const link = location.hash.match(/^#file=(.+)$/)?.[1];
  if (link)
    openLink(decodeURIComponent(link));
  // A file added in the live tab that opened this one: asked for.
  if (location.hash === "#viewer-file") {
    if (window.opener)
      window.opener.postMessage({ type: "pipeline-monitor-file-wanted" },
        location.origin);
    else {
      dock.open("recordings");
      recordings.showError("The file was added in another tab: add it " +
        "here with Add file….");
    }
  }
}

if (viewerMode)
  startViewer();
else
  connect();
refreshProperties();
