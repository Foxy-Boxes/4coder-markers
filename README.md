# 4coder-markers

A small local service that coding agents post **markers** and **stories** to. A marker attaches
two sentences to a range of lines in a file:

- **justification**: why this code matters, in one sentence
- **explanation**: what this code does, in one sentence

4coder polls the service. Marked lines get an amber bar and highlight, and while
the cursor is inside a marked range a popup in the top right of the view shows
both sentences.

Stories go further: a graph of steps that 4coder presents as slides, walking
through how the code fits together (see [Stories](#stories)).

The service is one C++11 file with no dependencies beyond libc. It runs on the
machine where the agent works, which is often a remote server; a tunnel brings
it to the editor.

## Install

```sh
make                 # builds ./4coder-markers
make install         # copies 4coder-markers and 4coder-markers-remote to ~/.local/bin
```

## Running it in the background

```sh
4coder-markers daemon start     # detaches; store, pid and log in ~/.4coder-markers
4coder-markers daemon status
4coder-markers daemon stop
```

`--state DIR` (or `$FCODER_MARKERS_HOME`) picks another directory. The store
keeps markers and stories across restarts. `daemon start` does nothing if a
service already answers on the port.

## Remote servers

`bin/4coder-markers-remote` installs the daemon on a server over ssh and keeps
it current: it copies the sources, builds them there with the server's C++
compiler (so any OS or architecture works), and restarts the daemon only when
the sources changed. Running it again is how a server is updated.

```sh
4coder-markers-remote deploy  user@server [--dir DIR]   # install or update, start the daemon
4coder-markers-remote connect user@server               # deploy, then forward local 7341 to it
4coder-markers-remote status  user@server
4coder-markers-remote disconnect user@server
4coder-markers-remote stop    user@server               # stop the daemon there
```

`DIR` defaults to `~/.4coder-markers` on the server and receives the binary, this
README and `AGENT_PROMPT.md`, a prompt to give the agent with the paths filled
in. The tunnel is its own ssh connection, closed by `disconnect`; it uses the
same ssh connection sharing as 4coder's ssh layer when that is open. The
editor polls one port, so connect one server at a time on 7341, or pass
`--local-port` and set `agent_markers_port` to match.

## For agents

The service listens on `http://127.0.0.1:7341` (loopback only). Check it is up
with `4coder-markers health`; if it isn't, start it with `4coder-markers serve`.

The easiest way to post is the CLI, which handles JSON escaping and makes paths
absolute:

```sh
4coder-markers add --agent claude \
    --file src/net/retry.c --line 40 --end-line 52 \
    --why "Every network call depends on this backoff, so changing it affects the whole client." \
    --what "It retries failed requests with exponential backoff, capped at five attempts."
```

Rules:

- Each sentence is a single sentence of at most 300 characters with no newlines.
  Longer text is rejected with an error that says why.
- Use absolute paths. On a remote host, post the path as it exists on that host;
  4coder matches it to the `/ssh:host:/path` buffer.
- Posting again with the same `agent`, `file` and `line` replaces that marker,
  so re-running an analysis updates instead of duplicating.
- Clean up with `4coder-markers clear --agent NAME` (optionally `--file F`).

Other commands: `list [--file F]`, `rm ID`, `health`. Use `--port P` or
`FCODER_MARKERS_PORT` to change the port.

### The explain-code skill

`skills/explain-code/SKILL.md` is a Claude Code skill: whenever you ask an agent to
explain code, it posts markers and a story for what it explains and gives the
explanation in its reply, so the chat, the highlights and the slides agree.

`deploy` writes a copy with the server's paths and port filled in to
`DIR/skills/explain-code`, and links it into `~/.claude/skills` for the deploying user
when they use Claude Code. Agents that run as another user link it once
themselves; later deploys update it through the link:

```sh
mkdir -p ~/.claude/skills && ln -sfn DIR/skills/explain-code ~/.claude/skills/explain-code
```

On your own machine, `make install-skill [PORT=P]` installs it.

## HTTP API

All bodies are JSON. Errors are `{"error": "message"}` with status 400, 404 or 413.

| Method | Path | Body / query | Response |
|---|---|---|---|
| `GET` | `/v1/health` | | `{"ok":true,"revision":N,"count":M}` |
| `POST` | `/v1/markers` | a marker, an array of markers, or `{"markers":[...]}` | `{"ids":[...],"revision":N}` |
| `GET` | `/v1/markers` | `?since_revision=N` (optional), `?file=PATH` (optional) | `{"revision":N,"markers":[...]}`, or `{"revision":N,"unchanged":true}` when nothing changed since N |
| `DELETE` | `/v1/markers/ID` | | `{"removed":1,"revision":N}` |
| `DELETE` | `/v1/markers` | `?file=PATH`, `?agent=NAME` (both optional; none deletes all) | `{"removed":K,"revision":N}` |

A batch is all or nothing: if any marker is invalid, none are stored and the
error names the index of the bad one.

Marker fields:

```json
{
  "file": "/abs/path/file.c",
  "line": 40,
  "end_line": 52,
  "justification": "One sentence: why it matters.",
  "explanation": "One sentence: what it does.",
  "agent": "claude"
}
```

`line` is 1-based; `end_line` defaults to `line`. The service adds `id` and
`created` (unix time).

```sh
curl -s -X POST http://127.0.0.1:7341/v1/markers -d '{"file":"/abs/a.c","line":3,
  "justification":"Why.","explanation":"What.","agent":"curl"}'
```

## Stories

A story is a guided walk through the code: a graph of steps that 4coder
presents as slides. Each step jumps the editor to a range of lines, highlights
it and shows a title and a few sentences of narrative. Steps lead to other
steps, so a story can branch ("on a cache hit" / "on a miss"), loop back, step
outside the code (another program, a service, an API) and continue in another
story. Stories sit next to markers; the marker API above does not change.

Write the story as JSON and post it with the CLI, which checks it and makes
file paths absolute:

```sh
4coder-markers story check story.json         # validate only
4coder-markers story post story.json --agent claude
4coder-markers story show request-path         # print it back as an outline
```

```json
{
  "id": "request-path",
  "title": "What happens to a request",
  "summary": "Optional; shown on the first step.",
  "start": "accept",
  "nodes": [
    {"id": "accept", "title": "A connection is accepted",
     "file": "src/server.c", "line": 40, "end_line": 75,
     "text": "A few sentences on what this step does and why it matters here.\nNewlines start paragraphs.",
     "next": [{"to": "parse", "label": "a normal request"},
              {"to": "reject", "label": "over the rate limit"}]},
    {"id": "parse", "title": "The body is parsed", "file": "src/parse.c", "line": 12, "end_line": 60,
     "text": "...", "next": [{"to": "db"}, {"to": "auth", "story": "auth-flow", "label": "needs a session"}]},
    {"id": "reject", "title": "Rejected with 429", "file": "src/limit.c", "line": 88, "text": "..."},
    {"id": "db", "kind": "database", "title": "PostgreSQL", "text": "Outside the code: the orders table.",
     "next": ["accept"]}
  ]
}
```

Rules:

- `id`s (story and step) are 1-64 characters of `[A-Za-z0-9._-]`. Posting a story
  with an existing id replaces it, so re-post the whole story after changes.
- Each step needs `id` and `title` (one line, at most 200 characters). `text` is the
  narrative, at most 2000 characters; keep it to a few sentences so it fits the slide.
- A step with `file` needs `line` (and optionally `end_line`). A step without
  `file` is outside the code; give it a `kind` such as `"service"`, `"database"`,
  `"api"` or `"user"` (it defaults to `"external"`).
- `next` holds at most 9 edges, in the order the reader should consider them; the
  first is the default "next". An edge is `{"to": STEP, "label": "...", "story": ID}`
  or just `"STEP"`. `label` (at most 80 characters) says when this way is taken.
  `story` makes the edge continue in another story, which may be posted later.
- Edges inside a story must point at its steps. Loops are fine.
- `start` defaults to the first step.

Other commands: `story list`, `story rm ID`, `story clear [--agent NAME]`.

| Method | Path | Body / query | Response |
|---|---|---|---|
| `GET` | `/v1/stories` | `?since_revision=N` (optional) | `{"revision":N,"stories":[...]}`, or `{"revision":N,"unchanged":true}` |
| `GET` | `/v1/stories/ID` | | the story |
| `POST` | `/v1/stories` | a story, an array of stories, or `{"stories":[...]}` | `{"ids":[...],"revision":N}` |
| `DELETE` | `/v1/stories/ID` | | `{"removed":1,"revision":N}` |
| `DELETE` | `/v1/stories` | `?agent=NAME` (optional; none deletes all) | `{"removed":K,"revision":N}` |

Stories have their own revision, separate from the markers' revision. A batch is
all or nothing, and errors name the story and step at fault.

## Running the service

```sh
4coder-markers serve                         # markers kept in memory
4coder-markers serve --store ~/.4coder-markers.json   # kept across restarts
```

## In 4coder

`config.4coder` turns the editor client on:

```
use_agent_markers = true;
agent_markers_port = 7341;
```

| Keys (emacs mapping) | Command |
|---|---|
| `C-x ]` / `C-x [` | `agent_marker_next` / `agent_marker_prev` in this file |
| `C-x M` | `agent_markers_list`: every marker, jump to one |
| `C-x Shift-M` | `agent_markers_toggle_popup` |
| `C-x N` | `agent_story_present`: resume the last story, or pick one |
| `C-x Shift-N` | `agent_story_pick`: pick a story and start from its first step |

While presenting, the view shows the step's code with a teal highlight and the
slide at the bottom:

| Key | |
|---|---|
| `n`, `Right`, `Space`, `Return` | take the highlighted way on |
| `p`, `Left`, `Backspace` | back to the previous step |
| `Up` / `Down` | choose a way on |
| `1`-`9` | take that way on |
| `o`, `Tab` | the map: the whole graph, arrows move, `Return` goes there |
| `g` / `s` | jump to a step / switch story |
| `h`, `r` | the first step / scroll back to this step's code |
| `q`, `Esc`, `C-g` | leave story mode |

Other keys keep their usual commands (scrolling, search, switching panels), and
a presentation stays on its view while you work in another one.

The editor side lives in the 4coder fork that includes this repo as
`code/marker_service`: `custom/4coder_agent_markers.cpp` and
`custom/4coder_agent_stories.cpp`, which include the headers in `protocol/`.
`C-x Shift-R` there runs `4coder-markers-remote connect` for the host of the
current `/ssh:` buffer.
