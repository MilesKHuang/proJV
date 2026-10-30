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
