// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Details panel for the selected element.

import { countElements } from "./render.js";

function add(parent, tag, content, cls) {
  const node = document.createElement(tag);
  if (content !== undefined)
    node.textContent = content;
  if (cls)
    node.className = cls;
  parent.appendChild(node);
  return node;
}

export function showDetails(aside, selection, index, onClose) {
  if (!selection) {
    aside.hidden = true;
    aside.replaceChildren();
    return;
  }

  const { element, path } = selection;
  aside.replaceChildren();
  aside.hidden = false;

  const close = add(aside, "button", "×", "close");
  close.title = "Close";
  close.addEventListener("click", onClose);
  add(aside, "h2", element.name);

  const dl = add(aside, "dl");
  const row = (k, v) => {
    add(dl, "dt", k);
    add(dl, "dd", v);
  };
  row("factory", element.factory || "—");
  row("type", element.type);
  row("state", element.pending ? `${element.state} → ${element.pending}`
    : element.state);
  row("path", path);
  if (element.bin)
    row("contains", `${countElements(element)} elements`);

  if (element.pads.length === 0)
    return;

  const table = add(aside, "table");
  const head = add(table, "tr");
  for (const h of ["pad", "dir", "presence", "linked to"])
    add(head, "th", h);
  for (const pad of element.pads) {
    const tr = add(table, "tr");
    add(tr, "td", pad.ghost ? `${pad.name} (ghost)` : pad.name);
    add(tr, "td", pad.direction);
    add(tr, "td", pad.presence || "—");
    const peers = index.peers.get(pad.id) || [];
    add(tr, "td", peers.length
      ? peers.map((id) => index.padName(id)).join(", ") : "—");
  }
}
