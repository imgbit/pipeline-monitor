// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// A pinned video pad's picture, about once a second, with what it shows
// (brightness, black, unchanged) as rows.

import { tip } from "./tooltip.js";

// No new picture for this long: the one shown is marked as old.
const STALE_MS = 2500;
// Shown as unchanged only from this long on.
const UNCHANGED_NS = 1.5e9;

const TIPS = {
  picture: "The frames' format and size. The picture is a small copy of " +
    "one frame a second.",
  brightness: "Luma of the picture, from 0 % (black) to 100 % (white): " +
    "the average and the brightest place. Black: below 10 % everywhere.",
  unchanged: "How long the picture has stayed exactly the same, compared " +
    "once a second: a repeated frame, as from a frozen source or a stuck " +
    "decoder. Live cameras have noise, so a still scene still changes.",
};

function add(parent, tag, cls, text) {
  const node = document.createElement(tag);
  if (cls)
    node.className = cls;
  if (text !== undefined)
    node.textContent = text;
  parent.append(node);
  return node;
}

function seconds(ns) {
  const s = ns / 1e9;
  return s < 60 ? `${Math.floor(s)} s`
    : `${Math.floor(s / 60)} min ${Math.floor(s % 60)} s`;
}

export class PictureView {
  constructor(card, after) {
    this.box = document.createElement("div");
    this.box.className = "pin-video";
    after.after(this.box);
    this.note = add(this.box, "div", "pin-video-note");
    this.canvas = add(this.box, "canvas", "pin-picture");
    this.rows = add(this.box, "dl", "pin-rows");
    this.cells = {};
    for (const key of ["picture", "brightness", "unchanged"]) {
      tip(add(this.rows, "dt", undefined, key), TIPS[key]);
      this.cells[key] = add(this.rows, "dd");
    }
    this.stale = null;
    this.clear();
  }

  clear() {
    this.box.hidden = true;
    this.format = null;
    clearTimeout(this.stale);
  }

  showReason(text) {
    this.box.hidden = false;
    this.note.hidden = false;
    this.note.textContent = text;
    this.canvas.hidden = this.rows.hidden = true;
    clearTimeout(this.stale);
  }

  // One entry of a "video" message. TRUE when the card changed size.
  apply(entry) {
    const hidden = this.box.hidden;
    if (entry.unavailable) {
      this.format = null;
      this.showReason(entry.unavailable);
      return true;
    }
    this.format = entry;
    this.note.hidden = true;
    this.box.hidden = false;
    this.rows.hidden = false;
    this.cells.picture.textContent = `${entry.width}×${entry.height} ` +
      `${entry.format}, 1 per second`;
    const p = entry.picture;
    if (!p) {
      this.canvas.hidden = true;
      this.cells.brightness.textContent = "—";
      this.cells.unchanged.textContent = "waiting for a frame…";
      return hidden;
    }
    const resized = hidden || this.canvas.hidden ||
      this.canvas.width !== p.width || this.canvas.height !== p.height;
    this.draw(p);
    const pct = (v) => `${Math.round(v * 100)} %`;
    this.cells.brightness.textContent = `avg ${pct(p.brightness_avg)} · ` +
      `max ${pct(p.brightness_max)}${p.black ? " · black" : ""}`;
    this.cells.unchanged.textContent = p.unchanged_for >= UNCHANGED_NS
      ? seconds(p.unchanged_for) : "—";
    this.box.classList.remove("stale");
    clearTimeout(this.stale);
    this.stale = setTimeout(() => {
      this.box.classList.add("stale");
      this.cells.unchanged.textContent = "no new frames";
    }, STALE_MS);
    return resized;
  }

  draw(p) {
    const bytes = atob(p.rgb);
    this.canvas.hidden = false;
    this.canvas.width = p.width;
    this.canvas.height = p.height;
    const ctx = this.canvas.getContext("2d");
    const image = ctx.createImageData(p.width, p.height);
    for (let i = 0, j = 0; i < bytes.length; i += 3, j += 4) {
      image.data[j] = bytes.charCodeAt(i);
      image.data[j + 1] = bytes.charCodeAt(i + 1);
      image.data[j + 2] = bytes.charCodeAt(i + 2);
      image.data[j + 3] = 255;
    }
    ctx.putImageData(image, 0, 0);
  }
}
