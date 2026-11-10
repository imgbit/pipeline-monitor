# pipeline-monitor protocol

This is how the pipeline-monitor plugin and its page talk. Scripts, tests
and other tools can use it too: the plugin serves the state of one
program's pipelines over HTTP and a WebSocket.

Protocol version: **1** (pipeline-monitor 0.9.0 and later).

## Overview

The plugin listens on `127.0.0.1:<port>` (8765 by default, see the README
for the `port` option):

- `GET` endpoints under `/api/` answer with the current state, one request
  at a time;
- the WebSocket at `/ws` sends updates as they happen and takes commands.

Both carry the same messages: an HTTP answer and the WebSocket message of
the same type have the same shape.

Access is from this machine only. Requests need a `Host` header of
`127.0.0.1:<port>` or `localhost:<port>`; a browser must also send an
`Origin` of that address. Only `GET` and `HEAD` are accepted. Other
requests get `403` or `405`. There is no authentication.

## Versioning

- Every message has `"protocol": 1`.
- New message types, fields, commands and endpoints can be added without a
  new version. Readers ignore what they do not know.
- Removing or renaming something, or changing a field's meaning or unit,
  raises the version.

## Conventions

### Envelope

Every message starts with the same three fields, followed by its own:

```json
{"type":"levels","protocol":1,"time":1794073709530508, ...}
```

| Field | Meaning |
|---|---|
| `type` | which message it is (see Messages) |
| `protocol` | the protocol version, 1 |
| `time` | when the plugin made the message, µs since 1970 |

### Times

| What | Unit |
|---|---|
| Moments on the wall clock: `time`, `first`, `last` | µs since 1970 |
| Every other time: durations, latencies, PTS, DTS, running time, clock and base time, segment values, gaps, ages | ns |
| Rates, named `…_per_s` | per second |

Times in ns go above 2^53 after about 104 days (for example the clock time
of a machine that has been up that long); in JavaScript they then lose
their last few ns.

### Missing values

`null` means unknown or none (no buffer yet, no maximum). A field that does
not apply can be left out; read a missing field as `null`.

### Ids and paths

- Elements and pads have an `id`: a number unique within one run of the
  program. A new run gives new ids.
- They also have a `path`: the names from the top-level pipeline down,
  `pipeline0/decodebin0/queue0`; for a pad the element's path and the pad
  name, `pipeline0/decodebin0/queue0:src`. A path stays the same across
  runs as long as the names do.
- Every reference carries both: `"element"` with `"path"`, `"pad"` with
  `"pad_path"`, `"from"` with `"from_path"`, `"to"` with `"to_path"`. Use the
  id within a run, the path to find the same thing again in another run.
  (Entries for something that is gone may lack the path.)

### Names of values

GStreamer's own names are written as GStreamer writes them: states
(`PLAYING`), flow results (`not-linked`), formats (`time`), caps. Values
that are pipeline-monitor's own are lowercase (`warning`, `queue2`).

## Connecting

1. Open `ws://127.0.0.1:<port>/ws`.
2. The plugin sends `hello`, then `graph`, the whole `log`, and `findings`
   if there are any. `levels` follow within 250 ms.
3. From then on it sends updates, and answers commands.
4. When the program ends normally it sends `bye` and closes. A connection
   that just drops means the program stopped some other way.

What a connection asked for (watched pads and properties, measurements)
belongs to that connection only. A new connection starts with nothing.

## Messages

| type | Sent | To | Over HTTP |
|---|---|---|---|
| `hello` | first, on every connection | that connection | `GET /api/hello` |
| `graph` | when the graph changes, at most every 100 ms | all | `GET /api/graph` |
| `log` | new or repeated errors and warnings, at most every 100 ms | all | `GET /api/log` (whole log) |
| `findings` | when the list changes, at most once per second | all | `GET /api/findings` |
| `levels` | every 250 ms | all | `GET /api/levels` |
| `pads` | every 250 ms, for watched pads | that connection | – |
| `properties` | every 250 ms, for watched properties | that connection | `GET /api/properties?element=ID` |
| `latency` | every 250 ms, while measuring | that connection | – |
| `recordings` | on connecting, when the list changes, every second while recording | all | `GET /api/recordings` |
| `error` | a command failed | that connection | – |
| `bye` | the program is ending | all | – |
| `watched-pads` | – | – | `GET /api/pads` |

### hello

```json
{"type":"hello","protocol":1,"time":1794073709321187,
 "version":"0.9.0","gstreamer":"1.28.8","program":"serve","pid":86677}
```

`version` is pipeline-monitor's, `program` the program's name (or `null`).
When the `pipeline-monitor` command serves recordings without a program,
hello also has `"viewer":true`, and the graph is empty.

