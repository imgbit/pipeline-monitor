// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

import { setCounter } from "./dock.js";

const COMMANDS = ["record-start", "record-stop", "snapshot", "record-delete"];

function add(parent, tag, text, cls) {
  const node = document.createElement(tag);
  if (text !== undefined)
    node.textContent = text;
  if (cls)
    node.className = cls;
  parent.append(node);
  return node;
}

function size(bytes) {
  if (bytes < 1024)
    return `${bytes} B`;
  if (bytes < 1024 * 1024)
    return `${(bytes / 1024).toFixed(0)} KB`;
  if (bytes < 1024 * 1024 * 1024)
    return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  return `${(bytes / 1024 / 1024 / 1024).toFixed(1)} GB`;
}

// µs to m:ss (h:mm:ss from an hour).
function duration(us) {
  const s = Math.max(0, Math.round(us / 1e6));
  const two = (n) => String(n).padStart(2, "0");
  return s < 3600 ? `${Math.floor(s / 60)}:${two(s % 60)}`
    : `${Math.floor(s / 3600)}:${two(Math.floor(s / 60) % 60)}:${two(s % 60)}`;
}

// What a file holds, from its first and last lines: {kind, started, last},
// or null if it is not a recording.
async function readSummary(file) {
  const head = (await file.slice(0, 1024 * 1024).text()).split("\n")[0];
  let header;
  try {
    header = JSON.parse(head);
  } catch {
    return null;
  }
  if (header?.format !== "pipeline-monitor-recording" ||
      header.version !== 1 || !Number.isFinite(header.started))
    return null;
  let last = header.started;
  const tail = await file.slice(Math.max(0, file.size - 65536)).text();
  for (const line of tail.split("\n")) {
    try {
      const time = JSON.parse(line)?.time;
      if (Number.isFinite(time))
        last = Math.max(last, time);
    } catch {
      // The first line of the tail is cut; a crash may cut the last.
    }
  }
  return { kind: header.kind, started: header.started, last };
}

// The recordings panel: Record/Stop, Snapshot, Add file… and the list: the
// recordings in the folder of the program's machine, and files added from
// this computer (kept in this tab only, never sent anywhere).
// handlers: send(command), view() -> what this page shows (saved with a
// recording), open(name), openFile(File); viewer: true in a viewer tab,
// where added files open without a server.
export class RecordingsView {
  constructor(nodes, handlers) {
    this.nodes = nodes;
    this.handlers = handlers;
    this.list = [];
    this.files = [];
    // The recording this tab shows: {name} or {file}, or null.
    this.shown = null;
    this.connected = false;
    nodes.record.addEventListener("click", () => {
      this.showError(null);
      this.handlers.send(this.running()?.kind === "recording"
        ? { type: "record-stop" }
        : { type: "record-start", view: this.handlers.view() });
    });
    nodes.addFile.addEventListener("click", () => nodes.fileInput.click());
    nodes.fileInput.addEventListener("change", () => {
      const file = nodes.fileInput.files[0];
      if (file)
        this.addFile(file);
      nodes.fileInput.value = "";
    });
    nodes.snapshot.addEventListener("click", () => {
      this.showError(null);
      this.handlers.send({ type: "snapshot", view: this.handlers.view() });
    });
  }

  // Adds a file from this computer to the top of the list (once).
  async addFile(file) {
    this.showError(null);
    if (!this.filesUsable()) {
      this.showError("Files can be added when the program runs.");
      return;
    }
    const summary = await readSummary(file).catch(() => null);
    if (!summary) {
      this.showError(`${file.name} is not a recording this page can open.`);
      return;
    }
    this.files = this.files.filter((f) => f.file.name !== file.name ||
      f.file.size !== file.size || f.file.lastModified !== file.lastModified);
    this.files.unshift({ file, ...summary });
    this.render();
  }

  setShown(source) {
    this.shown = source ?? null;
    this.render();
  }

  // Marks the row of the recording this tab shows: its Open reads it
  // again (a recording still being written shows its newer part).
  markShown(row, open, shown) {
    if (!shown)
      return;
    row.classList.add("shown");
    add(row.querySelector(".rec-kind"), "span", "shown", "rec-shown");
    open.textContent = "Reload";
    open.title = "Read it again";
  }

  // A live page opens added files in a viewer tab, which needs the
  // program's server; a viewer shows them itself.
  filesUsable() {
    return this.connected || Boolean(this.handlers.viewer);
  }

  running() {
    return this.list.find((r) => r.running);
  }

  setConnected(connected) {
    this.connected = connected;
    this.render();
  }

  // A "recordings" message.
  apply(message) {
    this.list = message.recordings;
    this.folder = message.folder;
    this.fileMax = message.file_max;
    this.totalMax = message.total_max;
    // Why the last recording ended early (e.g. the disk is full).
    if (message.failure && message.failure !== this.failure)
      this.showError(message.failure);
    this.failure = message.failure;
    this.render();
  }

  // An "error" answer: shown if it is about recordings. True if it was.
  applyError(message) {
    if (!COMMANDS.includes(message.command))
      return false;
    this.showError(message.reason);
    return true;
  }

