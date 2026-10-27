---
name: expand-pseudocode
description: Implement pseudocode that the user sends from the 4coder editor, then link each piece of pseudocode to the code that implements it so the editor can present them side by side. Use when the user asks to wait for, pick up, listen for or handle pseudocode requests from 4coder, or when `4coder-markers pseudo wait` prints a request.
---

# Implement pseudocode sent from 4coder

In 4coder the user presses `C-x P`, opens the files that hold the pseudocode (and any
code it should fit into), and sends them with `C-x Shift-P`, optionally with
instructions. You claim that request, write the real code, and answer with
**links**. Each link pairs a range of pseudocode with the range of code that
implements it. From the links the service builds a story, `pseudo-ID`, that the
user walks through in the editor: each pseudocode step, then its implementation.
The steps are also linked to existing stories that cover the same code.

## 0. Find the service

```sh
M="${FCODER_MARKERS_BIN:-$(command -v 4coder-markers || echo "$HOME/.4coder-markers/4coder-markers")}"
"$M" health || "$M" daemon start
```

`FCODER_MARKERS_PORT` selects the port (default 7341); never override a value the
user set. If the service cannot be reached, tell the user and stop. The request
lives in the service, so there is nothing to implement without it. Run every
later command in the same shell, or repeat the `M=` line.

## 1. Take a request

```sh
"$M" pseudo wait --agent claude      # waits up to 9 minutes, claims the oldest request, prints it
```

- Exit status 4 means no request arrived. Run it again, or run it in the
  background, if the user asked you to keep listening.
- `pseudo list --status pending` and `pseudo claim ID --agent claude` do the same
  thing step by step.
- `pseudo show ID` prints a request again.

The request lists `instructions` (may be empty) and `files`. A file with
`(on HOST)` was opened over ssh on HOST; its path is the path on that machine. If
the files are not on this machine (check with `ls`), answer with
`"$M" pseudo fail ID --reason '...'`, giving the reason.

## 2. Find the pseudocode and where the code goes

Read every file in the request before writing anything.

- **Where pseudocode is:** a whole file (`.pseudo`, `.md`, `.txt`, or a file that is
  plainly not code), or comment blocks and plain-language lines inside a source
  file.
- **Where the code goes:**
  - The instructions decide first.
  - Otherwise, pseudocode inside a source file is implemented in place, right below
    each pseudocode block.
  - A separate pseudocode file is implemented in the source files sent with it.
  - If there are none, create a file next to it, named and written in the
    project's language.
- **Keep the pseudocode.** Never delete or reword it: it is one end of every link.
  Inside source files, leave it as comments.
- **Fit the existing code.** Use the project's style, names, error handling and
  build. Build and run the tests if the project has them, and fix what breaks.

## 3. Answer

Take line numbers **after** all edits, from the files as they are now (use
`grep -n` or `nl -ba`). Inserting code shifts the pseudocode below it. Write one
link per step of the pseudocode, in the pseudocode's order:

```sh
f=$(mktemp /tmp/answer.XXXXXX) && cat > "$f" <<'EOF'
{"summary": "2-4 sentences: what was implemented, where, and anything the user must decide.",
 "changed": ["/abs/path/server.c"],
 "links": [
  {"title": "Parse the request line",
   "text": "1-3 sentences: how this code realises the pseudocode, and any deviation from it.",
   "pseudo": {"file": "/abs/path/design.pseudo", "line": 3, "end_line": 6},
   "code":   {"file": "/abs/path/server.c", "line": 40, "end_line": 72}}
 ]}
EOF
"$M" pseudo check "$f" && "$M" pseudo answer ID "$f" --agent claude; rm -f "$f"
```

- **Paths:** absolute, as on this machine. `changed` lists every file you wrote or
  created.
- **Titles:** one line, at most 200 characters. Text is at most 2000 characters.
  An answer holds at most 120 links.
- **Partial links:** a step you did not implement gets only `pseudo`, and the text
  says why. Code that has no pseudocode of its own, such as a helper, gets only
  `code`.
- **Errors:** if `check` fails, fix what it names.
- **Re-answering:** answering again replaces the earlier answer and its story.
- **The story:** do not post it yourself; the service builds it from the links.
- **Failure:** if the request cannot be done, run `pseudo fail ID --reason '...'`.

## 4. Reply

Tell the user what you implemented: the summary, then one line per link with
`path:line` of the code. End with: `In 4coder: C-x N walks story pseudo-ID`.
If they asked you to keep listening, go back to step 1.
