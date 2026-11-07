// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Latency per element from the "latency" message.

import { formatSpan } from "./units.js";

// Most outputs unmatched: the element makes new timestamps.
export function cannotMeasure(m) {
  return m.unmatched > 3 && m.unmatched > m.matched;
}

// Sources have no input and sinks no output: nothing in between.
export function measurable(entry) {
  return entry.inputs && entry.outputs;
}

// The label under a measured element in the graph, or null.
export function latencyLabel(entry, unit) {
  const m = entry.measured;
  if (!measurable(entry))
    return null;
  if (cannotMeasure(m))
    return { text: "latency ?", cls: "unknown",
      title: "Cannot measure: the element makes new timestamps" };
  if (m.avg === null)
    return null;
  const cls = m.avg >= 100e6 ? "high" : m.avg >= 10e6 ? "some" : "low";
  return {
    text: formatSpan(m.avg, unit),
    cls,
    title: `latency min ${formatSpan(m.min, unit)}, avg ` +
      `${formatSpan(m.avg, unit)}, max ${formatSpan(m.max, unit)} ` +
      "(last second)",
  };
}

// Rows for the details panel; pads: the element's pads, to explain why a
// source or a sink is not measured before any data came.
// reported: what the element reports, from its properties answer, when
// it is not being measured. pending: measuring is on, but no program is
// connected. A row with a null label is a note.
export function latencyRows(entry, unit, pads, reported, pending) {
  const rows = [];
  const m = entry?.measured;
  const inputs = entry ? entry.inputs : pads.some((p) => p.direction === "sink");
  const outputs = entry ? entry.outputs : pads.some((p) => p.direction === "src");
  if (!inputs)
    rows.push([null, "A source has no input, so there is nothing to " +
      "measure here."]);
  else if (!outputs)
    rows.push([null, "A sink has no output, so there is nothing to measure " +
      "here. Click its sink pad in the graph: the pad card's \"delay\" " +
      "shows how late buffers reach it."]);
  else if (pending)
    rows.push(["measured", "starts when the program runs"]);
  else if (!m)
    rows.push(["measured", "—"]);
  else if (cannotMeasure(m))
    rows.push(["measured", "cannot measure: timestamps change"]);
  else if (m.avg === null)
    rows.push(["measured", "waiting for data…"]);
  else
    rows.push(["measured", `${formatSpan(m.avg, unit)} avg`],
      ["min / max", `${formatSpan(m.min, unit)} / ${formatSpan(m.max, unit)}`]);
  // A sink has no output to report a latency for.
  if (!outputs)
    return rows;
  const r = entry?.reported ?? reported;
  rows.push(["reported", r ? `${formatSpan(r.min, unit)}${r.live ? " (live)"
    : ""}` : "—"]);
  return rows;
}
