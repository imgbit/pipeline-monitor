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
