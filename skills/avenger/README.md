# avenger -- ant-colony market research

Stage 1 of the commercialisation SOP: research before anyone designs anything.

    /avenger e-invoice archiving for small shops

## Shape

Stark is the only driver (`dynamic_dispatch`). In round 1 it dispatches the core
three lines in parallel - market, users, open source - and may wake the optional
three (tech trends, compliance, money). Every line writes its own file:
`docs/sop/avenger_<slug>_<line>.md`, so parallel lines never overwrite each
other; Stark merges them into `docs/sop/avenger_<slug>.md` in the final step.

Then Stark adversarially reviews what came back (does it answer the business
question / what is missing / what hidden link is worth chasing), re-dispatches
the blind spots, and finally emits exactly `DONE`.

## Knobs

| key | value | why |
|---|---|---|
| `max_tool_iters` | 60 | a line needs 10-15 source fetches; 3 (the default) would cut every report short |
| `submit_reserve` | 2 | warns the agent to stop exploring and submit |
| `max_parallel` | 6 | core + optional lines run at once |
| `max_rounds` | 8 | one dispatch round + a few review rounds |
| `stop_when` | `judgment == DONE` | Stark's own line is the stop signal |
| leaf `thinking` | off | six parallel reasoners is expensive for little gain |

Output goes to the workspace (`docs/sop/`), not to the proJV repo.
