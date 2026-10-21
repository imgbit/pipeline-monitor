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

// Caps as a list: media type and features, then one field per line.
export function capsList(caps) {
  const box = document.createElement("div");
  box.className = "caps-list";
  if (!caps) {
    add(box, "div", "no caps yet", "muted");
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
  show(x, y, heading, rows, caps) {
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
      n.appendChild(capsList(caps));
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
