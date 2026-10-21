// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

// Short summaries of GStreamer caps strings, e.g.
// "video/x-raw, format=(string)I420, width=(int)1280, height=(int)720,
// framerate=(fraction)30/1" becomes "video/x-raw I420 1280×720 30/1".

// Splits at top-level separators, ignoring those inside quotes or brackets
// (lists "{ a, b }", arrays "< a, b >", ranges "[ 1, 10 ]").
function splitTop(text, sep) {
  const parts = [];
  let depth = 0;
  let quoted = false;
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === "\\")
        i++;
      else if (c === '"')
        quoted = false;
    } else if (c === '"') {
      quoted = true;
    } else if ("{<[(".includes(c)) {
      depth++;
    } else if ("}>])".includes(c)) {
      depth--;
    } else if (c === sep && depth === 0) {
      parts.push(text.slice(start, i));
      start = i + 1;
    }
  }
  parts.push(text.slice(start));
  return parts.map((p) => p.trim()).filter(Boolean);
}

// Parses the first structure of a caps string into its media type, caps
// features and fields in order, with the type annotations removed, e.g.
// "width=(int)320" becomes ["width", "320"].
export function parseCaps(text) {
  // Current caps are fixed and have one structure; use the first one.
  const [first = ""] = splitTop(text || "", ";");
  const [head = "", ...rest] = splitTop(first, ",");
  const m = head.match(/^([^(]+)(?:\((.*)\))?$/);
  const fields = rest.map((f) => {
    const eq = f.indexOf("=");
    if (eq < 0)
      return [f, ""];
    let value = f.slice(eq + 1).trim().replace(/^\([\w-]+\)/, "");
    if (value.startsWith('"') && value.endsWith('"'))
      value = value.slice(1, -1).replace(/\\(.)/g, "$1");
    return [f.slice(0, eq).trim(), value];
  });
  return {
    media: m ? m[1].trim() : head,
    features: m && m[2] ? m[2] : null,
    fields,
  };
}

export function summarizeCaps(text) {
  if (!text)
    return "";
  const { media, features, fields: list } = parseCaps(text);
  const fields = Object.fromEntries(list);
  // System memory is the default and not worth showing.
  const memory = features && features !== "memory:SystemMemory"
    ? `(${features.replace(/memory:/g, "")})` : "";
  const parts = [media + memory];

  if (media === "video/x-raw" || media === "image/x-raw") {
    parts.push(fields.format);
    if (fields.width && fields.height)
      parts.push(`${fields.width}×${fields.height}`);
    if (fields.framerate && fields.framerate !== "0/1")
      parts.push(fields.framerate);
  } else if (media === "audio/x-raw") {
    parts.push(fields.format);
    if (fields.rate)
      parts.push(`${fields.rate} Hz`);
    if (fields.channels)
      parts.push(`${fields.channels} ch`);
  } else {
    parts.push(fields["stream-format"], fields.alignment);
    if (media.startsWith("video/") && fields.width && fields.height)
      parts.push(`${fields.width}×${fields.height}`);
    if (media.startsWith("audio/") && fields.rate)
      parts.push(`${fields.rate} Hz`);
  }
  return parts.filter(Boolean).join(" ");
}

// A link's caps are what its sink pad accepted, else what its source sent.
// info.ports maps ELK port ids to pads (see layout.js).
export function linkCaps(info, src, sink) {
  return info.ports.get(sink)?.pad.caps || info.ports.get(src)?.pad.caps || "";
}
