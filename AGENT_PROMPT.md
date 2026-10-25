You can annotate code for me with markers and stories that show up inside my
editor (4coder). The tool is @DIR@/4coder-markers; its full documentation is
@DIR@/README.md. Read the README first.

The service runs as a daemon on this machine. Check it with
`@DIR@/4coder-markers health`; if that fails, start it with
`@DIR@/4coder-markers daemon start --state @DIR@`.
Do not change the port (7341): my editor reaches it through an ssh tunnel.

Task:
<WHAT TO LOOK AT, e.g. "the page fault path in src/mm">

Markers: for each important region, post one marker:
  @DIR@/4coder-markers add --agent <YOUR NAME> \
      --file <ABSOLUTE PATH> --line <FIRST LINE> --end-line <LAST LINE> \
      --why "<one sentence: why this code matters>" \
      --what "<one sentence: what this code does>"
- Exactly one sentence each, under 300 characters, no newlines.
- Get line numbers from the file as it is now (grep -n, nl) and cover the whole
  region, such as a full function. Prefer 5-15 precise markers.

Story: also write one story (see "Stories" in the README) that walks me through
how these parts connect: one step per important piece of code, in the order
control or data actually flows, with a branch wherever the path splits (label
each way on with when it is taken) and steps without a file for the programs,
services or interfaces the code talks to. Keep each step's text to 2-4 sentences.
Write it to a JSON file and run:
  @DIR@/4coder-markers story check story.json
  @DIR@/4coder-markers story post story.json --agent <YOUR NAME>

Before re-posting after the code changed, run
`@DIR@/4coder-markers clear --agent <YOUR NAME>` (markers; stories are replaced
by id). When done, show me the output of `@DIR@/4coder-markers list` and
`@DIR@/4coder-markers story show <STORY ID>`.
