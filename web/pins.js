// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Pinned pads: a card with live values for each, anchored next to its pad
// or detached (fixed on screen). Pins are kept per pad path, so they come
// back after a reload or when the pad is created again.

import { formatBytes, formatSpan, formatStamp, rawNs } from "./units.js";

const PINS_KEY = "pipeline-monitor.pins";
const SVG = "http://www.w3.org/2000/svg";
// Default place of an anchored card, from its pad.
const OFFSET = { dx: 16, dy: 16 };
// Dragging an anchored card further than this detaches it.
const DETACH = 100;

// [key, label, title]; an empty entry starts a new group.
const ROWS = [
  ["rate", "buffers/s", "Over the last second, or longer (up to 10 s) " +
    "until two buffers are in it"],
  ["bytes", "bytes/s", "Over the same time as buffers/s"],
  ["gaps", "gaps", "Time between buffers, min / avg / max, over the same " +
    "time as buffers/s"],
  ["age", "last buffer", "How long ago the last buffer passed the pad"],
  ["delay", "delay", "Clock time when the buffer passed the pad, against " +
    "its running time"],
  [],
  ["pts", "PTS"],
  ["dts", "DTS"],
  ["duration", "duration"],
  ["running", "running time"],
  [],
  ["size", "size"],
  ["offset", "offset", "Frame or sample number for raw video or audio, " +
    "byte position for byte streams; set by the element that made the " +
    "buffer"],
  ["flags", "flags"],
  [],
  ["segment", "segment"],
  ["start", "start"],
  ["base", "base"],
  [],
  ["totals", "total"],
  ["events", "events", "Seen since the pad was pinned"],
];

// Set by GStreamer for its own bookkeeping, not by the stream.
const HIDDEN_FLAGS = new Set(["tag-memory"]);

function add(parent, tag, cls, content) {
  const node = document.createElement(tag);
  if (cls)
    node.className = cls;
  if (content !== undefined)
    node.textContent = content;
  parent.appendChild(node);
  return node;
}

// Line icons in a 12×12 box, drawn like the graph.
const ICONS = {
  fold: '<path d="M3 4.5 6 7.5 9 4.5"/>',
  back: '<path d="M10 6H2.5M5.5 3 2.5 6 5.5 9"/>',
  close: '<path d="M3 3 9 9M9 3 3 9"/>',
};

function button(parent, cls, icon, title) {
  const b = add(parent, "button", cls);
  b.type = "button";
  b.title = title;
  b.innerHTML = `<svg viewBox="0 0 12 12" aria-hidden="true">${ICONS[icon]}` +
    "</svg>";
  return b;
}

function load() {
  try {
    return JSON.parse(localStorage.getItem(PINS_KEY)) || [];
  } catch {
    return [];
  }
}

function late(delay, unit) {
  if (delay === null || delay === undefined)
    return "—";
  return `${formatSpan(Math.abs(delay), unit)} ${delay >= 0 ? "late" : "early"}`;
}

// Several spans, with the unit written once when they share it.
function spans(values, unit) {
  const parts = values.map((v) => formatSpan(v, unit));
  const units = parts.map((p) => p.split(" ")[1]);
  if (units.every((u) => u === units[0]))
    return `${parts.map((p) => p.split(" ")[0]).join(" / ")} ${units[0]}`;
  return parts.join(" / ");
}

export class Pins {
  // handlers: send(command), portPoint(portId) -> {x, y} or null,
  // highlight(portId or null), reveal(portId), changed().
  constructor(layer, lines, handlers) {
    this.layer = layer;
    this.lines = lines;
    this.handlers = handlers;
    this.unit = "auto";
    this.hovered = null;
    this.pins = new Map();
    for (const saved of load())
      this.create(saved);
  }

