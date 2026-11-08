// SPDX-FileCopyrightText: 2026 Łukasz Huńczak
// SPDX-License-Identifier: LGPL-2.1-or-later

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

// The recordings panel: Record/Stop, Snapshot and the list in the folder of
// the program's machine. handlers: send(command), view() -> what this page
// shows (saved with a recording), open(name), openFile(File).
export class RecordingsView {
  constructor(nodes, handlers) {
    this.nodes = nodes;
    this.handlers = handlers;
    this.list = [];
    this.connected = false;
    nodes.counter.addEventListener("click",
      () => this.setOpen(nodes.panel.hidden));
    nodes.close.addEventListener("click", () => this.setOpen(false));
    nodes.record.addEventListener("click", () => {
      this.showError(null);
      this.handlers.send(this.running()?.kind === "recording"
        ? { type: "record-stop" }
        : { type: "record-start", view: this.handlers.view() });
    });
    nodes.openFile.addEventListener("click", () => nodes.fileInput.click());
    nodes.fileInput.addEventListener("change", () => {
      const file = nodes.fileInput.files[0];
      if (file)
        this.handlers.openFile(file);
      nodes.fileInput.value = "";
    });
    nodes.snapshot.addEventListener("click", () => {
      this.showError(null);
      this.handlers.send({ type: "snapshot", view: this.handlers.view() });
    });
  }

  setOpen(open) {
    this.nodes.panel.hidden = !open;
    this.nodes.counter.setAttribute("aria-expanded", String(open));
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

  render() {
    const { counter, record, snapshot, rows, note } = this.nodes;
    const running = this.running();
    const recording = running?.kind === "recording";
    counter.textContent = recording
      ? `● ${duration(running.last - running.started)} · ${size(running.size)}`
      : running ? "Saving snapshot…" : "Recordings";
    counter.classList.toggle("recording", recording);
    record.textContent = recording ? "■ Stop" : "● Record";
    // A snapshot takes a moment; nothing else starts meanwhile.
    record.disabled = !this.connected || (Boolean(running) && !recording);
    snapshot.disabled = !this.connected || Boolean(running);
    record.title = this.connected ? "" : "Needs the program to be running";

    const fragment = document.createDocumentFragment();
    if (!this.list.length)
      add(fragment, "div", "No recordings yet.", "log-empty");
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
      const open = add(actions, "button", "Open", "rec-open");
      open.addEventListener("click", () => this.handlers.open(r.name));
      const link = add(actions, "a", "Download", "rec-download");
      link.href = `api/recordings/${encodeURIComponent(r.name)}`;
      link.download = r.name;
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
