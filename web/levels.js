// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Queue levels and pipeline times from the "levels" message.

import { formatBytes, formatSpan } from "./units.js";

const LIMITS = [["buffers", "buffers"], ["bytes", "bytes"], ["time", "time"]];

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

// The colour and the label use the same rounded percentage: a queue a few
// ns over or under its limit is shown as 100 % and full.
export function fillClass(ratio) {
  const p = Math.round(ratio * 100);
  if (p >= 100)
    return "full";
  return p >= 75 ? "high" : "ok";
}

export function percent(ratio) {
  return `${Math.round(ratio * 100)} %`;
}

function amount(key, value, unit) {
  if (key === "bytes")
    return formatBytes(value);
  if (key === "time")
    return formatSpan(value, unit);
  return String(value);
}

// What appsrc's and appsink's queue does at a limit (from their settings).
const AT_LIMIT = {
  blocks: { appsrc: "the program's push waits for room (block)",
    appsink: "waits for the program to pull, holding up the pipeline" },
  drops: { appsrc: "drops buffers (leaky-type)",
    appsink: "drops buffers (drop / leaky-type)" },
  signals: { appsrc: "only signals enough-data, and keeps queueing" },
};

// appsrc's and appsink's own queue: what this GStreamer can tell.
function appRows(queue, unit) {
  const rows = [];
  if (!queue.levels.length)
    rows.push(["level", "not available before GStreamer 1.28"]);
  for (const level of queue.levels) {
    for (const [key] of LIMITS) {
      const max = queue.limits[key];
      const value = level[key];
      rows.push([key, `${value === null ? "—" : amount(key, value, unit)} / ` +
        (max === null ? "no such limit" : max ? amount(key, max, unit)
          : "no limit")]);
    }
  }
  rows.push(["at a limit", AT_LIMIT[queue.at_limit]?.[queue.kind] ??
    queue.at_limit]);
  const { in: into, out, dropped } = queue.counts ?? {};
  if (into === null || into === undefined)
    rows.push(["in / out / dropped", "not available before GStreamer 1.28"]);
  else
    rows.push(["in / out / dropped", `${into} / ${out} / ${dropped}`]);
  return rows;
}

// appsink -> appsrc links, found from the data: one seen in the last 5 s
// is current; an older one is shown only when no current one is.
export const LINK_FRESH_US = 5e6;

// An appsrc's links: {current, old}; current ones by name (they would
// swap places as each is seen again), old ones the newest first.
export function appsrcLinks(queue, now) {
  const all = queue?.fed_by ?? [];
  return {
    current: all.filter((f) => now - f.last <= LINK_FRESH_US)
      .sort((a, b) => (a.path ?? "").localeCompare(b.path ?? "")),
    old: all.filter((f) => now - f.last > LINK_FRESH_US)
      .sort((a, b) => b.last - a.last),
  };
}

// The appsrcs an appsink's buffers currently leave: [{element, path}].
export function appsinkLinks(sinkId, queues, now) {
  const found = [];
  for (const q of queues.values())
    if (q.kind === "appsrc" && appsrcLinks(q, now).current
      .some((f) => f.element === sinkId))
      found.push({ element: q.element, path: q.path });
  return found;
}

function segmentCell(s, key) {
  if (s === undefined)
    return "—";
  if (s === null)
    return "none";
  if (key === "format" || key === "rate")
    return String(s[key]);
  return s.format === "time" ? clockTime(s[key]) : String(s[key]);
}

