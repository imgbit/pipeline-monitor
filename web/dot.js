// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// GStreamer's .dot files (gst_debug_bin_to_dot_data): read into the page's
// own graph (the "graph" message of docs/protocol.md), to be shown like a
// snapshot. Only what GStreamer writes is read; anything else is refused.

const MAX_SIZE = 32 * 1024 * 1024;
const MAX_DEPTH = 256;

export class DotError extends Error {}

// The part of DOT that GStreamer writes: graphs, subgraphs, node and edge
// statements with attribute lists, graph attributes. GStreamer does not
// escape quotes inside labels (an element named a"b), so a string ends at
// a quote followed by ; , ] or the end of a line.
export function parseDot(text) {
  if (typeof text !== "string" || text.length > MAX_SIZE)
    throw new DotError("too large");
  let i = 0;
  const n = text.length;
  const space = () => {
    for (;;) {
      while (i < n && /\s/.test(text[i]))
        i++;
      if (text.startsWith("//", i) || (text[i] === "#" &&
          (i === 0 || text[i - 1] === "\n"))) {
        while (i < n && text[i] !== "\n")
          i++;
      } else if (text.startsWith("/*", i)) {
        const end = text.indexOf("*/", i + 2);
        i = end < 0 ? n : end + 2;
      } else {
        return;
      }
    }
  };
  const quoted = () => {
    let out = "";
    i++;
    while (i < n) {
      const c = text[i];
      if (c === "\\" && i + 1 < n) {
        const next = text[i + 1];
        out += next === "n" ? "\n" : next === "l" ? "\n" : next === "\""
          ? "\"" : next === "\\" ? "\\" : c + next;
        i += 2;
        continue;
      }
      if (c === "\"") {
        let j = i + 1;
        while (j < n && (text[j] === " " || text[j] === "\t"))
          j++;
        if (j >= n || ";,]\n\r".includes(text[j])) {
          i++;
          return out;
        }
      }
      out += c;
      i++;
    }
    throw new DotError("a string does not end");
  };
  const word = () => {
    space();
    if (text[i] === "\"")
      return quoted();
    const m = /^[A-Za-z0-9_.\-<>]+/.exec(text.slice(i, i + 4096));
    if (!m)
      throw new DotError(`unexpected "${text.slice(i, i + 12)}"`);
    i += m[0].length;
    return m[0];
  };
  const attributes = () => {
    const attrs = {};
    i++;
    for (;;) {
      space();
      if (text[i] === "]") {
        i++;
        return attrs;
      }
      if (text[i] === "," || text[i] === ";") {
        i++;
        continue;
      }
      const key = word();
      space();
      if (text[i] !== "=")
        throw new DotError("an attribute without a value");
      i++;
      attrs[key] = word();
    }
  };
  const block = (id, depth) => {
    if (depth > MAX_DEPTH)
      throw new DotError("nested too deep");
    const graph = { id, attrs: {}, nodes: [], edges: [], subgraphs: [] };
    for (;;) {
      space();
      if (i >= n)
        throw new DotError("a block does not end");
      if (text[i] === "}") {
        i++;
        return graph;
      }
      if (text[i] === ";") {
        i++;
        continue;
      }
      const first = word();
      if (first === "subgraph") {
        const sub = word();
        space();
        if (text[i] !== "{")
          throw new DotError("a subgraph without a body");
        i++;
        graph.subgraphs.push(block(sub, depth + 1));
        continue;
      }
      if (["node", "edge", "graph"].includes(first)) {
        space();
        if (text[i] === "[")
          attributes();
        continue;
      }
      space();
      if (text[i] === "=") {
        i++;
        graph.attrs[first] = word();
        continue;
      }
      if (text.startsWith("->", i)) {
        i += 2;
        const to = word();
        space();
        graph.edges.push({ from: first, to,
          attrs: text[i] === "[" ? attributes() : {} });
        continue;
      }
      graph.nodes.push({ id: first, attrs: text[i] === "[" ? attributes()
        : {} });
    }
  };
  space();
  const kind = word();
  if (kind !== "digraph")
    throw new DotError("not a digraph");
  const id = word();
  space();
  if (text[i] !== "{")
    throw new DotError("no graph body");
  i++;
  return block(id, 0);
}

