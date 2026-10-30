# musk -- kanban execution with dev x test antagonism

Stage 3 of the commercialisation SOP: build it, and prove it.

    /musk build the plan in docs\sop\steve_e-invoice-archive.md inside D:\ws\sop-test

## Shape

Musk is the only driver (`dynamic_dispatch`). Each round is one Musk turn:
read the board back from disk, split or advance tasks, dispatch dev -> test pairs
through `request_agent`, judge each result on quoted evidence, and emit one
board line. `emit DONE` ends the pipeline.

Every task is paired: `dev_1..2` implement, `test_1..2` verify independently
against the same DoD, Musk judges. Same task returned three times is escalated,
not looped.

The dev/test round trip happens inside one driver turn (`max_tool_iters=120`),
so it does not burn skill rounds; the driver's context grows linearly, which is
why leaves report <=15 lines plus evidence and keep the detail in files.

## Knobs

| key | value | why |
|---|---|---|
| `max_tool_iters` | 120 | a full dev->test->rework chain plus board writes |
| `submit_reserve` | 2 | stop exploring and emit before the budget dies |
| `max_parallel` | 4 | two pairs in flight at once |
| `max_rounds` | 10 | one round per board wave, escalation included |
| `stop_when` | `kanban == DONE` | Musk's own board line is the stop signal |
| leaf `thinking` | off | dev/test are execution roles; the driver thinks |

## Safety, stated plainly

The shell tool has **no approval dialog** on the skill path (approval lives only
on the main agent). So this skill is trusted to police itself: build / test /
read-only diagnostics only, nothing outside the job directory, no `git push`, no
deletes, no installs. `musk.md`, `dev.md` and `test.md` all repeat that rule on
purpose - it is the only gate there is. If a hard guarantee is ever needed, it
belongs in the engine as a command whitelist, not here.

Output goes to the workspace (`docs/sop/`), not to the proJV repo.
