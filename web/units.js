// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Formatting of the values the tracer sends: times in ns, sizes in bytes.

export const TIME_UNITS = ["auto", "ns", "ms", "s"];

const DASH = "—";

// A timestamp (PTS, running time, ...). "auto" is GStreamer's own
// H:MM:SS.nnnnnnnnn, as in debug logs.
export function formatStamp(ns, unit) {
  if (ns === null || ns === undefined)
    return DASH;
  if (unit !== "auto")
    return formatSpan(ns, unit);
  const neg = ns < 0 ? "-" : "";
  ns = Math.abs(ns);
  const s = Math.floor(ns / 1e9);
  const frac = String(ns % 1e9).padStart(9, "0");
  const h = Math.floor(s / 3600);
  const m = String(Math.floor(s / 60) % 60).padStart(2, "0");
  const sec = String(s % 60).padStart(2, "0");
  return `${neg}${h}:${m}:${sec}.${frac}`;
}

// A duration or difference (gap, delay). "auto" picks the unit by size.
export function formatSpan(ns, unit) {
  if (ns === null || ns === undefined)
    return DASH;
  const abs = Math.abs(ns);
  if (unit === "auto") {
    if (abs < 1e3)
      return `${ns} ns`;
    if (abs < 1e6)
      return `${(ns / 1e3).toFixed(1)} µs`;
    if (abs < 1e9)
      return `${(ns / 1e6).toFixed(1)} ms`;
    return `${(ns / 1e9).toFixed(3)} s`;
  }
  if (unit === "ms")
    return `${(ns / 1e6).toFixed(3)} ms`;
  if (unit === "s")
    return `${(ns / 1e9).toFixed(6)} s`;
  return `${ns} ns`;
}

export function formatBytes(n) {
  if (n === null || n === undefined)
    return DASH;
  if (n < 1024)
    return `${Math.round(n)} B`;
  if (n < 1024 * 1024)
    return `${(n / 1024).toFixed(1)} KiB`;
  if (n < 1024 * 1024 * 1024)
    return `${(n / 1024 / 1024).toFixed(1)} MiB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GiB`;
}

// The raw value, shown when hovering a formatted one.
export function rawNs(ns) {
  return ns === null || ns === undefined ? "unknown" : `${ns} ns`;
}