const STATES = { "~": "VOID_PENDING", 0: "NULL", "-": "READY", "=": "PAUSED",
  ">": "PLAYING" };
const MODES = { ">": "push", "<": "pull", "-": "none" };

// "video/x-raw\n  format: I420\n  width: 1280" -> "video/x-raw, format=I420,
// width=1280" (as the page shows caps).
function capsText(label) {
  const lines = label.split("\n").map((l) => l.trim()).filter(Boolean);
  const out = [];
  for (const line of lines) {
    const m = /^([^:\s]+):\s*(.*)$/.exec(line);
    if (m && out.length)
      out[out.length - 1] += `, ${m[1]}=${m[2]}`;
    else
      out.push(line);
  }
  return out.join("; ");
}

// An element's label: "GstType\nname\n[state]\nkey=value..." (the top
// bin's type in <>).
function elementLabel(label) {
  const lines = (label || "").split("\n");
  const type = (lines[0] || "").replace(/^<(.*)>$/, "$1");
  const name = lines[1] ?? "";
  let state = null;
  let pending = null;
  let rest = lines.slice(2);
  const m = /^\[(.)\](?:\s*->\s*\[(.)\])?$/.exec(rest[0] || "");
  if (m) {
    state = STATES[m[1]] ?? null;
    pending = m[2] ? STATES[m[2]] ?? null : null;
    rest = rest.slice(1);
  }
  const properties = rest.filter(Boolean).map((line) => {
    const eq = line.indexOf("=");
    return eq > 0 ? { name: line.slice(0, eq), value: line.slice(eq + 1) }
      : { name: line, value: "" };
  });
  return { type, name, state, pending, properties };
}

