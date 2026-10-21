// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

import { buildElkGraph, elementPath } from "./layout.js";
import { GraphView, countElements } from "./render.js";
import { showDetails } from "./panel.js";
import { LogView } from "./log.js";
import { Tooltip } from "./tooltip.js";
import { linkCaps } from "./caps.js";

const $ = (id) => document.getElementById(id);
const COLLAPSED_KEY = "pipeline-monitor.collapsed";
const CAPS_LABELS_KEY = "pipeline-monitor.caps-labels";

const elk = new ELK({ workerUrl: "vendor/elkjs/elk-worker.min.js" });

const state = {
  graph: { pipelines: [], links: [] },
  pipelineId: null,
  selected: null,
  collapsed: loadCollapsed(),
  capsLabels: loadFlag(CAPS_LABELS_KEY),
  known: new Map(),
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
  return rows;
}

function showTooltip(target, x, y) {
  const { info, index } = shown;
  if (!target || !info) {
    tooltip.hide();
    return;
  }
  if (target.port) {
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
    update();
  },
  autoFit(on) {
    $("auto-fit").setAttribute("aria-pressed", String(on));
  },
});

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
  const elements = new Map();
  const peers = new Map();
  const visit = (element, parents) => {
    elements.set(element.id, { element, path: elementPath(element, parents) });
    for (const pad of element.pads)
      pads.set(pad.id, { pad, element });
    for (const child of element.children)
      visit(child, [...parents, element.name]);
  };
  graph.pipelines.forEach((p) => visit(p, []));
  for (const { src, sink } of graph.links) {
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
  $("empty").hidden = Boolean(pipeline);
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
    showDetails($("details"), null);
    return;
  }
  if (pipeline.id !== state.pipelineId) {
    state.pipelineId = pipeline.id;
    view.setAutoFit(true);
  }

  const selection = index.elements.get(state.selected);
  if (!selection)
    state.selected = null;

  const { graph, info } = buildElkGraph(pipeline, state.graph.links,
    state.collapsed, state.capsLabels);
  const layout = await elk.layout(graph);
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
  });
  const details = selection && log.enabled ? {
    entries: log.entriesFor(selection.element.id),
    muted: log.isMuted(selection.path),
    toggleMute: () => log.setMuted(selection.path,
      !log.isMuted(selection.path)),
  } : null;
  showDetails($("details"), selection, index, () => view.handlers.select(null),
    details);
}

function connect(delay = 1000) {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  const status = $("connection");

  ws.onopen = () => {
    status.textContent = "connected";
    status.className = "connection connected";
    delay = 1000;
    log.reset();
  };
  ws.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "graph") {
      state.graph = message.graph;
      update();
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
  update();
});
window.addEventListener("resize", () => {
  if (view.autoFit)
    view.fit();
});

connect();
