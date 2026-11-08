// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

const HEIGHT_KEY = "pipeline-monitor.panel-height";

// A header button: a fixed name, and a count (or other short text) when
// there is something; cls colours the count (error, warning, recording).
export function setCounter(button, name, count, cls) {
  const label = document.createElement("span");
  label.textContent = name;
  button.replaceChildren(label);
  if (count !== null && count !== undefined && count !== "") {
    const badge = document.createElement("span");
    badge.className = `count${cls ? ` ${cls}` : ""}`;
    badge.textContent = count;
    button.append(badge);
  }
}

// One panel at the bottom at a time: the header buttons open theirs and
// close it again; the panels' top edge sets the height for all of them.
// panels: name -> {button, panel, close}.
export class Dock {
  constructor(panels) {
    this.panels = panels;
    this.current = null;
    this.setHeight(this.loadHeight());
    for (const [name, { button, panel, close }] of Object.entries(panels)) {
      button.addEventListener("click",
        () => this.open(this.current === name ? null : name));
      close.addEventListener("click", () => this.open(null));
      const handle = document.createElement("div");
      handle.className = "panel-resize";
      handle.title = "Drag to resize";
      panel.prepend(handle);
      this.resizing(handle);
    }
  }

  open(name) {
    this.current = name;
    for (const [n, { button, panel }] of Object.entries(this.panels)) {
      panel.hidden = n !== name;
      button.setAttribute("aria-pressed", String(n === name));
      button.setAttribute("aria-expanded", String(n === name));
    }
  }

  resizing(handle) {
    handle.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      handle.setPointerCapture(e.pointerId);
      const move = (m) => this.setHeight(window.innerHeight - m.clientY);
      const up = () => {
        handle.removeEventListener("pointermove", move);
        handle.removeEventListener("pointerup", up);
        try {
          localStorage.setItem(HEIGHT_KEY, String(this.height));
        } catch {
          // Storage may be unavailable; the height still applies now.
        }
      };
      handle.addEventListener("pointermove", move);
      handle.addEventListener("pointerup", up);
    });
  }

  loadHeight() {
    try {
      return Number(localStorage.getItem(HEIGHT_KEY)) || null;
    } catch {
      return null;
    }
  }

  // Between 80 px and all but 150 px of the window; null: the default.
  setHeight(px) {
    this.height = px === null ? null
      : Math.max(80, Math.min(px, window.innerHeight - 150));
    document.documentElement.style.setProperty("--panel-height",
      this.height === null ? "35vh" : `${this.height}px`);
  }
}