  showError(text) {
    this.nodes.error.textContent = text ?? "";
    this.nodes.error.hidden = !text;
  }

  // A file added from this computer: Open, and Remove (from the list only).
  fileRow(fragment, f) {
    const row = add(fragment, "div", undefined, "rec-row added");
    row.dataset.file = f.file.name;
    row.title = `${f.file.name}, added from this computer (not uploaded)`;
    const kind = add(row, "span", undefined, "rec-kind");
    add(kind, "span", "file", "rec-file");
    kind.append(` ${f.kind ?? "?"}`);
    add(row, "span", new Date(f.started / 1000).toLocaleString(), "rec-time");
    add(row, "span", f.kind === "snapshot" ? "–"
      : duration(f.last - f.started), "rec-duration");
    add(row, "span", size(f.file.size), "rec-size");
    const actions = add(row, "span", undefined, "rec-actions");
    const open = add(actions, "button", "Open", "rec-open");
    open.disabled = !this.filesUsable();
    open.addEventListener("click", () => this.handlers.openFile(f.file));
    this.markShown(row, open, this.shown?.file === f.file);
    const remove = add(actions, "button", "Remove", "rec-remove");
    remove.disabled = !this.filesUsable();
    remove.title = "Remove from the list (the file stays on your computer)";
    remove.addEventListener("click", () => {
      this.files = this.files.filter((x) => x !== f);
      this.render();
    });
  }

  render() {
    const { counter, record, snapshot, rows, note } = this.nodes;
    const running = this.running();
    const recording = running?.kind === "recording";
    setCounter(counter, "Recordings", recording
      ? `● ${duration(running.last - running.started)} · ${size(running.size)}`
      : running ? "saving…" : null, "recording");
    record.textContent = recording ? "■ Stop" : "● Record";
    // A snapshot takes a moment; nothing else starts meanwhile.
    record.disabled = !this.connected || (Boolean(running) && !recording);
    snapshot.disabled = !this.connected || Boolean(running);
    record.title = this.connected ? "" : "Needs the program to be running";

    const fragment = document.createDocumentFragment();
    this.nodes.addFile.disabled = !this.filesUsable();
    if (!this.connected && (this.list.length || this.files.length))
      add(fragment, "div", this.handlers.viewer
        ? "The pipeline-monitor command is not running: its recordings can " +
          "be opened, downloaded and deleted again when it runs. Added " +
          "files open now."
        : "The program is not running: the list works again when it runs. " +
          "To open recordings without it, use the pipeline-monitor " +
          "command.", "rec-offline");
    if (!this.list.length && !this.files.length)
      add(fragment, "div", "No recordings yet.", "log-empty");
    for (const f of this.files)
      this.fileRow(fragment, f);
    for (const r of this.list) {
      const row = add(fragment, "div", undefined,
        `rec-row${r.running ? " running" : ""}`);
      row.dataset.name = r.name;
      row.title = r.name;
      add(row, "span", r.kind ?? "?", "rec-kind");
      add(row, "span", r.started
        ? new Date(r.started / 1000).toLocaleString() : "?", "rec-time");
      add(row, "span", r.kind === "snapshot" ? "–"
        : duration(r.last - r.started), "rec-duration");
      add(row, "span", size(r.size), "rec-size");
      const actions = add(row, "span", undefined, "rec-actions");
      if (r.running) {
        add(actions, "span", r.kind === "snapshot" ? "saving…" : "recording…",
          "muted");
        continue;
      }
      // The files are on the program's machine, served by the program.
      const open = add(actions, "button", "Open", "rec-open");
      open.disabled = !this.connected;
      open.addEventListener("click", () => this.handlers.open(r.name));
      this.markShown(row, open, this.shown?.name === r.name);
      const download = add(actions, "button", "Download", "rec-download");
      download.disabled = !this.connected;
      download.addEventListener("click", () => {
        const link = document.createElement("a");
        link.href = `api/recordings/${encodeURIComponent(r.name)}`;
        link.download = r.name;
        link.click();
      });
      // A second click within 3 s deletes; kept here, as the list is redrawn
      // every second while recording.
      const armed = this.armed?.name === r.name &&
        this.armed.until > Date.now();
      const remove = add(actions, "button", armed ? "Delete?" : "Delete",
        "rec-delete");
      remove.disabled = !this.connected;
      remove.addEventListener("click", () => {
        if (this.armed?.name === r.name && this.armed.until > Date.now()) {
          this.armed = null;
          this.handlers.send({ type: "record-delete", name: r.name });
          return;
        }
        this.armed = { name: r.name, until: Date.now() + 3000 };
        this.render();
        setTimeout(() => this.render(), 3100);
      });
    }
    rows.replaceChildren(fragment);
    note.textContent = this.folder ? `Saved in ${this.folder} on the ` +
      `program's machine. Up to ${size(this.fileMax)} per recording and ` +
      `${size(this.totalMax)} in total; the oldest are removed first. ` +
      "Passwords in addresses and secret properties are hidden; element " +
      "names, caps, properties and messages are in the file." : "";
  }
}
