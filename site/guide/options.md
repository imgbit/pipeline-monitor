# Options

Options go in brackets after the tracer's name, separated by commas:

```sh
GST_TRACERS="pipeline-monitor(port=9000,record=on)" ./my-program
```

| Option | Default | |
|---|---|---|
| `port=N` | `8765` | the page's port; if taken, the next free one is used |
| `log=false` | on | do not collect errors and warnings (the program still gets them) |
| `flow=false` | on | no data flow tracking (finding where data stopped; about 60 ns per buffer) |
| `wait=true` | off | hold the program at start until a page is open |
| `record=on` | off | record from the start, also without a page, until the program ends |
| `recordings=PATH` | `~/.local/share/pipeline-monitor/recordings` (Windows: `%LOCALAPPDATA%\pipeline-monitor\recordings`) | where recordings go (on Windows write the path with `/`) |
| `record-file-max=SIZE` | `100M` | a recording ends at this size |
| `record-total-max=SIZE` | `1G` | above this, the oldest recordings are deleted |
| `ui-dir=PATH` | – | for working on the page itself: serve its files from this folder |

Sizes take `K`, `M` or `G`.

**The `pipeline-monitor` command** opens recordings without a program:

| | |
|---|---|
| `pipeline-monitor` | the recordings in the folder |
| `pipeline-monitor open FILE.jsonl` | that recording |
| `--recordings DIR` | another folder |
| `--port N` | the port (default 8766, or the next free one) |
| `--no-browser` | only print the address |
| `--version` | the version |