// The page's graph from a parsed .dot. factories: GType name -> factory
// name (looked up, not read from the file), or null.
export function dotToGraph(dot, factories = null) {
  let nextId = 1;
  const padByNode = new Map();
  const proxies = new Map();
  const links = [];

  const padOf = (node, direction, elementPath) => {
    const label = (node.attrs.label || "").split("\n");
    const name = label[0];
    const marks = /^\[(.)\]/.exec(label[1] || "");
    const fill = node.attrs.fillcolor || "";
    const style = node.attrs.style || "";
    const pad = {
      id: nextId++, name, path: `${elementPath}:${name}`, direction,
      ghost: fill === "#ddddff" || fill === "#ffdddd",
    };
    // Solid is drawn for "always" and for a pad without a template alike:
    // only dotted and dashed say what they are.
    if (style.includes("dotted"))
      pad.presence = "sometimes";
    else if (style.includes("dashed"))
      pad.presence = "request";
    if (marks && MODES[marks[1]])
      pad.mode = MODES[marks[1]];
    padByNode.set(node.id, pad);
    return pad;
  };

  const element = (graph, label, parentPath) => {
    const info = elementLabel(label);
    const path = parentPath ? `${parentPath}/${info.name}` : info.name;
    const children = graph.subgraphs.filter((s) =>
      /^cluster_/.test(s.id) && !/_(sink|src)$/.test(s.id));
    const factory = factories?.[info.type];
    const e = {
      id: nextId++, name: info.name, path,
      factory: typeof factory === "string" ? factory : null,
      // Not in the file: looked up from the type.
      factory_looked_up: typeof factory === "string",
      type: info.type, bin: children.length > 0 ||
        graph.attrs.fillcolor === "#ffffff" || !parentPath,
      state: info.state, pads: [], children: [], properties: info.properties,
    };
    if (info.pending)
      e.pending = info.pending;
    for (const s of graph.subgraphs) {
      const m = /_(sink|src)$/.exec(s.id);
      if (!m || !/^cluster_/.test(s.id))
        continue;
      for (const node of s.nodes) {
        if (node.id.startsWith("_"))
          proxies.set(node.id, null);
        else
          e.pads.push(padOf(node, m[1], path));
      }
      // A ghost pad's line to its proxy pad is drawn in the pad's block.
      collect(s);
    }
    for (const node of graph.nodes)
      if (node.id.startsWith("_"))
        proxies.set(node.id, null);
    collect(graph);
    for (const child of children)
      e.children.push(element(child, child.attrs.label, path));
    return e;
  };

  const edges = [];
  const collect = (graph) => {
    for (const edge of graph.edges)
      edges.push(edge);
  };

  const pipeline = element(dot, dot.attrs.label, "");

  // A ghost pad and its proxy pad are joined by a dashed line: links
  // through the proxy are the ghost pad's.
  for (const edge of edges) {
    if (edge.attrs.style !== "dashed")
      continue;
    if (padByNode.has(edge.from) && proxies.has(edge.to))
      proxies.set(edge.to, edge.from);
    else if (padByNode.has(edge.to) && proxies.has(edge.from))
      proxies.set(edge.from, edge.to);
  }
  const resolve = (id) => padByNode.get(proxies.get(id) ?? id);
  for (const edge of edges) {
    if (edge.attrs.style === "invis" || edge.attrs.style === "dashed")
      continue;
    const src = resolve(edge.from);
    const sink = resolve(edge.to);
    if (!src || !sink || src === sink)
      continue;
    links.push({ src: src.id, sink: sink.id });
    if (edge.attrs.label && edge.attrs.label.trim()) {
      const caps = capsText(edge.attrs.label);
      src.caps ??= caps;
      sink.caps ??= caps;
    } else if (edge.attrs.taillabel || edge.attrs.headlabel) {
      // Not negotiated: what each side accepts.
      src.accepts = capsText(edge.attrs.taillabel || "");
      sink.accepts = capsText(edge.attrs.headlabel || "");
    }
  }
  return { pipelines: [pipeline], links, threads: [] };
}

// Whether text looks like a .dot file (not a recording).
export function looksLikeDot(text) {
  return /^\s*(strict\s+)?digraph\b/.test(text.slice(0, 256));
}

let factoryTable = null;

// The type -> factory table (web/factories.json), loaded once; null if it
// cannot be read (factories are then not shown).
export async function factories() {
  if (factoryTable === null) {
    factoryTable = fetch("factories.json").then((r) => r.ok ? r.json() : null)
      .then((t) => t?.types ?? null).catch(() => null);
  }
  return factoryTable;
}

// A .dot file as a recording of one moment (a snapshot), so the viewer
// shows it as any other: the graph, then the properties GStreamer wrote.
// started: when the file was written, if known (µs since 1970).
export function dotToRecording(text, name, table, started) {
  const graph = dotToGraph(parseDot(text), table);
  const time = Number.isFinite(started) ? Math.round(started) : 0;
  const elements = [];
  const visit = (e) => {
    if (e.properties.length)
      elements.push({ element: e.id, path: e.path, properties:
        e.properties.map((p) => ({ name: p.name, type: "", writable: false,
          value: p.value })) });
    delete e.properties;
    e.children.forEach(visit);
  };
  graph.pipelines.forEach(visit);
  const lines = [
    { format: "pipeline-monitor-recording", version: 1, protocol: 1,
      kind: "snapshot", started: time, source: "dot", name,
      hello: { version: null, gstreamer: null, program: null }, view: null },
    { type: "graph", protocol: 1, time, ...graph },
    { type: "properties", protocol: 1, time, elements },
    { type: "end", protocol: 1, time, reason: "snapshot" },
  ];
  return lines.map((l) => JSON.stringify(l)).join("\n") + "\n";
}
