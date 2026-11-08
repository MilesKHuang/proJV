# avenger -- field research on any topic

A swarm that learns a field and hands back a map. The topic can be a product, a
technology, a market, a craft or a hobby.

    /avenger e-invoice archiving for small shops
    /avenger a monster AI behaviour tree for a beginner village

The deliverable is a field map, not a business verdict and not an action plan.
The map says how the money moves, who holds which segment, where the price power
sits, and which piece of ground one person could stand in. What to do about it
is the reader's decision.

## Shape

Stark is the only driver (`dynamic_dispatch`).

Round 1 - reconnaissance. Stark dispatches `research_aud` alone. It learns the
field from introductory material and writes a knowledge map: the vocabulary, the
key questions, the TYPE of source each line should hunt, and the local unknowns
no web search can settle. Types, not URLs: a specific link is an answer and goes
stale, a source type is a method.

Round 2 - dispatch. Stark reads the map and sends the core three lines in
parallel (market, users, open source), waking the optional four (tech trends,
compliance, money, ecosystem) only when the map justifies them. Every line
writes its own file `docs/sop/avenger_<slug>_<line>.md`, so parallel lines never
overwrite each other.

Round 3+ - adversarial review. Stark asks of every report: does it answer the
question it was dispatched with, what did it not see, and which hidden link or
unclosed gap between two actors is worth chasing. Blind spots go back out with a
sharper question. Every round Stark emits one line opening with the coverage
count, `COVER 5/8 OPEN: q3,q7` - answered questions out of the key questions in
the reconnaissance map. When every question is answered or marked
`unanswerable online`, Stark emits exactly `DONE` and the final step merges
everything into `docs/sop/avenger-<slug>.md`.

## What lands on disk

`docs/sop/avenger-<slug>.md`, opening with a one-page summary of at most 60 lines:

1. three to five plain lines: what this field is and how money moves in it;
2. the foothold: which payers, what they buy today instead, why the incumbents
   leave that work alone, what it costs one person to hold it;
3. the landscape: who plays which segment, the moat, where the price power sits;
4. build vs reuse: what a newcomer must own, what can be bought or rented;
5. the top five facts that are still unproven, each with what would settle it;
6. the cheapest next step that would settle the biggest unknown.

Below the summary: the evidence table (conclusion | grade | confidence | source |
date), one condensed section per research line, and the facts still unproven.
There is no go/no-go sentence in it, and running out of rounds ships the map
with its holes marked rather than filled by guessing.

## Citations

The map reads like a paper. Each line report numbers the facts it rests on
(`[MKT-01]`, `[USR-01]`, `[CMP-01]`, ...: one namespace per line, at most 25
ids) and closes with a `## Sources` section that gives every id a title, a URL
and the date it was opened. A source seen only in search results is marked
`search snippet only`, and that source cannot carry a `[high]` fact.

Stark keeps the ids when it merges and never renumbers them - eight lines wrote
in parallel, so a global counter cannot be rebuilt without errors. The map
therefore ends with one reference list per namespace, and every number in the
body carries its id inline. A fact with no id is deleted or rewritten as a
marked range; it is never left as a bare precise figure.

`docs/archive/skills_ab/check_refs.ps1` checks a finished map: body markers
present, reference entries present, cited-vs-listed cross-check, and one HTTP
request per URL with OK / BLOCKED / DEAD classes.

## Knobs

| key | value | why |
|---|---|---|
| `max_tool_iters` | 60 | a line needs 10-15 source fetches; 3 (the default) would cut every report short |
| `submit_reserve` | 2 | warns the agent to stop exploring and submit |
| `max_parallel` | 6 | aud runs alone in round 1; the dispatch round wakes 3-7 lines at once |
| `max_rounds` | 8 | recon + one dispatch round + a few review rounds |
| `stop_when` | `judgment == DONE` | Stark's own coverage line is the stop signal |
| leaf `thinking` | off | many parallel reasoners is expensive for little gain |

Output goes to the workspace (`docs/sop/`), not to the proJV repo.

## Cost note

Reconnaissance costs one extra round (about 5 searches and 1 fetch, capped), and
every dispatched line carries the web tools. A general topic therefore runs
longer in wall-clock time than a pure code question; the cap is `max_rounds`.
