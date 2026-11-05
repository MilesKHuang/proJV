# avenger -- ant-colony research on any topic

Stage 1 of the commercialisation SOP: research before anyone designs anything.
The topic can be a product, a technology, a market, a craft or a hobby - the
swarm learns the field first and then digs.

    /avenger e-invoice archiving for small shops
    /avenger a monster AI behaviour tree for a beginner village

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
decision question, what did it not see, what links the facts. Blind spots go
back out with a sharper question. When nothing is open, Stark emits exactly
`DONE` and the final step merges every line into `docs/sop/avenger-<slug>.md`,
opening with a one-page summary and 3-5 plain lines.

## Knobs

| key | value | why |
|---|---|---|
| `max_tool_iters` | 60 | a line needs 10-15 source fetches; 3 (the default) would cut every report short |
| `submit_reserve` | 2 | warns the agent to stop exploring and submit |
| `max_parallel` | 6 | aud runs alone in round 1; the dispatch round wakes 3-7 lines at once |
| `max_rounds` | 8 | recon + one dispatch round + a few review rounds |
| `stop_when` | `judgment == DONE` | Stark's own line is the stop signal |
| leaf `thinking` | off | many parallel reasoners is expensive for little gain |

Output goes to the workspace (`docs/sop/`), not to the proJV repo.

## Cost note

Reconnaissance costs one extra round (about 5 searches and 1 fetch, capped), and
every dispatched line carries the web tools. A general topic therefore runs
longer in wall-clock time than a pure code question; the cap is `max_rounds`.
