// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// The Performance panel: the whole program's CPU, memory and threads, and
// each thread's CPU over the last minute, from "performance" messages.

import { formatBytes } from "./units.js";

const SVG = "http://www.w3.org/2000/svg";
// A thread at this share of one core or more cannot go faster.
const BUSY = 0.95;

function el(tag, cls, text) {
  const node = document.createElement(tag);
  if (cls)
    node.className = cls;
  if (text !== undefined)
    node.textContent = text;
  return node;
}

function percent(cpu) {
  return cpu === null || cpu === undefined ? "–"
    : `${Math.round(cpu * 100)} %`;
}

// A small line of values (oldest first) from 0 to max.
function sparkline(values, max, colour, width, height) {
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("width", width);
  svg.setAttribute("height", height);
  svg.setAttribute("class", "spark");
  const points = values.map((v, i) => v === null ? null : [
    values.length > 1 ? i / (values.length - 1) * width : width,
    height - 1 - Math.min(1, v / max) * (height - 2)]);
  const d = points.map((p, i) => p && `${i && points[i - 1] ? "L" : "M"}${
    p[0].toFixed(1)},${p[1].toFixed(1)}`).filter(Boolean).join(" ");
  const path = document.createElementNS(SVG, "path");
  path.setAttribute("d", d);
  path.setAttribute("fill", "none");
  path.setAttribute("stroke", colour);
  svg.append(path);
  return svg;
}

export class PerformanceView {
  // nodes: {body, others}; handlers: {select(element id)}.
  constructor(nodes, handlers) {
    this.nodes = nodes;
    this.handlers = handlers;
    nodes.others.addEventListener("change", () => this.render());
  }

  // history: "performance" messages, oldest first, the last minute;
  // threads: the graph's view of threads (colours, names, the elements
  // each runs) or null; note: shown when there is no data.
  show(history, threads, note) {
    this.history = history;
    this.threads = threads;
    this.note = note;
    this.render();
  }

  render() {
    const body = this.nodes.body;
    const last = this.history?.at(-1);
    if (!last?.process && !last?.threads) {
      body.replaceChildren(el("div", "perf-note muted", this.note ??
        "No performance data yet"));
      return;
    }
    body.replaceChildren(this.summary(last), this.table(last));
  }

  summary(last) {
    const top = el("div", "perf-top");
    const process = last.process;
    const series = (pick) => this.history.map((m) => pick(m) ?? null);
    const cpu = series((m) => m.process?.cpu);
    const memory = series((m) => m.process?.memory);
    const card = (label, value, spark) => {
      const c = el("div", "perf-card");
      c.append(el("div", "perf-label muted", label), el("div", "perf-value",
        value));
      if (spark)
        c.append(spark);
      return c;
    };
    top.append(card("CPU", percent(process?.cpu), sparkline(cpu,
      Math.max(1, ...cpu.filter((v) => v !== null)), "var(--accent)", 200,
      32)));
    top.append(card("Memory", process ? formatBytes(process.memory) : "–",
      sparkline(memory, Math.max(1, ...memory.filter((v) => v !== null)) *
        1.1, "var(--paused)", 200, 32)));
    const threads = last.threads || [];
    const tasks = threads.filter((t) => t.task).length;
    const count = card("Threads", String(process?.threads ?? threads.length));
    count.append(el("div", "muted", `${tasks} GStreamer streaming, ${
      threads.length - tasks} other`));
    top.append(count);
    return top;
  }

  table(last) {
    const table = el("table", "perf-table");
    const head = el("tr");
    for (const [text, cls] of [["Thread"], [""], ["CPU", "num"],
      ["last minute"], ["Runs"]])
      head.append(el("th", cls, text));
    table.append(head);

    const all = last.threads || [];
    const known = all.filter((t) => t.id !== undefined);
    const others = all.filter((t) => t.id === undefined);
    const history = (os) => this.history.map((m) =>
      m.threads?.find((t) => t.os === os)?.cpu ?? null);
    for (const t of known)
      table.append(this.row(t, history(t.os)));
    if (this.nodes.others.checked) {
      for (const t of others)
        table.append(this.row(t, history(t.os)));
    } else if (others.length) {
      const cpu = others.reduce((sum, t) => sum + (t.cpu ?? 0), 0);
      const sums = this.history.map((m) => (m.threads || [])
        .filter((t) => t.id === undefined)
        .reduce((sum, t) => sum + (t.cpu ?? 0), 0));
      const tr = el("tr", "perf-others");
      tr.append(el("td", null, `other threads (${others.length})`),
        el("td", "muted", "move no data through pads"),
        el("td", "num", percent(cpu)));
      const spark = el("td");
      spark.append(sparkline(sums, Math.max(1, ...sums), "var(--muted)", 110,
        18));
      tr.append(spark, el("td", "muted", "–"));
      table.append(tr);
    }
    return table;
  }

  row(t, cpus) {
    const tr = el("tr");
    const name = el("td");
    const colour = this.threads?.colours.get(t.id)?.line;
    const swatch = el("span", "swatch");
    if (colour)
      swatch.style.background = colour;
    name.append(swatch, document.createTextNode(t.name || `thread ${t.os}`));
    const kind = t.task ? "streaming" : t.id !== undefined
      ? "pushes data, not a task" : "other";
    const graph = el("td");
    graph.append(sparkline(cpus, Math.max(1, ...cpus.filter((v) =>
      v !== null)), colour || "var(--muted)", 110, 18));
    const bar = el("span", "perf-bar");
    const fill = el("i", t.cpu >= BUSY ? "busy" : "");
    fill.style.width = `${Math.min(100, (t.cpu ?? 0) * 100)}%`;
    bar.append(fill);
    graph.append(bar);
    const runs = el("td");
    const elements = this.threads?.runs.get(t.id) || [];
    elements.forEach(({ id, name: label }, i) => {
      if (i)
        runs.append(", ");
      const link = el("button", "perf-element", label);
      link.addEventListener("click", () => this.handlers.select(id));
      runs.append(link);
    });
    if (!elements.length)
      runs.append(el("span", "muted", "–"));
    const cpu = el("td", `num${t.cpu >= BUSY ? " busy" : ""}`, percent(t.cpu));
    tr.append(name, el("td", "muted", kind), cpu, graph, runs);
    return tr;
  }
}