// [label, into appsink, out of appsrc, differs]: the same buffer at both
// ends (link), or the appsink's last buffer and segment against the
// appsrc's segment (chosen: not the same buffer, PTS not compared).
// into.pipeline, out.pipeline: the two pipelines' entries of the "levels"
// message, for their clocks and base times. A fifth item, when there, is
// what the row means.
export function linkRows(into, out, same) {
  const rows = [["PTS", clockTime(into.pts), same ? clockTime(out.pts) : "—",
    same && into.pts !== out.pts, same
      ? "The same buffer's timestamp (its PTS) as it went into the appsink " +
        "and as it left the appsrc, read by the tracer at each push. The " +
        "program or appsrc (do-timestamp) may change it on the way."
      : "The appsink's last buffer's timestamp; appsrc's buffers are not " +
        "known to be the same, so theirs is not shown."]];
  for (const key of ["format", "start", "base", "rate"]) {
    const a = segmentCell(into.segment, key);
    const b = segmentCell(out.segment, key);
    rows.push([`segment ${key}`, a, b, a !== b]);
  }
  // The buffer's running time at each end, from its PTS and segment.
  const runA = runningTime(into.pts, into.segment);
  const runB = same ? runningTime(out.pts, out.segment) : null;
  rows.push(["running time", clockTime(runA), clockTime(runB), false]);
  const p = into.pipeline;
  const q = out.pipeline;
  if (p || q) {
    rows.push(["pipeline", p?.path ?? "—", q?.path ?? "—", false]);
    const clockA = p?.clock ?? "none";
    const clockB = q?.clock ?? "none";
    const sameClock = Boolean(p && q) && clockA === clockB;
    rows.push(["clock", clockA, clockB, Boolean(p && q) && !sameClock]);
    // Only a problem when the timestamps were carried over unchanged.
    const carried = same && into.pts === out.pts;
    // From a millisecond: below what is shown is no difference to see.
    rows.push(["base time", clockTime(p?.base_time), clockTime(q?.base_time),
      carried && p?.base_time != null && q?.base_time != null &&
      Math.abs(p.base_time - q.base_time) >= 1e6]);
    // When each pipeline plays it on its clock (base time + running time),
    // both counted from the appsink pipeline's start: small numbers to
    // compare, rather than the clock's own (time since the computer
    // started, for the system clock).
    const playA = runA === null || p?.base_time == null ? null
      : p.base_time + runA;
    const playB = runB === null || q?.base_time == null ? null
      : q.base_time + runB;
    const from = (t) => t === null ? "—" : t < p.base_time
      ? `−${clockTime(p.base_time - t)}` : clockTime(t - p.base_time);
    let a = "—";
    let b = "—";
    if (playA !== null)
      a = from(playA);
    if (playB !== null && sameClock)
      b = from(playB) + (playA !== null
        ? ` (${signedSpan(playB - playA)})` : "");
    else if (playB !== null)
      b = "different clocks: not comparable";
    rows.push(["plays at", a, b, false,
      `When each pipeline plays this buffer, counted from ${p?.path ?? "the " +
      "appsink's pipeline"}'s start: base time + the buffer's running ` +
      "time (its PTS within its segment), on the clock. The same buffer " +
      "meant for the same moment at both ends shows the same time; the " +
      "difference after it is how much later (or earlier) it plays after " +
      "the program and appsrc. A lost segment, timestamps carried between " +
      "pipelines with different base times, or restamping all show here."]);
  }
  return rows;
}

// (PTS − segment start) / rate + segment base, for a time segment played
// forwards; null when it cannot be told.
function runningTime(pts, segment) {
  if (pts === null || pts === undefined || !segment ||
      segment.format !== "time" || !(segment.rate > 0) || pts < segment.start)
    return null;
  return Math.round((pts - segment.start) / segment.rate) + segment.base;
}

function signedSpan(ns) {
  return `${ns < 0 ? "−" : "+"}${(Math.abs(ns) / 1e9).toFixed(3)} s`;
}

// appsrc's segment sent or appsink's segment received: rows for the
// details panel.
export function appStreamRows(queue) {
  const s = queue.segment;
  const at = (v) => s.format === "time" ? clockTime(v) : String(v ?? "—");
  return [[queue.kind === "appsrc" ? "segment sent" : "segment received",
    s ? `${s.format}, from ${at(s.start)}, base ${at(s.base)}` +
      (s.rate !== 1 ? `, rate ${s.rate}` : "") : "none yet"]];
}

// Rows for the tooltip and the details panel; padName(id) names a
// multiqueue pad.
export function queueRows(queue, unit, padName) {
  if (queue.kind === "appsrc" || queue.kind === "appsink")
    return appRows(queue, unit);
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
  // The clock's name only in the tooltip: the header has little room.
  const text = [`running ${clockTime(p.running_time)}`, position, latency]
    .join(" · ");
  const title = [
    clock,
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
