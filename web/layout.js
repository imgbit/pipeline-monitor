// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Converts one pipeline from the graph message into an ELK graph.

import { countElements } from "./render.js";
import { linkCaps, summarizeCaps } from "./caps.js";

const HEADER = 38;
const ROW = 18;
const PORT = 8;
const BIN_HEADER = 42;
const BADGE = 32;

const canvas = document.createElement("canvas");
const ctx = canvas.getContext("2d");
const fontFamily = getComputedStyle(document.documentElement)
  .getPropertyValue("--font");

function textWidth(text, size, weight = "normal") {
  ctx.font = `${weight} ${size}px ${fontFamily}`;
  return Math.ceil(ctx.measureText(text).width);
}

export function elementPath(element, parents) {
  return [...parents, element.name].join("/");
}

function sides(element) {
  return {
    sinks: element.pads.filter((p) => p.direction === "sink"),
    srcs: element.pads.filter((p) => p.direction === "src"),
  };
}

function maxLabel(pads) {
  return Math.max(0, ...pads.map((p) => textWidth(p.name, 11)));
}

function leafNode(element, info, kind, inside) {
  const { sinks, srcs } = sides(element);
  const toggle = kind === "collapsed" ? 28 : 0;
  const factory = element.factory || element.type;
  const title = Math.max(
    textWidth(element.name, 13, "600") + 18 + toggle + BADGE,
    textWidth(inside ? `${factory} · ${inside}` : factory, 11) + toggle);
  const pads = maxLabel(sinks) + maxLabel(srcs) + 24;
  const width = Math.max(90, title, pads) + 24;
  const rows = Math.max(sinks.length, srcs.length, 1);
  const height = HEADER + rows * ROW + 4;

  const port = (pad, i, west) => {
    info.ports.set(`p${pad.id}`, { pad, element, west, labelled: true });
    return {
      id: `p${pad.id}`,
      width: PORT,
      height: PORT,
      x: west ? -PORT / 2 : width - PORT / 2,
      y: HEADER + i * ROW + (ROW - PORT) / 2,
      layoutOptions: { "elk.port.side": west ? "WEST" : "EAST" },
    };
  };

  return {
    id: `e${element.id}`,
    width,
    height,
    ports: [...sinks.map((p, i) => port(p, i, true)),
      ...srcs.map((p, i) => port(p, i, false))],
    layoutOptions: { "elk.portConstraints": "FIXED_POS" },
    kind,
  };
}

function binNode(element, info, parents, collapsed) {
  const path = [...parents, element.name];
  const node = {
    id: `e${element.id}`,
    ports: element.pads.map((pad) => {
      const west = pad.direction === "sink";
      info.ports.set(`p${pad.id}`, { pad, element, west, labelled: false });
      return {
        id: `p${pad.id}`,
        width: PORT,
        height: PORT,
        layoutOptions: { "elk.port.side": west ? "WEST" : "EAST" },
      };
    }),
    children: element.children.map((c) => buildNode(c, info, path, collapsed)),
    layoutOptions: {
      "elk.portConstraints": "FIXED_SIDE",
      "elk.padding": `[top=${BIN_HEADER},left=24,bottom=16,right=24]`,
      "elk.nodeSize.constraints": "MINIMUM_SIZE",
      "elk.nodeSize.minimum":
        `(${textWidth(element.name, 13, "600") + 70 + BADGE}, 60)`,
    },
    kind: "bin",
  };
  return node;
}

function buildNode(element, info, parents, collapsed) {
  const path = elementPath(element, parents);
  let node;

  if (element.bin && !collapsed.has(path))
    node = binNode(element, info, parents, collapsed);
  else if (element.bin)
    node = leafNode(element, info, "collapsed",
      `${countElements(element)} inside`);
  else
    node = leafNode(element, info, "element");

  info.nodes.set(node.id, { element, path, kind: node.kind });
  delete node.kind;
  return node;
}

