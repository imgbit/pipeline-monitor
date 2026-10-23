// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Draws an ELK layout as SVG and handles pan and zoom.

import { fillClass, percent, queueFill } from "./levels.js";


const SVG = "http://www.w3.org/2000/svg";

function el(name, attrs = {}, parent = null) {
  const node = document.createElementNS(SVG, name);
  for (const [k, v] of Object.entries(attrs))
    node.setAttribute(k, v);
  if (parent)
    parent.appendChild(node);
  return node;
}

function text(parent, x, y, cls, content, anchor = "start") {
  const t = el("text", { x, y, class: cls, "text-anchor": anchor }, parent);
  t.textContent = content;
  return t;
}

function title(parent, content) {
  el("title", {}, parent).textContent = content;
}

function walk(node, fn) {
  fn(node);
  for (const child of node.children || [])
    walk(child, fn);
}

export class GraphView {
  constructor(svg, handlers) {
    this.svg = svg;
    this.handlers = handlers;
    this.transform = { x: 0, y: 0, k: 1 };
    this.bounds = null;
    this.autoFit = true;

    const defs = el("defs", {}, svg);
    // The same size on every line, thin or thick; coloured ones for links
    // with a problem.
    for (const id of ["arrow", "arrow-warning", "arrow-error"]) {
      const marker = el("marker", {
        id, viewBox: "0 0 8 8", refX: 8, refY: 4, markerUnits: "userSpaceOnUse",
        markerWidth: 7, markerHeight: 7, orient: "auto-start-reverse",
      }, defs);
      el("path", { d: "M0,0 L8,4 L0,8 z" }, marker);
    }
    this.viewport = el("g", {}, svg);

    this.setupPanZoom();
  }

  setupPanZoom() {
    let drag = null;

    this.svg.addEventListener("pointerdown", (e) => {
      if (e.button !== 0)
        return;
      drag = { x: e.clientX, y: e.clientY, tx: this.transform.x,
        ty: this.transform.y, moved: false, target: e.target };
      this.svg.setPointerCapture(e.pointerId);
    });
    this.svg.addEventListener("pointermove", (e) => {
      if (!drag) {
        this.hover(e);
        return;
      }
      const dx = e.clientX - drag.x;
      const dy = e.clientY - drag.y;
      if (!drag.moved && Math.hypot(dx, dy) < 4)
        return;
      drag.moved = true;
      this.handlers.hover(null);
      this.svg.classList.add("panning");
      this.transform.x = drag.tx + dx;
      this.transform.y = drag.ty + dy;
      this.setAutoFit(false);
      this.apply();
    });
    this.svg.addEventListener("pointerup", () => {
      if (drag && !drag.moved)
        this.click(drag.target);
      drag = null;
      this.svg.classList.remove("panning");
    });
    this.svg.addEventListener("pointerleave", () => this.handlers.hover(null));
    this.svg.addEventListener("wheel", (e) => {
      e.preventDefault();
      const r = this.svg.getBoundingClientRect();
      this.zoomAt(Math.exp(-e.deltaY * 0.002), e.clientX - r.left,
        e.clientY - r.top);
    }, { passive: false });
  }

  // Reports the pad, link or queue under the pointer: {port}, {src, sink}
  // or {level}.
  hover(e) {
    const target = e.target.closest("[data-port], [data-edge], [data-level]");
    if (!target) {
      this.handlers.hover(null);
    } else if (target.dataset.port) {
      this.handlers.hover({ port: target.dataset.port }, e.clientX, e.clientY);
    } else if (target.dataset.level) {
      this.handlers.hover({ level: Number(target.dataset.level) }, e.clientX,
        e.clientY);
    } else {
      const [src, sink] = target.dataset.edge.split(" ");
      this.handlers.hover({ src, sink }, e.clientX, e.clientY);
    }
  }

  // Any interaction stops auto-fit, so nothing moves under the cursor.
  setAutoFit(on) {
    if (this.autoFit === on)
      return;
    this.autoFit = on;
    this.handlers.autoFit(on);
    if (on)
      this.fit();
  }

