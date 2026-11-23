// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Tooltip for pads and links, shown at once (the browser's own tooltips
// appear after a delay and can only show plain text).

import { parseCaps } from "./caps.js";

function add(parent, tag, content, cls) {
  const node = document.createElement(tag);
  if (content !== undefined)
    node.textContent = content;
  if (cls)
    node.className = cls;
  parent.appendChild(node);
  return node;
}

// Caps as a list: media type and features, then one field per line. A
// pulled pad gets none: data is read from it without caps.
export function capsList(caps, pulled = false) {
  const box = document.createElement("div");
  box.className = "caps-list";
  if (!caps) {
    add(box, "div", pulled ? "no caps (data is pulled)" : "no caps",
      "muted");
    return box;
  }
  const { media, features, fields } = parseCaps(caps);
  add(box, "div", features ? `${media}(${features})` : media, "caps-media");
  const dl = add(box, "dl");
  for (const [name, value] of fields) {
    add(dl, "dt", name);
    add(dl, "dd", value);
  }
  return box;
}

export class Tooltip {
  constructor() {
    this.node = add(document.body, "div", undefined, "tooltip");
    this.node.setAttribute("role", "tooltip");
    this.node.hidden = true;
  }

  // rows: [[label, text], ...]; caps: a caps string, or undefined for none.
  show(x, y, heading, rows, caps, pulled = false) {
    const n = this.node;
    n.replaceChildren();
    add(n, "div", heading, "tooltip-heading");
    if (rows.length) {
      const dl = add(n, "dl");
      for (const [label, value] of rows) {
        add(dl, "dt", label);
        add(dl, "dd", value);
      }
    }
    if (caps !== undefined)
      n.appendChild(capsList(caps, pulled));
    n.hidden = false;
    this.move(x, y);
  }

  // Keeps the tooltip inside the window, below and right of the pointer.
  move(x, y) {
    const n = this.node;
    const pad = 14;
    const w = n.offsetWidth;
    const h = n.offsetHeight;
    const left = x + pad + w > innerWidth ? Math.max(4, x - pad - w) : x + pad;
    const top = y + pad + h > innerHeight ? Math.max(4, y - pad - h) : y + pad;
    n.style.left = `${left}px`;
    n.style.top = `${top}px`;
  }

  hide() {
    this.node.hidden = true;
  }
}

// Explanations of names and values, the same on desktop and phones: what
// has one is underlined with dots; its text shows on hover (after a
// moment) or on a tap or click, until a tap elsewhere or Escape. Buttons
// keep the browser's own hint of what they do.
// Whether text in node is selected: values there are then left as they
// are, so the selection is not lost to the next update.
export function selectingIn(node) {
  const s = document.getSelection();
  return Boolean(s && !s.isCollapsed && s.rangeCount &&
    node.contains(s.getRangeAt(0).commonAncestorContainer));
}

export function tip(node, text) {
  if (text)
    node.dataset.tip = text;
  else
    delete node.dataset.tip;
  return node;
}

export function initTips() {
  const pop = add(document.body, "div", undefined, "tooltip tip-pop");
  pop.setAttribute("role", "tooltip");
  pop.hidden = true;
  let timer = null;
  // The text waiting to be shown or shown, and whether a tap holds it.
  let pending = null;
  let held = false;
  let last = { x: 0, y: 0 };
  const hide = () => {
    clearTimeout(timer);
    pop.hidden = true;
    held = false;
    pending = null;
  };
  const show = (el) => {
    // Rebuilt since (the panel refreshes): the one now under the pointer.
    if (!el.isConnected)
      el = document.elementFromPoint(last.x, last.y)?.closest("[data-tip]");
    if (!el)
      return;
    pop.textContent = el.dataset.tip;
    pop.hidden = false;
    // Inside what is seen of the page (on a phone, the visual viewport).
    const width = Math.min(innerWidth, visualViewport?.width ?? innerWidth);
    const height = Math.min(innerHeight, visualViewport?.height ??
      innerHeight);
    pop.style.maxWidth = `${Math.min(360, width - 8)}px`;
    const r = el.getBoundingClientRect();
    const w = pop.offsetWidth;
    const h = pop.offsetHeight;
    const top = r.bottom + 6 + h > height ? r.top - 6 - h : r.bottom + 6;
    pop.style.left = `${Math.max(4, Math.min(r.left, width - w - 4))}px`;
    pop.style.top = `${Math.max(4, top)}px`;
  };
  document.addEventListener("pointermove", (e) => {
    last = { x: e.clientX, y: e.clientY };
  });
  document.addEventListener("pointerover", (e) => {
    if (e.pointerType !== "mouse" || held)
      return;
    const el = e.target.closest?.("[data-tip]");
    if (!el) {
      hide();
      return;
    }
    // The panel is rebuilt under a still pointer: the same label again
    // keeps the wait (or the tip) going.
    if (el.dataset.tip === pending)
      return;
    clearTimeout(timer);
    pop.hidden = true;
    pending = el.dataset.tip;
    timer = setTimeout(() => show(el), 300);
  });
  // A tap or click: on release, as the element pressed may have been
  // rebuilt by then (no click would come).
  document.addEventListener("pointerup", (e) => {
    const target = document.elementFromPoint(e.clientX, e.clientY);
    const el = target?.closest("[data-tip]");
    if (el && !target.closest("button, a")) {
      if (held && pending === el.dataset.tip) {
        hide();
        return;
      }
      clearTimeout(timer);
      pending = el.dataset.tip;
      show(el);
      held = true;
      return;
    }
    if (!pop.contains(target))
      hide();
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape")
      hide();
  });
}