  save() {
    const list = [...this.pins.values()].map(({ path, detached, x, y, dx, dy,
      folded }) => ({ path, detached, x, y, dx, dy, folded }));
    try {
      localStorage.setItem(PINS_KEY, JSON.stringify(list));
    } catch {
      // Storage may be unavailable; pins still work for this session.
    }
  }

  create(saved) {
    const pin = {
      ...OFFSET, detached: false, x: 0, y: 0, folded: false, ...saved,
      id: null, sent: false, gone: false, stats: null, video: false,
    };
    this.buildCard(pin);
    this.pins.set(pin.path, pin);
    return pin;
  }

  has(path) {
    return this.pins.has(path);
  }

  // Port ids ("p12") of pinned pads, for the graph.
  ports() {
    return new Set([...this.pins.values()].filter((p) => p.id !== null)
      .map((p) => `p${p.id}`));
  }

  // The live values of a pinned pad, for its tooltip.
  statsFor(id) {
    const pin = [...this.pins.values()].find((p) => p.id === id);
    return pin && !pin.gone ? pin.stats : null;
  }

  toggle(path) {
    const pin = this.pins.get(path);
    if (pin) {
      this.unpin(pin);
      return;
    }
    this.create({ path });
    this.save();
    this.handlers.changed();
  }

  unpin(pin) {
    if (pin.sent && pin.id !== null)
      this.handlers.send({ type: "unmonitor", pad: pin.id });
    if (this.hovered === pin)
      this.hover(null);
    pin.card.remove();
    this.pins.delete(pin.path);
    this.save();
    this.handlers.changed();
  }

  // A new connection knows nothing of our pads.
  connected() {
    for (const pin of this.pins.values())
      pin.sent = false;
  }

  // Matches pins to the pads of a new graph; pads: Map path -> {id, caps}.
  sync(pads) {
    for (const pin of this.pins.values()) {
      const pad = pads.get(pin.path);
      if (!pad) {
        if (pin.id !== null)
          this.setGone(pin);
        continue;
      }
      pin.video = (pad.caps || "").startsWith("video/");
      if (pad.id !== pin.id) {
        if (pin.sent && pin.id !== null)
          this.handlers.send({ type: "unmonitor", pad: pin.id });
        pin.id = pad.id;
        pin.sent = false;
      }
      if (!pin.sent) {
        this.handlers.send({ type: "monitor", pad: pin.id });
        pin.sent = true;
      }
    }
  }

  setGone(pin) {
    pin.gone = true;
    pin.sent = false;
    pin.id = null;
    this.fill(pin);
  }

  // A pad-stats message.
  apply(stats) {
    for (const entry of stats.pads) {
      const pin = [...this.pins.values()].find((p) => p.id === entry.pad);
      if (!pin)
        continue;
      if (entry.gone) {
        this.setGone(pin);
        continue;
      }
      pin.gone = false;
      pin.stats = entry;
      this.fill(pin);
    }
    // Cards change size with their values.
    this.place();
  }

  setUnit(unit) {
    this.unit = unit;
    for (const pin of this.pins.values())
      this.fill(pin);
  }

  buildCard(pin) {
    const card = add(this.layer, "div", "pin-card");
    pin.card = card;
    const head = add(card, "div", "pin-head");
    const name = pin.path.split("/").pop();
    add(head, "span", "pin-title", name).title =
      `${pin.path}\nClick to show the pad, drag to move`;
    pin.summary = add(head, "span", "pin-summary");
    const fold = button(head, "pin-fold", "fold", "Fold");
    const anchor = button(head, "pin-anchor", "back", "Back to its pad");
    const close = button(head, "pin-close", "close", "Unpin");
    pin.note = add(card, "div", "pin-note", "waiting for data…");
    const dl = add(card, "dl", "pin-rows");
    pin.cells = {};
    let group = false;
    for (const [key, label, title] of ROWS) {
      if (!key) {
        group = true;
        continue;
      }
      const dt = add(dl, "dt", group ? "group" : undefined, label);
      if (title)
        dt.title = title;
      pin.cells[key] = add(dl, "dd", group ? "group" : undefined);
      group = false;
    }

    fold.addEventListener("click", () => {
      pin.folded = !pin.folded;
      this.save();
      this.fill(pin);
      this.place();
    });
    anchor.addEventListener("click", () => {
      pin.detached = false;
      Object.assign(pin, OFFSET);
      this.save();
      this.fill(pin);
      this.place();
    });
    close.addEventListener("click", () => this.unpin(pin));
    card.addEventListener("pointerenter", () => this.hover(pin));
    card.addEventListener("pointerleave", () => this.hover(null));
    this.setupDrag(pin, head);
    this.fill(pin);
  }