  click(target) {
    this.setAutoFit(false);
    const toggle = target.closest(".toggle");
    if (toggle) {
      this.handlers.toggle(toggle.dataset.path);
      return;
    }
    const port = target.closest("[data-port]");
    if (port) {
      this.handlers.pin(port.dataset.port);
      return;
    }
    const node = target.closest(".node");
    this.handlers.select(node ? Number(node.dataset.id) : null);
  }

  apply() {
    const { x, y, k } = this.transform;
    this.viewport.setAttribute("transform", `translate(${x},${y}) scale(${k})`);
    this.handlers.moved();
  }

  // The drawn shape of a target: "p<id>" a pad, "e<id>" an element's box.
  targetNode(target) {
    if (target.startsWith("e"))
      return this.boxes?.get(Number(target.slice(1)))?.g;
    return this.viewport.querySelector(`[data-port="${target}"]`);
  }

  // Where a card's line ends, relative to the SVG, or null if not drawn:
  // the middle of a pad, the top right corner of an element.
  portPoint(target) {
    const node = this.targetNode(target);
    if (!node)
      return null;
    const s = this.svg.getBoundingClientRect();
    if (target.startsWith("e")) {
      const r = node.querySelector(":scope > rect.box").getBoundingClientRect();
      return { x: r.right - s.x, y: r.y + 10 - s.y };
    }
    const r = node.getBoundingClientRect();
    return { x: r.x + r.width / 2 - s.x, y: r.y + r.height / 2 - s.y };
  }

  // Pans so that a pad is in the middle.
  reveal(port) {
    const point = this.portPoint(port);
    if (!point)
      return;
    const r = this.svg.getBoundingClientRect();
    this.setAutoFit(false);
    this.transform.x += r.width / 2 - point.x;
    this.transform.y += r.height / 2 - point.y;
    this.apply();
  }

  // Fill bars on queue nodes; queues: Map element id -> queue from the
  // "levels" message. Drawn over the nodes, redrawn on every message.
  setLevels(queues) {
    if (!this.levelLayer)
      return;
    this.levelLayer.replaceChildren();
    for (const [id, queue] of queues) {
      const box = this.boxes?.get(id);
      if (!box)
        continue;
      const { g, node } = box;
      if (!g.dataset.level) {
        g.dataset.level = id;
        // The levels tooltip replaces the browser's.
        g.querySelector(":scope > title")?.remove();
      }
      const fill = queueFill(queue);
      const cls = `level ${fillClass(fill.ratio)}`;
      const x = node.x + 6;
      const width = node.width - 12;
      const y = node.y + node.height - 5;
      el("rect", { class: "level-track", x, y, width, height: 3, rx: 1.5 },
        this.levelLayer);
      el("rect", { class: `${cls} level-fill`, x, y, height: 3, rx: 1.5,
        width: Math.max(0, Math.min(1, fill.ratio)) * width },
      this.levelLayer);
      text(this.levelLayer, node.x + node.width - 8, node.y + 33,
        `${cls} level-text`, fill.ratio > 0 ? `${percent(fill.ratio)} ` +
          fill.limit : "0 %", "end");
    }
  }

  // Latency labels under measured elements; labels: Map element id ->
  // {text, cls, title}. Redrawn on every "latency" message.
  setLatency(labels) {
    if (!this.latencyLayer)
      return;
    this.latencyLayer.replaceChildren();
    for (const [id, label] of labels) {
      const node = this.boxes?.get(id)?.node;
      if (!node)
        continue;
      const t = text(this.latencyLayer, node.x + node.width / 2,
        node.y + node.height + 11, `latency-text ${label.cls}`, label.text,
        "middle");
      title(t, label.title);
    }
  }

  // Marks pads, elements and links with problems; pads, elements: Map id
  // -> "error" | "warning" | "info". Done on the drawn graph, no new
  // layout; applied again after every render.
  setProblems(pads, elements) {
    this.problems = { pads, elements };
    if (!this.viewport)
      return;
    for (const node of this.viewport.querySelectorAll("[class*='problem-']"))
      node.classList.remove("problem-error", "problem-warning", "problem-info");
    for (const [id, severity] of pads) {
      this.viewport.querySelector(`[data-port="p${id}"]`)
        ?.classList.add(`problem-${severity}`);
      // The link leaving a src pad where data stopped.
      const hit = [...this.viewport.querySelectorAll(".edge-hit")]
        .find((e) => e.dataset.edge?.startsWith(`p${id} `));
      hit?.previousElementSibling?.classList.add(`problem-${severity}`);
    }
    for (const [id, severity] of elements)
      this.boxes?.get(id)?.g.classList.add(`problem-${severity}`);
  }