// capsLabels: give every link a caps label, laid out by ELK.
export function buildElkGraph(pipeline, links, collapsed, capsLabels) {
  const info = { nodes: new Map(), ports: new Map() };
  const children = pipeline.children.map((c) =>
    buildNode(c, info, [pipeline.name], collapsed));
  const edges = links
    .filter((l) => info.ports.has(`p${l.src}`) && info.ports.has(`p${l.sink}`))
    .map((l) => {
      const edge = {
        id: `l${l.src}-${l.sink}`,
        sources: [`p${l.src}`],
        targets: [`p${l.sink}`],
      };
      const summary = capsLabels &&
        summarizeCaps(linkCaps(info, `p${l.src}`, `p${l.sink}`));
      if (summary)
        edge.labels = [{ text: summary, width: textWidth(summary, 10) + 8,
          height: 14 }];
      return edge;
    });

  const graph = {
    id: "root",
    layoutOptions: {
      "elk.algorithm": "layered",
      "elk.direction": "RIGHT",
      "elk.hierarchyHandling": "INCLUDE_CHILDREN",
      "elk.edgeRouting": "ORTHOGONAL",
      "elk.layered.spacing.nodeNodeBetweenLayers": "48",
      "elk.spacing.nodeNode": "24",
      "elk.spacing.edgeNode": "14",
      "elk.layered.considerModelOrder.strategy": "NODES_AND_EDGES",
      "elk.padding": "[top=24,left=24,bottom=24,right=24]",
      "elk.json.shapeCoords": "ROOT",
      "elk.json.edgeCoords": "ROOT",
      "elk.edgeLabels.placement": "CENTER",
    },
    children,
    edges,
  };
  return { graph, info };
}

// Shifts every shape of a node, its children and ports by dy (ROOT coords).
function shiftNode(node, dy) {
  node.y += dy;
  for (const port of node.ports || [])
    port.y += dy;
  for (const label of node.labels || [])
    label.y += dy;
  for (const child of node.children || [])
    shiftNode(child, dy);
  for (const edge of node.edges || [])
    shiftEdge(edge, dy);
}

function shiftEdge(edge, dy) {
  for (const s of edge.sections || []) {
    s.startPoint.y += dy;
    s.endPoint.y += dy;
    for (const p of s.bendPoints || [])
      p.y += dy;
  }
  for (const label of edge.labels || [])
    if (label.y !== undefined)
      label.y += dy;
}

// Keeps the graph from jumping when a branch comes and goes: unconnected
// parts of a pipeline (e.g. audio and video) never move up, and the size
// (for auto-fit) never shrinks. memory: {tops: Map, width, height}, kept by
// the caller and reset when the layout changes on purpose.
export function keepPlaces(layout, memory) {
  const top = new Map();
  const parent = new Map();
  const find = (id) => (parent.get(id) === id ? id : find(parent.get(id)));
  for (const child of layout.children || []) {
    parent.set(child.id, child.id);
    const visit = (node) => {
      for (const port of node.ports || [])
        top.set(port.id, child.id);
      for (const c of node.children || [])
        visit(c);
    };
    visit(child);
  }
  for (const edge of layout.edges || []) {
    const a = top.get(edge.sources[0]);
    const b = top.get(edge.targets[0]);
    if (a && b)
      parent.set(find(a), find(b));
  }

  const parts = new Map();
  for (const child of layout.children || []) {
    const root = find(child.id);
    if (!parts.has(root))
      parts.set(root, { nodes: [], y: Infinity, left: null });
    const part = parts.get(root);
    part.nodes.push(child);
    part.y = Math.min(part.y, child.y);
    // A part is known by its leftmost node, usually the source; elements
    // added later (branches) come first in the graph, so order won't do.
    if (!part.left || child.x < part.left.x ||
        (child.x === part.left.x && child.y < part.left.y))
      part.left = child;
  }

  // Shifts only grow from top to bottom, so ELK's spacing is kept.
  const shift = new Map();
  let dy = 0;
  for (const part of [...parts.values()].sort((a, b) => a.y - b.y)) {
    const key = part.left.id;
    dy = Math.max(dy, (memory.tops.get(key) ?? part.y) - part.y);
    memory.tops.set(key, part.y + dy);
    for (const node of part.nodes)
      shift.set(node.id, dy);
  }
  for (const child of layout.children || [])
    if (shift.get(child.id))
      shiftNode(child, shift.get(child.id));
  for (const edge of layout.edges || []) {
    const d = shift.get(top.get(edge.sources[0]));
    if (d)
      shiftEdge(edge, d);
  }
  memory.width = Math.max(memory.width || 0, layout.width);
  memory.height = Math.max(memory.height || 0, layout.height + dy);
  layout.width = memory.width;
  layout.height = memory.height;
}
