# steve -- multi-expert product design (script mode, no driver)

Stage 2 of the commercialisation SOP: turn research into one executable plan.

    /steve e-invoice archiving for small shops

## Shape

Seven roles, **all of them declared in `round_steps`, none of them a driver** -
no agent here holds `request_agent`. The engine is the orchestrator:

1. `round==1`: Steve writes the brief (core value + scope + questions).
2. `round==1`, parallel: UX / UI / backend / algorithm design their part.
3. `round==1`: PM unifies the four into one plan.
4. `round==1`: QA writes the test plan and acceptance criteria.
5. `round>1`: PM revises the plan against `{{residual_concerns}}`
   (only runs when the previous vote had a dissent - see below).
6. Every round, parallel: all seven vote (`shape=vote`).
7. On convergence: PM writes `docs/sop/steve_<slug>.md`.

## Why there is no driver here

The engine's rule is "an agent that appears in the action script is a driver,
and a driver cannot be dispatched to". A skill that wants both runtime dispatch
*and* engine-counted votes hits that rule head-on. Steve's cast is fixed and
must be complete, so the right answer is to let the engine orchestrate: steps
run in order, so PM always sees finished designs - stronger than hoping a driver
waits. Same shape as bigbang / boss_forge.

`converge=all_agree` therefore works mechanically: seven votes in `votes_`, all
green means done. A dissent sends the plan back to PM, and the revision step is
gated by `when: round>1`, which can only run when round 1 did not converge - so
a unanimous round 1 never pays for a revision.

## Knobs

| key | value | why |
|---|---|---|
| `max_tool_iters` | 40 | designers read one research file (`file_requests`) and submit; 3 would be too tight |
| `max_rounds` | 6 | 1 build round + up to 5 revision rounds |
| `converge` | `all_agree` | the whole point: unanimity is counted, not claimed |
| `loop_detect` | `design_pm` | if the revised plan is byte-identical, stop instead of spinning |
| `thinking` | on | this is the product-thinking stage; the cost is worth it |

Output goes to the workspace (`docs/sop/`), not to the proJV repo.

## Vote discipline (added after the Ostlr run)

The first real run took **six** rounds to converge, and most of the later
dissents were not design findings: they were stale task ids, two numbers for one
bound, and concerns the plan had already answered in a "dissent resolved" list
while the body still contradicted it. Two rounds were spent on that alone.

Every role now carries the same four rules:

| Rule | Why |
|---|---|
| A `[high]` means "cannot be built" or "my stance is unmet" - nothing else | kills the "I found a typo, vote no" round |
| Every `[high]` must quote the plan line it is about | makes the concern falsifiable |
| Two `[high]` maximum per vote, the rest are `[medium]` | forces ranking instead of accumulation |
| Never re-raise an answered concern; say "resolved" | stale text stays `[medium]` and cannot block |

The PM additionally has to **fix at the source**: change the body, not only the
resolution list, or the same concern returns next round.

## Action inventory (the other thing the first run taught us)

Nobody owned "what can the user click". UX designed zones, UI designed component
states, and the launcher dialog shipped with no way to open it. So now:

- UX must name, for every zone in the IA, **the trigger that opens it**.
- UI must produce an **action inventory** (`action -> control -> location ->
  keyboard`); a component list without one is incomplete.
- The PM carries the inventory into the plan and into the DoD.
- QA adds a first-sight usability check: the primary action, mouse only, in
  three steps or fewer, with a screenshot as evidence.

## Knobs worth knowing

`max_rounds = 6` is the backstop, not the target. With the discipline above a
converged plan usually lands in two rounds. Raising it to 20 does not buy a
better plan: each extra round is a full rewrite, and every rewrite is a fresh
chance to introduce the contradictions the next vote then finds.
