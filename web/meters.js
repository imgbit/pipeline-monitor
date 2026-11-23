// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Audio levels in a pinned pad's card: a bar per channel, and the facts
// (format, peak, RMS, clipping, silence) as rows.

import { tip } from "./tooltip.js";

const FLOOR = -60;
// Colour zones, as in most digital meters: above the EBU alignment level
// (-18 dBFS) yellow, close to full scale red.
const YELLOW = -18;
const RED = -6;
const HOLD_MS = 1500;
const FALL_DB_PER_S = 20;
// Bars rise at once and fall like a peak meter (IEC 60268-10: 20 dB in
// 1.7 s), so short sounds stay readable. The rows show the true values.
const BAR_FALL_DB_PER_S = 20 / 1.7;
// Rows change at most this often, to stay readable; bars every message.
const ROWS_MS = 250;
// No new levels for this long: the bars go empty.
const STALE_MS = 400;

const TIPS = {
  peak: "The highest sample over the last quarter second, in dBFS (0 = " +
    "full scale). The real peak between samples can be a little higher.",
  rms: "The average level (root mean square) over the last quarter " +
    "second, in dBFS. Plain RMS: a full-scale sine reads -3.0 dBFS.",
  clipped: "Samples at full scale since the pad was pinned: there, the " +
    "signal was cut off.",
  silent: "Channels below -60 dBFS for 2 s or more. Digital silence: every " +
    "sample exactly 0, so nothing is producing sound.",
  audio: "The format of the pad's caps.",
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

function percent(db) {
  if (db === null || db <= FLOOR)
    return 0;
  return Math.min(100, (db - FLOOR) / -FLOOR * 100);
}

function db(value) {
  return value === null ? "−∞" : value.toFixed(1).replace("-", "−");
}

function seconds(ns) {
  const s = ns / 1e9;
  return s < 60 ? `${Math.floor(s)} s`
    : `${Math.floor(s / 60)} min ${Math.floor(s % 60)} s`;
}

export class AudioMeter {
  constructor(card, after) {
    this.box = document.createElement("div");
    this.box.className = "pin-audio";
    after.after(this.box);
    this.note = add(this.box, "div", "pin-audio-note");
    this.bars = add(this.box, "div", "meter-bars");
    this.scale = add(this.box, "div", "meter-scale");
    const ticks = add(this.scale, "span", "meter-ticks");
    for (const d of [-60, -40, -20, -6, 0])
      add(ticks, "span", undefined, String(d).replace("-", "−"))
        .style.left = `${percent(d)}%`;
    this.rows = add(this.box, "dl", "pin-rows");
    this.cells = {};
    for (const key of ["audio", "peak", "rms", "clipped", "silent"]) {
      tip(add(this.rows, "dt", undefined, key === "rms" ? "RMS" : key),
        TIPS[key]);
      this.cells[key] = add(this.rows, "dd");
    }
    this.mini = null;
    this.channels = [];
    this.rowsAt = 0;
    this.stale = null;
    this.clear();
  }

  // Nothing known (yet): the block is hidden.
  clear() {
    this.box.hidden = true;
    this.info = null;
    clearTimeout(this.stale);
  }

  // Small bars for the folded card's head, after the summary.
  attachMini(after) {
    this.mini = document.createElement("span");
    this.mini.className = "meter-mini";
    this.mini.hidden = true;
    after.after(this.mini);
  }

  showReason(text) {
    this.box.hidden = false;
    this.note.hidden = false;
    this.note.textContent = text;
    this.bars.hidden = this.scale.hidden = this.rows.hidden = true;
    if (this.mini)
      this.mini.hidden = true;
  }

  // One entry of an "audio" message. TRUE when the card changed size.
  apply(entry, folded) {
    const hidden = this.box.hidden;
    if (entry.unavailable) {
      this.info = null;
      this.showReason(entry.unavailable);
      return true;
    }
    let resized = false;
    if (entry.format)
      resized = this.setFormat(entry);
    if (!this.info || !entry.levels)
      return resized;
    this.box.hidden = false;
    this.showLevels(entry.levels);
    if (this.mini)
      this.mini.hidden = !folded;
    clearTimeout(this.stale);
    this.stale = setTimeout(() => this.showLevels(null), STALE_MS);
    return resized || hidden;
  }

  setFormat(entry) {
    const same = this.info && this.info.positions.join() ===
      entry.positions.join() && this.info.shown === entry.shown;
    this.info = entry;
    this.note.hidden = true;
    this.bars.hidden = this.scale.hidden = this.rows.hidden = false;
    const more = entry.channels > entry.shown
      ? ` (${entry.positions.slice(0, entry.shown).join(" ")} shown)` : "";
    const layout = entry.interleaved || entry.channels === 1 ? ""
      : ", planar";
    this.cells.audio.textContent = `${entry.format}, ${entry.rate / 1000} ` +
      `kHz, ${entry.channels} channel${entry.channels === 1 ? "" : "s"}` +
      `${layout}${more}`;
    if (same)
      return false;
    this.bars.replaceChildren();
    this.channels = [];
    if (this.mini)
      this.mini.replaceChildren();
    for (let i = 0; i < entry.shown; i++) {
      const row = add(this.bars, "div", "meter");
      add(row, "span", "meter-label", entry.positions[i]);
      const bar = add(row, "span", "meter-bar");
      const off = add(bar, "span", "meter-off");
      const peak = add(bar, "span", "meter-peak");
      const hold = add(bar, "span", "meter-hold");
      const mini = this.mini ? add(this.mini, "span", "meter-bar") : null;
      const miniOff = mini ? add(mini, "span", "meter-off") : null;
      this.channels.push({ row, bar, off, peak, hold, miniOff,
        label: entry.positions[i], held: null, heldAt: 0, clipAt: 0,
        rowPeak: null, rowPower: 0, rowWindows: 0 });
    }
    return true;
  }

  showLevels(levels) {
    const now = performance.now();
    this.channels.forEach((ch, i) => {
      const l = levels?.[i];
      // The newest window of the entry.
      const peak = l ? l.peak[l.peak.length - 1] : null;
      const rms = l ? l.rms[l.rms.length - 1] : null;
      if (peak !== null && (ch.held === null || peak >= ch.held)) {
        ch.held = peak;
        ch.heldAt = now;
      } else if (ch.held !== null && now - ch.heldAt > HOLD_MS) {
        ch.held -= FALL_DB_PER_S * (now - ch.heldAt - HOLD_MS) / 1000;
        ch.heldAt = now - HOLD_MS;
        if (peak !== null && peak > ch.held)
          ch.held = peak;
        if (ch.held <= FLOOR)
          ch.held = null;
      }
      if (l && l.clipped > 0)
        ch.clipAt = now;
      const fallen = (shown, value) => {
        const lower = shown === undefined || shown === null ? null
          : shown - BAR_FALL_DB_PER_S * (now - ch.shownAt) / 1000;
        if (value === null)
          return lower !== null && lower > FLOOR ? lower : null;
        return lower !== null && lower > value ? lower : value;
      };
      const barPeak = levels ? fallen(ch.barPeak, peak) : null;
      const barRms = levels ? Math.min(fallen(ch.barRms, rms) ?? FLOOR,
        barPeak ?? FLOOR) : null;
      ch.barPeak = barPeak;
      ch.barRms = barRms;
      ch.shownAt = now;
      // Solid to the RMS, paler from there to the peak.
      ch.off.style.left = `${percent(barPeak)}%`;
      ch.peak.style.left = `${percent(barRms)}%`;
      ch.peak.style.width = `${percent(barPeak) - percent(barRms)}%`;
      ch.hold.hidden = ch.held === null;
      ch.hold.style.left = `calc(${percent(ch.held)}% - 1px)`;
      ch.bar.classList.toggle("clipping", now - ch.clipAt < 1000);
      if (ch.miniOff)
        ch.miniOff.style.left = `${percent(barPeak)}%`;
      ch.level = l;
      // For the rows: the highest peak and the mean power since they were
      // last filled.
      if (l) {
        for (const v of l.peak)
          if (v !== null && (ch.rowPeak === null || v > ch.rowPeak))
            ch.rowPeak = v;
        for (const v of l.rms) {
          ch.rowPower += v === null ? 0 : 10 ** (v / 10);
          ch.rowWindows++;
        }
      }
    });
    if (now - this.rowsAt >= ROWS_MS || !levels) {
      this.rowsAt = now;
      this.fillRows(levels);
      for (const ch of this.channels) {
        ch.rowPeak = null;
        ch.rowPower = 0;
        ch.rowWindows = 0;
      }
    }
  }

  fillRows(levels) {
    const named = (f) => this.channels.map((ch, i) =>
      `${ch.label} ${f(levels[i], ch)}`).join(" · ");
    if (!levels) {
      for (const key of ["peak", "rms"])
        this.cells[key].textContent = "no samples";
      return;
    }
    this.cells.peak.textContent = `${named((l, ch) => db(ch.rowPeak))} dBFS`;
    this.cells.rms.textContent = `${named((l, ch) => db(ch.rowWindows &&
      ch.rowPower > 0 ? 10 * Math.log10(ch.rowPower / ch.rowWindows) : null))}` +
      " dBFS";
    const clipped = levels.reduce((n, l) => n + l.clipped_total, 0);
    this.cells.clipped.textContent = clipped === 0 ? "0"
      : named((l) => l.clipped_total);
    this.cells.clipped.classList.toggle("bad", clipped > 0);
    const silent = this.channels.map((ch, i) => [ch.label, levels[i]])
      .filter(([, l]) => l.silent_for !== null);
    this.cells.silent.textContent = silent.length === 0 ? "—"
      : silent.map(([label, l]) => `${label} ${seconds(l.silent_for)}` +
        (l.digital_silence ? " (digital silence)" : "")).join(" · ");
  }
}
