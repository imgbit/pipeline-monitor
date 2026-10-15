import { buildElkGraph, elementPath } from "./layout.js";
import { GraphView, countElements } from "./render.js";
import { showDetails } from "./panel.js";

const $ = (id) => document.getElementById(id);
const COLLAPSED_KEY = "pipeline-monitor.collapsed";

const elk = new ELK({ workerUrl: "vendor/elkjs/elk-worker.min.js" });

const state = {
  graph: { pipelines: [], links: [] },
  pipelineId: null,
  selected: null,
  collapsed: loadCollapsed(),
  known: new Map(),
};

const view = new GraphView($("graph"), {
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
});

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
    view.userMoved = false;
  }

  const selection = index.elements.get(state.selected);
  if (!selection)
    state.selected = null;

  const { graph, info } = buildElkGraph(pipeline, state.graph.links,
    state.collapsed);
  const layout = await elk.layout(graph);
  const selectedPads = new Set(selection
    ? selection.element.pads.map((p) => `p${p.id}`) : []);

  view.render(layout, info, {
    selected: state.selected,
    selectedPads,
    appeared: appearedIds(pipeline, index),
  });
  showDetails($("details"), selection, index, () => view.handlers.select(null));
}

function connect(delay = 1000) {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  const status = $("connection");

  ws.onopen = () => {
    status.textContent = "connected";
    status.className = "connection connected";
    delay = 1000;
  };
  ws.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "graph") {
      state.graph = message.graph;
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
$("fit").addEventListener("click", () => view.fit());
window.addEventListener("resize", () => {
  if (!view.userMoved)
    view.fit();
});

connect();