### graph

```json
{"type":"graph","protocol":1,"time":…,
 "pipelines":[ELEMENT],
 "links":[{"src":2,"sink":4}]}
```

ELEMENT:

| Field | |
|---|---|
| `id`, `name`, `path` | |
| `factory` | the factory name, e.g. `queue` |
| `type` | the GType name, e.g. `GstQueue` |
| `bin` | whether it is a bin |
| `state` | `NULL`, `READY`, `PAUSED`, `PLAYING` |
| `pending` | the state it is changing to, if any |
| `preroll` | for sinks and bins with sinks, if known: `waiting`, `done`, `eos` (prerolled on end of stream), `live`, `no-wait` |
| `errors`, `warnings` | how many it posted, if any |
| `pads` | [PAD] |
| `children` | [ELEMENT] |

PAD: `id`, `name`, `path`, `direction` (`src`, `sink`), `presence`
(`always`, `sometimes`, `request`), `mode` (`push`, `pull`, `none` when not
active; since 0.11), `caps` (once negotiated; a pulled pad usually has
none), `ghost`, and
`flow` when the last push or pull did not return ok:
`{"result":"not-linked","count":12,"first":…,"last":…}`.

`links` connect pad ids. A link is listed once both its pads are known.

### log

```json
{"type":"log","protocol":1,"time":…,"enabled":true,"seq":42,
 "entries":[{"id":7,"level":"warning","source":"message","element":3,
   "path":"pipeline0/tee0","text":"…","debug":"…",
   "domain":"gst-core-error-quark","code":1,
   "count":3,"first":…,"last":…}]}
```

- `level`: `error` or `warning`; `source`: `message` (a bus message),
  `state-change` (a failed state change), `link` (a failed link).
- `debug`, `domain` and `code` only when the message has them.
- A repeat of the same entry counts up and updates `last`.
- On the WebSocket only entries that are new or repeated since the last
  `log` message; `seq` is the latest change. Over HTTP, the whole log (at
  most 500 entries).
- `enabled` is false when the `log=false` option is set.

### findings

```json
{"type":"findings","protocol":1,"time":…,
 "findings":[{"id":"flow:pipeline0/queue1:src","severity":"warning",
   "element":12,"path":"pipeline0/queue1","pad":14,
   "pad_path":"pipeline0/queue1:src",
   "text":"Data stopped at pipeline0/queue1:src: not-linked",
   "hint":"Pushing from this pad returned not-linked: …",
   "count":1,"first":…,"last":…}]}
```

- `severity`: `error`, `warning`, `info`. Most severe first.
- `id`: the kind and the place, the same in every run. Kinds: `flow`,
  `blocked`, `stuck`, `starved`, `link`, `missing`, `silent`, `full`,
  `dropping`, `late`.
- `text` says what was found, `hint` what it means; both state only what
  was observed.
- `count`, `first`, `last` when the problem repeats.
- For format problems: `"caps":{"upstream":"…","downstream":"…",
  "common":false}`, what each side can handle and whether they share a
  format.

### levels

```json
{"type":"levels","protocol":1,"time":…,
 "queues":[{"element":8,"path":"pipeline0/decodebin3-0/multiqueue0",
   "kind":"multiqueue","limits":{"buffers":0,"bytes":10485760,"time":100000000},
   "overruns":0,"underruns":81,
   "levels":[{"pad":69,"pad_path":"pipeline0/decodebin3-0/multiqueue0:sink_0",
     "buffers":0,"bytes":0,"time":0}]}],
 "pipelines":[{"element":45,"path":"pipeline0","state":"PLAYING",
   "clock":"GstSystemClock","clock_type":"GstSystemClock",
   "clock_provider":null,"clock_time":…,"base_time":…,"running_time":…,
   "position":…,"duration":null,
   "latency":{"live":true,"min":61333333,"max":null}}]}
```

- `kind`: `queue`, `queue2`, `multiqueue`. `leaky` (`no`, `upstream`,
  `downstream`) for queues that have it; `buffering` (percent) for queues
  that post buffering messages.
- `levels`: one entry for a queue; one per sink pad, with `pad`, for a
  multiqueue.
- `clock_provider`: the path of the element that gives the clock, or
  `null`.

### pads