  // Kept across renders: the graph is redrawn while a card is hovered.
  highlight(target) {
    this.highlighted = target;
    for (const node of this.viewport.querySelectorAll(".highlight"))
      node.classList.remove("highlight");
    if (target)
      this.targetNode(target)?.classList.add("highlight");
  }

  zoomAt(factor, cx, cy) {
    const k = Math.min(4, Math.max(0.1, this.transform.k * factor));
    const f = k / this.transform.k;
    this.transform.x = cx - (cx - this.transform.x) * f;
    this.transform.y = cy - (cy - this.transform.y) * f;
    this.transform.k = k;
    this.setAutoFit(false);
    this.apply();
  }

  zoomCenter(factor) {
    const r = this.svg.getBoundingClientRect();
    this.zoomAt(factor, r.width / 2, r.height / 2);
  }

  fit() {
    const r = this.svg.getBoundingClientRect();
    if (!this.bounds || r.width === 0 || r.height === 0)
      return;
    const { width, height } = this.bounds;
    const k = Math.min(1.5, (r.width - 32) / width, (r.height - 32) / height);
    this.transform = {
      k,
      x: (r.width - width * k) / 2,
      y: (r.height - height * k) / 2,
    };
    this.apply();
  }

  clear() {
    this.viewport.replaceChildren();
    this.bounds = null;
  }

  // layout: ELK output with root coordinates; info: maps from layout.js.
  render(layout, info, view) {
    this.boxes = new Map();
    const bins = el("g");
    const edges = el("g");
    const nodes = el("g");
    const labels = el("g");

    for (const edge of layout.edges || [])
      this.drawEdge(edges, labels, edge, info, view);

    for (const child of layout.children || [])
      walk(child, (node) => {
        const meta = info.nodes.get(node.id);
        if (meta.kind === "bin")
          this.drawBin(bins, node, meta, info, view);
        else
          this.drawNode(nodes, node, meta, info, view);
      });

    this.levelLayer = el("g");
    this.latencyLayer = el("g");
    this.viewport.replaceChildren(bins, edges, nodes, labels, this.levelLayer,
      this.latencyLayer);
    this.bounds = { width: layout.width, height: layout.height };
    if (this.problems)
      this.setProblems(this.problems.pads, this.problems.elements);
    if (this.highlighted)
      this.highlight(this.highlighted);
    if (this.autoFit)
      this.fit();
    else
      this.handlers.moved();
  }

  nodeGroup(parent, node, meta, view, cls) {
    const e = meta.element;
    const classes = ["node", cls, e.state];
    if (e.pending)
      classes.push("pending");
    if (view.selected === e.id)
      classes.push("selected");
    if (view.appeared.has(e.id))
      classes.push("appeared");
    if (view.pinned.has(`e${e.id}`))
      classes.push("pinned");
    const g = el("g", { class: classes.join(" "), "data-id": e.id }, parent);
    el("rect", { class: "box", x: node.x, y: node.y, width: node.width,
      height: node.height, rx: 5 }, g);
    this.boxes.set(e.id, { g, node });
    return g;
  }

  drawHeader(g, node, e, factoryText) {
    el("circle", { class: "dot", cx: node.x + 12, cy: node.y + 15, r: 4 }, g);
    text(g, node.x + 21, node.y + 19, "name", e.name);
    text(g, node.x + 10, node.y + 33, "factory", factoryText);
    title(g, `${e.name} (${e.type})\nstate: ${e.state}` +
      (e.pending ? ` → ${e.pending}` : ""));
  }

  drawToggle(g, node, path, expanded) {
    const t = el("g", { class: "toggle", "data-path": path }, g);
    const x = node.x + node.width - 24;
    const y = node.y + 6;
    el("rect", { x, y, width: 18, height: 18, rx: 3 }, t);
    text(t, x + 9, y + 13, "", expanded ? "−" : "+", "middle");
    title(t, expanded ? "Collapse bin" : "Expand bin");
  }

