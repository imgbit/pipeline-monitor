// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Queue levels and pipeline times from the "levels" message.

import { formatBytes, formatSpan } from "./units.js";

const LIMITS = [["buffers", "buf"], ["bytes", "bytes"], ["time", "time"]];

// How full a queue is: the most blocking limit over all its levels (a
// queue is full when any limit that is set is reached). Can exceed 1:
// multiqueue raises its limits to avoid deadlocks.
export function queueFill(queue) {
  let best = { ratio: 0, limit: null, level: null };
  for (const level of queue.levels) {
    for (const [key, label] of LIMITS) {
      const max = queue.limits[key];
      if (!max)
        continue;
      const ratio = level[key] / max;
      if (ratio > best.ratio || !best.limit)
        best = { ratio, limit: label, level };
    }
  }
  return best;
}

export function fillClass(ratio) {
  if (ratio >= 1)
    return "full";
  return ratio >= 0.75 ? "high" : "ok";
}

export function percent(ratio) {
  return ratio > 1 ? "> 100 %" : `${Math.round(ratio * 100)} %`;
}

function amount(key, value, unit) {
  if (key === "bytes")
    return formatBytes(value);
  if (key === "time")
    return formatSpan(value, unit);
  return String(value);
}

// Rows for the tooltip and the details panel; padName(id) names a
// multiqueue pad.
export function queueRows(queue, unit, padName) {
  const rows = [];
  for (const level of queue.levels) {
    const prefix = level.pad ? `${padName(level.pad)} ` : "";
    for (const [key] of LIMITS) {
      const max = queue.limits[key];
      rows.push([`${prefix}${key}`, `${amount(key, level[key], unit)} / ` +
        (max ? amount(key, max, unit) : "no limit")]);
    }
  }
  rows.push(["overruns", String(queue.overruns)]);
  rows.push(["underruns", String(queue.underruns)]);
  if (queue.buffering !== null)
    rows.push(["buffering", `${queue.buffering} %`]);
  if (queue.leaky && queue.leaky !== "no")
    rows.push(["leaky", `${queue.leaky} (drops instead of blocking)`]);
  return rows;
}

// H:MM:SS.mmm, short enough for the header.
function clockTime(ns) {
  if (ns === null || ns === undefined)
    return "—";
  const s = Math.floor(ns / 1e9);
  const ms = String(Math.floor(ns / 1e6) % 1000).padStart(3, "0");
  return `${Math.floor(s / 3600)}:${String(Math.floor(s / 60) % 60)
    .padStart(2, "0")}:${String(s % 60).padStart(2, "0")}.${ms}`;
}

// The header line for a pipeline, and the details for its hover text.
export function pipelineTimes(p, unit) {
  const provider = p.clock_provider ? p.clock_provider.split("/").pop()
    : null;
  const clock = p.clock ? `clock ${p.clock}${provider ? ` (${provider})` : ""}`
    : "no clock";
  const position = `position ${clockTime(p.position)}` +
    (p.duration !== null ? ` / ${clockTime(p.duration)}` : "");
  const latency = p.latency
    ? `latency ${formatSpan(p.latency.min, unit)}${p.latency.live ? " live"
      : ""}` : "latency —";
  const text = [clock, `running ${clockTime(p.running_time)}`, position,
    latency].join(" · ");
  const title = [
    `clock type: ${p.clock_type || "—"}`,
    `clock provider: ${p.clock_provider || "system"}`,
    `clock time: ${p.clock_time ?? "—"} ns`,
    `base time: ${p.base_time ?? "—"} ns`,
    `running time: ${p.running_time ?? "—"} ns`,
    p.latency ? `latency: min ${formatSpan(p.latency.min, unit)}, max ` +
      `${p.latency.max === null ? "none" : formatSpan(p.latency.max, unit)}`
      : "latency: not answered",
  ].join("\n");
  return { text, title };
}