```json
{"type":"pads","protocol":1,"time":…,
 "pads":[{"pad":11,"pad_path":"pipeline0/queue0:src",
   "buffers":7,"bytes":806400,"lists":0,"list_buffers":0,
   "last":{"pts":3236724333,"dts":null,"duration":33333333,
     "running_time":3236724333,"delay":6455500,"size":115200,
     "offset":null,"offset_end":null,"flags":["tag-memory"],"age":1137750},
   "segment":{"format":"time","rate":1.0,"start":0,"stop":null,"base":0},
   "window":{"seconds":0.209,"buffers_per_s":33.52,"bytes_per_s":3861500.282,
     "gap_min":29927334,"gap_avg":33424375,"gap_max":35871833},
   "events":{"segment":0,"flush":0,"eos":0,"caps":0,"gap":0}}]}
```

- Counts since watching started; `last` is the last buffer (`null` before
  the first), `delay` how late it was on the clock (negative: early),
  `age` how long ago it passed.
- `window`: rates and gaps between buffers over about the last second
  (longer for sparse streams, up to 10 s).
- A pad that is gone: `{"pad":11,"pad_path":"…","gone":true}`, once.

### properties

```json
{"type":"properties","protocol":1,"time":…,
 "elements":[{"element":20,"path":"pipeline0/fakesink0",
   "properties":[{"name":"stats","type":"GstStructure",
     "blurb":"Sink Statistics","writable":false,
     "value":{"structure":"application/x-gst-base-sink-stats",
       "fields":{"average-rate":-1,"dropped":0,"rendered":98}}}]}]}
```

PROPERTY: `name`, `type` (GType name), `blurb`, `writable`, `value`, and
where they apply `default`, `changed` (whether the value differs from the
default), `min`, `max`.

`value` by type: numbers, strings and booleans as JSON; enums
`{"nick":"paused","value":3}`; flags `{"flags":["discont"]}`; caps
`{"caps":"…"}`; structures `{"structure":"name","fields":{…}}`; fractions
`"30/1"`; arrays and lists as JSON arrays; samples
`{"type":"GstSample","caps":"…"}`; other types `{"type":"GType name"}`.
Text that is not valid UTF-8 is repaired with replacement characters.

A getter that does not return within 300 ms: the property has
`"not_answering":true` instead of `value`, and is not asked again until
that call has returned. If the whole read stops at it, the element has
`"not_answering":"name"` instead of `properties`. An element that is gone: `{"element":20,"gone":true}`, once.

`GET /api/properties?element=ID` answers with all properties of that one
element, plus `reported`: the latency the element reports
(`{"live":true,"min":…,"max":…}`), when it answers a latency query. Unknown
element: `404`.

### latency

```json
{"type":"latency","protocol":1,"time":…,
 "elements":[{"element":9,"path":"pipeline0/queue0","inputs":true,
   "outputs":true,"measured":{"min":13458,"avg":17220,"max":24542,
     "last":14417,"matched":7,"unmatched":0},
   "reported":{"live":true,"min":0,"max":null}}],
 "paths":[{"from":10,"from_path":"pipeline0/queue0:sink","to":11,
   "to_path":"pipeline0/queue0:src","measured":{…}}],
 "all":{"on":false,"buffers_per_s":0,"stopped_rate":null}}
```

- `measured`: over about the last second. Buffers are matched by running
  time; `unmatched` counts outputs whose input was not seen (an element
  that makes new timestamps). Buffers already inside an element when
  measuring starts are not counted for up to 2 s.
- `inputs`, `outputs`: whether the element has sink and src pads; without
  both there is nothing in between to measure.
- `reported`: the latency the element reports, or `null`.
- `all`: "measure all". `stopped_rate` is the buffers per second at which it
  stopped itself to protect the program, or `null`.
- A connection gets only what it measures; when its last measurement ends
  it gets one message with empty lists.

### error

```json
{"type":"error","protocol":1,"time":…,"command":"watch-pad",
 "reason":"\"pad\" must be a positive whole number."}
```

The answer to a command that failed. `command` is the command's `type`, or
`null` when the message could not be read as a command.

### recordings