  // Error count in red, otherwise the warning count in amber; grey if muted.
  drawBadge(g, node, badge, beforeToggle) {
    if (!badge)
      return;
    const n = badge.errors || badge.warnings;
    const cls = ["badge", badge.errors ? "has-errors" : "has-warnings"];
    if (badge.muted)
      cls.push("muted");
    const b = el("g", { class: cls.join(" ") }, g);
    const label = n > 99 ? "99+" : String(n);
    // A circle for one digit, a pill that grows for more.
    const width = 16 + (label.length - 1) * 6;
    const right = node.x + node.width - (beforeToggle ? 28 : 4);
    const y = node.y + 7;
    el("rect", { x: right - width, y, width, height: 16, rx: 8 }, b);
    text(b, right - width / 2, y + 11.5, "", label, "middle");
    title(b, [badge.errors && `${badge.errors} errors`,
      badge.warnings && `${badge.warnings} warnings`,
      badge.muted && "muted"].filter(Boolean).join(", "));
  }

  drawBin(parent, node, meta, info, view) {
    const g = this.nodeGroup(parent, node, meta, view, "bin");
    this.drawHeader(g, node, meta.element, meta.element.factory ||
      meta.element.type);
    this.drawToggle(g, node, meta.path, true);
    this.drawBadge(g, node, view.badges.get(meta.element.id), true);
    this.drawPorts(g, node, info, view);
  }

  drawNode(parent, node, meta, info, view) {
    const e = meta.element;
    const collapsed = meta.kind === "collapsed";
    const g = this.nodeGroup(parent, node, meta, view,
      collapsed ? "collapsed" : "element");
    let factory = e.factory || e.type;
    if (collapsed)
      factory += ` · ${countElements(e)} inside`;
    this.drawHeader(g, node, e, factory);
    if (collapsed)
      this.drawToggle(g, node, meta.path, false);
    this.drawBadge(g, node, view.badges.get(e.id), collapsed);
    this.drawPorts(g, node, info, view);
  }

  drawPorts(g, node, info, view) {
    for (const port of node.ports || []) {
      const p = info.ports.get(port.id);
      const cls = ["pad", p.pad.presence || "", p.pad.ghost ? "ghost" : "",
        view.pinned.has(port.id) ? "pinned" : ""];
      const cx = port.x + port.width / 2;
      const cy = port.y + port.height / 2;
      let shape;
      if (p.pad.presence === "request")
        shape = el("circle", { cx, cy, r: port.width / 2 }, g);
      else
        shape = el("rect", { x: port.x, y: port.y, width: port.width,
          height: port.height, rx: 1 }, g);
      shape.setAttribute("class", cls.filter(Boolean).join(" "));
      shape.dataset.port = port.id;
      if (p.labelled) {
        if (p.west)
          text(g, port.x + port.width + 4, cy + 4, "padlabel", p.pad.name);
        else
          text(g, port.x - 4, cy + 4, "padlabel", p.pad.name, "end");
      }
    }
  }

  drawEdge(parent, labels, edge, info, view) {
    const selected = view.selectedPads.has(edge.sources[0]) ||
      view.selectedPads.has(edge.targets[0]);

    for (const s of edge.sections || []) {
      const pts = [s.startPoint, ...(s.bendPoints || []), s.endPoint];
      const d = pts.map((p, i) => `${i ? "L" : "M"}${p.x},${p.y}`).join(" ");
      el("path", { d, class: selected ? "edge selected" : "edge",
        "marker-end": "url(#arrow)" }, parent);
      // Wider and invisible, so the thin line is easy to hover.
      el("path", { d, class: "edge-hit",
        "data-edge": `${edge.sources[0]} ${edge.targets[0]}` }, parent);
    }

    // Only with the "caps" switch: ELK has made room for the label.
    const placed = edge.labels?.[0];
    if (placed && placed.x !== undefined) {
      const g = el("g", { class: "edge-label",
        "data-edge": `${edge.sources[0]} ${edge.targets[0]}` }, labels);
      el("rect", { x: placed.x, y: placed.y, width: placed.width,
        height: placed.height, rx: 3 }, g);
      text(g, placed.x + 4, placed.y + 10.5, "", placed.text);
    }
  }
}

export function countElements(element) {
  return element.children.reduce((n, c) => n + 1 + countElements(c), 0);
}