  hover(pin) {
    this.hovered = pin;
    this.handlers.highlight(pin && pin.id !== null ? `p${pin.id}` : null);
    this.place();
  }

  setupDrag(pin, head) {
    let drag = null;

    head.addEventListener("pointerdown", (e) => {
      if (e.button !== 0 || e.target.closest("button"))
        return;
      drag = { x: e.clientX, y: e.clientY, moved: false,
        left: pin.card.offsetLeft, top: pin.card.offsetTop,
        dx: pin.dx, dy: pin.dy };
      head.setPointerCapture(e.pointerId);
    });
    head.addEventListener("pointermove", (e) => {
      if (!drag)
        return;
      const mx = e.clientX - drag.x;
      const my = e.clientY - drag.y;
      if (!drag.moved && Math.hypot(mx, my) < 4)
        return;
      drag.moved = true;
      if (pin.detached) {
        pin.x = drag.left + mx;
        pin.y = drag.top + my;
      } else {
        pin.dx = drag.dx + mx;
        pin.dy = drag.dy + my;
        if (Math.hypot(pin.dx - OFFSET.dx, pin.dy - OFFSET.dy) > DETACH) {
          pin.detached = true;
          // Carries on from where the card is now.
          drag = { ...drag, x: e.clientX, y: e.clientY,
            left: pin.card.offsetLeft, top: pin.card.offsetTop };
          this.fill(pin);
        }
      }
      this.place();
    });
    head.addEventListener("pointerup", () => {
      if (drag && !drag.moved && pin.id !== null)
        this.handlers.reveal(`p${pin.id}`);
      else if (drag)
        this.save();
      drag = null;
    });
  }