The recordings in the folder (see [Recording files](#recording-files)),
the latest started first.

```json
{"type":"recordings","protocol":1,"time":…,
 "folder":"/home/me/.local/share/pipeline-monitor/recordings",
 "file_max":104857600,"total_max":1073741824,"failure":null,
 "recordings":[{"name":"pipeline-monitor-serve-20261108-143322.jsonl",
   "kind":"recording","started":…,"last":…,"size":160768,
   "running":true}]}
```

`kind`: `recording` or `snapshot` (`null` for a file whose header cannot
be read); `started` when it began, `last` the last write (µs since 1970);
`size` in bytes; `running` while it is being written. `failure`: why the
last recording ended early (it could not be written, e.g. the disk is
full), until the next one starts; otherwise `null`.
`GET /api/recordings/<name>` downloads a file.

### bye

```json
{"type":"bye","protocol":1,"time":…}
```

### watched-pads (HTTP only)

`GET /api/pads`: the pads any connection watches, how many connections
watch each, and the buffers counted.

```json
{"type":"watched-pads","protocol":1,"time":…,
 "pads":[{"pad":11,"pad_path":"pipeline0/queue0:src","viewers":1,"buffers":7}]}
```

## Commands

Sent over the WebSocket as JSON text, at most 64 KiB:

```json
{"type":"watch-pad","pad":12}
```

| Command | Fields | Effect |
|---|---|---|
| `watch-pad` / `unwatch-pad` | `pad` | that pad in `pads` messages |
| `watch-property` / `unwatch-property` | `element`, `property` | that property in `properties` messages |
| `measure-element` / `unmeasure-element` | `element` | that element's latency in `latency` messages |
| `measure-path` / `unmeasure-path` | `from`, `to` (pads) | the time from one pad to another in `latency` messages |
| `measure-all` / `unmeasure-all` | – | every element in `latency` messages, while the load allows |
| `record-start` | `view` (optional object) | starts a recording; `view` is saved in its header |
| `record-stop` | – | ends the running recording |
| `snapshot` | `view` (optional object) | saves the state of this moment |
| `record-delete` | `name` | deletes a recording that is not running |

- Ids are positive whole numbers. Property names are letters, digits, `-`
  and `_`.
- A command that fails is answered with `error`: binary or too long
  messages, invalid JSON, not an object, a missing or unknown `type`, a
  field of the wrong type, an element, pad, property or recording that
  does not exist; for recordings also: one is already running, none is
  running, or the folder cannot be written (the reason names it).
- Stopping something whose element or pad is gone always succeeds.
- Watching a pad that does not exist is answered in `pads` with
  `"gone":true`, not with an error.

## Endpoints

| Endpoint | Answer |
|---|---|
| `GET /` and the page's files | the page |
| `GET /api/hello` | `hello` |
| `GET /api/graph` | `graph` |
| `GET /api/log` | `log`, the whole log |
| `GET /api/findings` | `findings` |
| `GET /api/levels` | `levels` |
| `GET /api/properties?element=ID` | `properties` with that element and `reported` |
| `GET /api/pads` | `watched-pads` |
| `GET /api/recordings` | `recordings` |
| `GET /api/recordings/<name>` | the recording file (JSON Lines), as an attachment; 404 if there is none |
| `GET /api/opened` | the file given to `pipeline-monitor open`; 404 if there is none |
| `/ws` | the WebSocket |

JSON answers are sent with `Cache-Control: no-store`.

## Recording files

A recording (or a snapshot, a recording of one moment) is a JSON Lines
file (`.jsonl`): one JSON object per line. Its format has its own version,
1; like the protocol, it may gain fields and message types without a new
version, and readers ignore what they do not know.

1. The header:

   ```json
   {"format":"pipeline-monitor-recording","version":1,"protocol":1,
    "kind":"recording","started":1794139542123456,
    "hello":{"version":"0.10.0","gstreamer":"1.28.8","program":"serve","pid":4242},
    "view":{"pins":[...],"measuring":[...],"selected":"pipeline0/queue0",...}}
   ```

   `kind` is `recording` or `snapshot`; `started` is µs since 1970;
   `hello` is the program's [hello](#hello); `view` is what the page that
   started it showed (pins, measurements, the selected element, collapsed
   bins), or `null`.
2. Then messages in the order they were made, as above, each with its
   `type` and `time`: `graph`, `log`, `findings`, `levels`, `properties`,
   `pads`, `latency`. At the start: the graph, the whole log, the
   findings, the levels and every element's properties; then changes to
   the graph, new log entries and findings as they come, and once a second
   the levels, pipeline times and what pages watch or measure. An element
   added later gets its properties once it is in its pipeline.
3. The last line, `{"type":"end",…,"reason":…}`: `stopped`, `limit` (the
   file reached its size), `snapshot` (a snapshot is complete),
   `program-ended` or `error` (it could not be written; see `failure` in
   [recordings](#recordings)). After a crash it is missing and the last
   line may be cut off; readers skip lines they cannot read.

Passwords in URIs (`rtsp://user:password@host` becomes
`rtsp://(hidden)@host`) and the values of properties whose name has a part
`password`, `passwd`, `passphrase`, `secret`, `token`, `credential(s)` or
`auth`, or ends in `key`, are replaced by `(hidden)` before writing.

Names: `pipeline-monitor-<program>-<YYYYMMDD-HHMMSS>[-snapshot][-N].jsonl`.

## Example

```sh
curl -s http://127.0.0.1:8765/api/findings
```
