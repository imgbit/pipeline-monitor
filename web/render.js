// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Draws an ELK layout as SVG and handles pan and zoom.

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

function padTooltip(pad) {
  const extra = [pad.direction, pad.presence, pad.ghost ? "ghost" : null];
  return `${pad.name} (${extra.filter(Boolean).join(", ")})`;
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
    const marker = el("marker", {
      id: "arrow", viewBox: "0 0 8 8", refX: 8, refY: 4,
      markerWidth: 7, markerHeight: 7, orient: "auto-start-reverse",
    }, defs);
    el("path", { d: "M0,0 L8,4 L0,8 z" }, marker);
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
      if (!drag)
        return;
      const dx = e.clientX - drag.x;
      const dy = e.clientY - drag.y;
      if (!drag.moved && Math.hypot(dx, dy) < 4)
        return;
      drag.moved = true;
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
    this.svg.addEventListener("wheel", (e) => {
      e.preventDefault();
      const r = this.svg.getBoundingClientRect();
      this.zoomAt(Math.exp(-e.deltaY * 0.002), e.clientX - r.left,
        e.clientY - r.top);
    }, { passive: false });
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
    const node = target.closest(".node");
    this.handlers.select(node ? Number(node.dataset.id) : null);
  }

  apply() {
    const { x, y, k } = this.transform;
    this.viewport.setAttribute("transform", `translate(${x},${y}) scale(${k})`);
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
    const bins = el("g");
    const edges = el("g");
    const nodes = el("g");

    for (const edge of layout.edges || [])
      this.drawEdge(edges, edge, view);

    for (const child of layout.children || [])
      walk(child, (node) => {
        const meta = info.nodes.get(node.id);
        if (meta.kind === "bin")
          this.drawBin(bins, node, meta, info, view);
        else
          this.drawNode(nodes, node, meta, info, view);
      });

    this.viewport.replaceChildren(bins, edges, nodes);
    this.bounds = { width: layout.width, height: layout.height };
    if (this.autoFit)
      this.fit();
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
    const g = el("g", { class: classes.join(" "), "data-id": e.id }, parent);
    el("rect", { class: "box", x: node.x, y: node.y, width: node.width,
      height: node.height, rx: 5 }, g);
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

  drawBin(parent, node, meta, info, view) {
    const g = this.nodeGroup(parent, node, meta, view, "bin");
    this.drawHeader(g, node, meta.element, meta.element.factory ||
      meta.element.type);
    this.drawToggle(g, node, meta.path, true);
    this.drawPorts(g, node, info);
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
    this.drawPorts(g, node, info);
  }

  drawPorts(g, node, info) {
    for (const port of node.ports || []) {
      const p = info.ports.get(port.id);
      const cls = ["pad", p.pad.presence || "", p.pad.ghost ? "ghost" : ""];
      const cx = port.x + port.width / 2;
      const cy = port.y + port.height / 2;
      let shape;
      if (p.pad.presence === "request")
        shape = el("circle", { cx, cy, r: port.width / 2 }, g);
      else
        shape = el("rect", { x: port.x, y: port.y, width: port.width,
          height: port.height, rx: 1 }, g);
      shape.setAttribute("class", cls.join(" ").trim());
      title(shape, padTooltip(p.pad));
      if (p.labelled) {
        if (p.west)
          text(g, port.x + port.width + 4, cy + 4, "padlabel", p.pad.name);
        else
          text(g, port.x - 4, cy + 4, "padlabel", p.pad.name, "end");
      }
    }
  }

  drawEdge(parent, edge, view) {
    for (const s of edge.sections || []) {
      const pts = [s.startPoint, ...(s.bendPoints || []), s.endPoint];
      const d = pts.map((p, i) => `${i ? "L" : "M"}${p.x},${p.y}`).join(" ");
      const cls = view.selectedPads.has(edge.sources[0]) ||
        view.selectedPads.has(edge.targets[0]) ? "edge selected" : "edge";
      el("path", { d, class: cls, "marker-end": "url(#arrow)" }, parent);
    }
  }
}

export function countElements(element) {
  return element.children.reduce((n, c) => n + 1 + countElements(c), 0);
}