  // Writes the latest values into the card.
  fill(pin) {
    const card = pin.card;
    const s = pin.stats;
    const unit = this.unit;
    const c = pin.cells;
    card.classList.toggle("detached", pin.detached);
    card.classList.toggle("folded", pin.folded);
    card.classList.toggle("gone", pin.gone);
    card.querySelector(".pin-fold").title = pin.folded ? "Unfold" : "Fold";
    pin.note.hidden = !pin.gone && Boolean(s);
    pin.note.textContent = pin.gone
      ? (s ? "pad is gone, last values:" : "pad is gone")
      : "waiting for data…";
    if (!s) {
      pin.summary.textContent = "";
      return;
    }

    const set = (key, content, title = "") => {
      c[key].textContent = content;
      c[key].title = title;
    };
    const w = s.window;
    const l = s.last;
    // The window grows for sparse streams; say over how long.
    const over = w && w.seconds > 1.5 ? ` (${Math.round(w.seconds)} s)` : "";
    const rate = w ? `${w.buffers_per_s.toFixed(1)}${pin.video ? " ≈ fps" : ""}` +
      over : "—";
    set("rate", rate);
    set("bytes", w ? `${formatBytes(w.bytes_per_s)}/s` : "—");
    const gaps = w ? [w.gap_min, w.gap_avg, w.gap_max] : [];
    set("gaps", w && w.gap_avg !== null ? spans(gaps, unit) : "—",
      gaps.map(rawNs).join(" / "));
    set("age", l ? `${formatSpan(l.age, unit)} ago` : "—",
      l ? rawNs(l.age) : "");
    set("delay", l ? late(l.delay, unit) : "—", l ? rawNs(l.delay) : "");
    set("pts", l ? formatStamp(l.pts, unit) : "—", l ? rawNs(l.pts) : "");
    set("dts", l ? formatStamp(l.dts, unit) : "—", l ? rawNs(l.dts) : "");
    set("duration", l ? formatSpan(l.duration, unit) : "—",
      l ? rawNs(l.duration) : "");
    set("running", l ? formatStamp(l.running_time, unit) : "—",
      l ? rawNs(l.running_time) : "");
    set("size", l ? formatBytes(l.size) : "—", l ? `${l.size} bytes` : "");
    set("offset", l && l.offset !== null
      ? `${l.offset}${l.offset_end !== null ? ` – ${l.offset_end}` : ""}` : "—");
    const flags = l ? l.flags.filter((f) => !HIDDEN_FLAGS.has(f)) : [];
    set("flags", flags.length ? flags.join(", ") : "—");
    const seg = s.segment;
    const time = seg?.format === "time";
    const segValue = (n) => (time ? formatStamp(n, unit) : (n ?? "—"));
    set("segment", seg ? `${seg.format}${seg.rate === 1 ? ""
      : `, rate ${seg.rate}`}` : "—");
    set("start", seg ? segValue(seg.start) : "—", time ? rawNs(seg.start) : "");
    set("base", seg ? segValue(seg.base) : "—", time ? rawNs(seg.base) : "");
    set("totals", `${s.buffers} buffers, ${formatBytes(s.bytes)}`,
      `${s.bytes} bytes`);
    const events = Object.entries(s.events).filter(([, n]) => n > 0)
      .map(([name, n]) => `${name} ${n}`);
    set("events", events.length ? events.join(", ") : "—");
    pin.summary.textContent = pin.folded
      ? `${w ? w.buffers_per_s.toFixed(1) : "—"}/s · ${l ? late(l.delay, unit)
        : "—"}` : "";
  }

  // Puts every card in place and draws the lines to the pads. Called after
  // each render and whenever the graph is panned or zoomed.
  place() {
    const width = this.layer.clientWidth;
    const height = this.layer.clientHeight;
    this.lines.replaceChildren();
    for (const pin of this.pins.values()) {
      const card = pin.card;
      const point = pin.id !== null ? this.handlers.portPoint(`p${pin.id}`)
        : null;
      if (point)
        pin.lastPoint = point;
      // A gone pad's card stays where the pad was; a pad that is only not
      // drawn (collapsed bin, other pipeline) hides its card.
      const at = point || (pin.gone ? pin.lastPoint : null);
      if (!pin.detached && !at) {
        card.hidden = true;
        continue;
      }
      card.hidden = false;
      let left = pin.detached ? pin.x : at.x + pin.dx;
      let top = pin.detached ? pin.y : at.y + pin.dy;
      if (pin.detached) {
        // Kept on screen when the window gets smaller.
        left = Math.max(0, Math.min(left, width - card.offsetWidth));
        top = Math.max(0, Math.min(top, height - card.offsetHeight));
      }
      card.style.left = `${left}px`;
      card.style.top = `${top}px`;
      if (point && (!pin.detached || this.hovered === pin))
        this.line(point, left, top, card, pin.detached);
    }
  }

  // From the pad to the nearest point of the card.
  line(point, left, top, card, dashed) {
    const x = Math.max(left, Math.min(point.x, left + card.offsetWidth));
    const y = Math.max(top, Math.min(point.y, top + card.offsetHeight));
    const l = document.createElementNS(SVG, "line");
    l.setAttribute("x1", point.x);
    l.setAttribute("y1", point.y);
    l.setAttribute("x2", x);
    l.setAttribute("y2", y);
    l.setAttribute("class", dashed ? "pin-line dashed" : "pin-line");
    this.lines.appendChild(l);
  }
}
