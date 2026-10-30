# Authoring a skill - the sub-agent orchestra, without reading the source

> Audience: a person or an agent that must produce a **working** skill folder.
> Everything you need is on this page. No C++ knowledge required.
> Sibling pages: [module map](module-map.md) (where the engine lives), [recipes](recipes.md).

---

## 0. The 60-second model

1. A **skill is a folder**, not code: `config.json` + one `.md` prompt per agent + `tools/*.json`. Drop it in `projv_files/skills/` and it runs. Nothing is compiled.
2. The engine is an interpreter. It runs **rounds**; each round runs **steps**; each step runs **actions**; each action = one agent + one verb tool + one message.
3. Two orchestration modes: **script** (the engine owns the cast and the order) or **live** (one *driver* agent decides who works next). Pick one; you cannot mix them in the same skill.
4. Two hard rules decide whether your skill produces anything: **the engine only emits chat events** (the agent must write files itself), and **template typos fail silently** (an unknown `{{key}}` renders as an empty string).
5. Install = copy the folder. Invoke = `/skill <name> <topic>`, or `/<name> <topic>`, or the main agent's `run_skill` tool.

---

## 1. Folder layout and install

```
myskill/
├── config.json          # the program: agents, steps, stop rules
├── lead.md              # one system prompt per agent id
├── critic.md
└── tools/
    └── emit.json        # one verb tool definition per file
```

| Thing | Rule |
|---|---|
| Folder name | becomes the skill name and the slash command. ASCII, lowercase, no spaces, <=30 chars |
| Prompt files | named by `agents[].prompt`, relative to the skill folder |
| Verb tools | every `*.json` in `tools/`; the file name is free, the `name` field is what matters |
| Install | copy into `<exeDir>/projv_files/skills/<folder>` (Windows: next to `proJV_tui.exe`) |
| Discover | `/skill` or `/skill list` |
| Run | `/skill myskill <topic>`, `/myskill <topic>`, or the main agent's `run_skill` |
| Stop | `Esc` cancels at the next action/step/round boundary |

Capability tools (`read_file`, `exec_shell`, ...) come from the app registry and are **skill-wide**: every agent sees the same list. Verb tools are skill-local and are also given to every agent, which is what lets a worker that has no action in the program still answer.

---

## 2. Choose the mode

| Question | Script mode | Live mode |
|---|---|---|
| Is the cast known before the run? | yes | no |
| Does everyone have to agree? | yes: `converge=all_agree` | no |
| Does one agent decide who works next? | no | yes: one **driver** via `request_agent` |
| What drives | `round_steps` | the driver's own turns |
| Convergence | mechanical vote count, or `stop_when` | `stop_when` on a slot the driver writes |
| Examples | `steve`, `bigbang` | `avenger`, `musk`, `monica` |

**The rule that decides it:** if the cast must be fixed *and* must vote unanimously, use **script** mode and let the engine count the votes. If the cast must be chosen at runtime, use **live** mode with exactly one driver. You cannot have both: a driver cannot be a dispatch target, so putting your dispatcher into a voting step breaks the run (`cannot dispatch to a driver agent`).

---

## 3. `config.json` - top level

| Key | Type | Default | Allowed / range | Meaning |
|---|---|---|---|---|
| `name` | string | - (**required**) | matches the folder name | skill name = slash command |
| `description` | string | `""` | free text | one line, shown by `/skill list` |
| `tools` | string[] | `[]` | registry tool names | capability whitelist shared by all agents |
| `agents` | object[] | - (**required**) | see below | the cast |
| `max_rounds` | int | `8` | >= 1 | hard cap on rounds |
| `converge` | string | `"none"` | `none` \| `all_agree` | stop when every vote agrees |
| `loop_detect` | string | `""` | a slot name | stop when that slot is unchanged between two rounds |
| `dynamic_dispatch` | bool | `false` | - | live mode: drivers get `request_agent` |
| `dispatch_tool` | string | `""` | a verb tool name | verb used for dispatched workers (`""` = the worker's first tool) |
| `max_tool_iters` | int | `3` | >= 1 | tool-call budget per agent turn |
| `submit_reserve` | int | `0` | `0 .. max_tool_iters-1` | keep the last N iterations for the submission tool |
| `max_parallel` | int | `16` | `1 .. 64` | concurrent dispatched groups in one turn |
| `tool_mode` | string | `"verb"` | `verb` \| `auto` | `verb` pins the action tool; `auto` lets the model choose |
| `thinking` | string | `"on"` | `on` \| `off` | thinking mode for the whole skill |
| `stop_when` | object | - | `{slot, equals}` | stop the round loop when `slots[slot] == equals` |
| `round_steps` | step[] | `[]` | see §4 | the program |
| `on_converge` | step[] | `[]` | see §4 | runs once after the loop ends |

**`agents[]`**

| Field | Type | Default | Note |
|---|---|---|---|
| `id` | string | - (**required**) | referenced by actions; lowercase with `_` |
| `display_name` | string | `id` | shown as `[Name]` in the chat and in the sidebar |
| `prompt` | string | `""` | file name inside the skill folder |
| `thinking` | string | `""` | `""` = inherit the skill level; `on` \| `off` overrides |

**What the loader rejects outright** (the run stops with a message): an unknown value in `tool_mode`, `converge`, `thinking`, `when`, `shape` or `emit`; `max_rounds < 1`; `max_tool_iters < 1`; `submit_reserve >= max_tool_iters`; `max_parallel` outside 1..64; an action referencing an undeclared agent; an agent without `id`; missing `name`.

**What it only warns about** (visible in the chat as `[skill] config: unknown key 'x'`, with a did-you-mean suggestion): any key the loader does not know. Treat a warning as a typo in your file, not as noise.

---

## 4. Steps and actions

**Step**

| Field | Type | Default | Note |
|---|---|---|---|
| `when` | string | `""` | `""` (always) \| `round==1` \| `round>1` |
| `parallel` | bool | `false` | run all actions of this step concurrently |
| `progress` | string | `""` | template shown in the status line while the step runs |
| `actions` | object[] | `[]` | see below |

**Action**

| Field | Type | Default | Note |
|---|---|---|---|
| `agent` | string | - (**required**) | must be a declared agent id |
| `tool` | string | `""` | verb tool the agent must call; `""` with `tool_mode=auto` lets the model pick |
| `message` | string | `""` | the turn's instruction; a template (§5) |
| `capture` | string | `""` | JSON field to extract from the reply (shape `text`) |
| `into` | string | agent id | slot name the result is stored in |
| `shape` | string | `"text"` | `text` \| `vote` |
| `emit` | string | `""` | `""` \| `message` \| `vote` \| `document` - what appears in the chat |
| `vars` | object | `{}` | string values available as `{{name}}` inside this message only |

**How a reply is merged**

| shape | Reply | Effect |
|---|---|---|
| `text` | JSON containing `capture` | that string goes into the slot |
| `text` | anything else | the raw text goes into the slot |
| `vote` | `{"agree":bool,"agreed_points":[],"concerns":[],"suggested_tweak":"..."}` | counted as a vote |
| `vote` | not JSON, empty, or an error string | counted as **dissent** with a `[high]` concern - a failed call therefore blocks `all_agree` on purpose |
| any | `emit: "document"` | the text is also exposed as the run's final document |

---

## 5. Templates - and the trap

Available inside `message` and `progress`:

| Variable | Value |
|---|---|
| `{{topic}}` | the topic the skill was started with |
| `{{round}}` / `{{max_rounds}}` | current round / the cap |
| `{{round_context}}` | last round's vote summary |
| `{{residual_concerns}}` | one line per dissenting concern |
| `{{round_outputs}}` | all `emit: message` texts of this round, prefixed by role |
| `{{skills_dir}}` | absolute path of the skills directory |
| `{{X.current}}` | slot `X` written in this round |
| `{{X.prev}}` | slot `X` **as it was at the end of the previous round** |
| `{{varname}}` | a value from the action's `vars` |

**Traps, in the order they bite**

1. **An unknown key renders as an empty string and does not warn.** `{{topic.current}}` is not a slot; it silently becomes nothing. If an agent behaves as if it was told nothing, check the braces first.
2. **There is no `slot.` prefix.** It is `{{design_pm.current}}`, not `{{slot.design_pm.current}}`.
3. **`.prev` is a snapshot taken at the end of the previous round.** On the round where the loop stops, `.prev` still holds the round before it - so "what did you conclude last round" must use `.prev`, not `.current`.
4. **`vars` shadow slots** for the action they are declared on.
5. Templates are plain string substitution: no conditionals, no loops. Branching is expressed with `when` and with separate steps.

---

## 6. Execution model

```
for round = 1 .. max_rounds:
    for each step in round_steps where whenMatches(step.when, round):
        if step.parallel: run all actions concurrently, then merge
        else: run actions in order, merging each
    if stopRule(round): converged = true; break
    round_context = vote summary; prev = snapshot of slots
if not cancelled: run on_converge (with round = the last round reached)
```

**Stop rules, checked in this order after every round**

1. `stop_when`: `slots[slot] == equals` (exact string match, so pick a token like `DONE`).
2. `loop_detect`: the named slot is non-empty and identical to the previous round's value (never on round 1).
3. `converge: all_agree`: at least one vote exists and every vote has `agree: true`.

**Facts worth knowing**

- `on_converge` runs whether the loop converged *or* hit the cap - unless the run was cancelled. A cancelled run skips it.
- A step with `parallel: true` and one action behaves like a serial step.
- Actions in a parallel step share nothing: slot writes are merged after all of them finish, in declaration order.
- Cancellation is checked between actions, between steps and between rounds; in-flight API calls are aborted by the cancel probe.

---

## 7. Budgets

| Knob | What it protects you from |
|---|---|
| `max_tool_iters` | a loop that never submits. 3 for short turns, 40+ for research, 120 for long implementation. |
| `submit_reserve` | the last N iterations are reserved for the verb: near the budget the agent is told to stop exploring and submit. Must stay below `max_tool_iters`. |
| `max_parallel` | runaway concurrency when a driver dispatches everything in one reply. |
| `tool_mode: verb` | the model answering in prose instead of calling the verb. Pins the tool; the reply is the tool's argument JSON. |
| `tool_mode: auto` | needed when the action must use capability tools (`read_file`, `web_search`). `dynamic_dispatch` always implies `auto`. |
| `thinking` | cost and latency. The useful form is the per-agent override: orchestrator on, cheap workers off. |

---

## 8. The driver rule (and what it costs you)

- **Drivers = every agent that appears in any action** of `round_steps` or `on_converge`.
- Only a driver gets `request_agent`, and only when `dynamic_dispatch: true`.
- **A driver can never be a dispatch target** (`cannot dispatch to a driver agent`).
- A worker is a leaf: no `request_agent`, no way to talk to another worker. **The driver is the only channel** - if two workers must exchange information, route it through the driver's message.
- Dispatch is one layer deep by design. There is no `dispatch_from`.

---

## 9. Deliverable discipline - the failure that bites every skill

The engine emits chat events. **It never writes files for you.** A skill whose final step only "reports" produces a nice transcript and nothing on disk.

In the closing action, tell the agent explicitly:

1. `md_file` `action=write` with the whole document;
2. if it exceeds 8 KB, continue with `action=append` chunks;
3. `read_file` the tail back to prove nothing was truncated;
4. only then call the verb that emits the document to the chat.

Also decide where it lands (`docs/sop/<name>-<slug>.md` is the house convention), and make the prompt ask for a one-page summary first - a reader who never opens the appendix still gets the answer.

---

## 10. Worked example A - script mode with a unanimous vote

Three agents, one debate, a mechanical stop, a document at the end.

```json
{
  "name": "debate",
  "description": "Draft a plan, critique it from two stances, vote until unanimous, write the plan.",
  "thinking": "on",
  "tools": ["read_file", "grep_files", "md_file"],
  "tool_mode": "auto",
  "max_tool_iters": 20,
  "submit_reserve": 2,
  "max_parallel": 2,
  "max_rounds": 4,
  "converge": "all_agree",
  "agents": [
    {"id": "lead",    "display_name": "Lead",     "prompt": "lead.md",   "thinking": "on"},
    {"id": "critic_a","display_name": "Critic A", "prompt": "critic.md", "thinking": "off"},
    {"id": "critic_b","display_name": "Critic B", "prompt": "critic.md", "thinking": "off"}
  ],
  "round_steps": [
    {"when": "round==1", "progress": "Round {{round}}/{{max_rounds}} - lead drafts",
     "actions": [
       {"agent": "lead", "tool": "draft", "capture": "text", "into": "plan", "emit": "message",
        "message": "Topic: {{topic}}\n\nWrite the first version of the plan, at most 40 lines. Call draft(text=...)."}
     ]},
    {"progress": "Round {{round}}/{{max_rounds}} - lead revises",
     "actions": [
       {"agent": "lead", "tool": "draft", "capture": "text", "into": "plan", "emit": "message",
        "message": "Plan on the table:\n{{plan.current}}\n\nDissent from the last vote:\n{{residual_concerns}}\n\nRevise the FULL plan (not a diff), at most 40 lines. Call draft(text=...)."}
     ]},
    {"parallel": true, "progress": "Round {{round}}/{{max_rounds}} - vote",
     "actions": [
       {"agent": "critic_a", "tool": "vote", "shape": "vote", "emit": "vote",
        "message": "Plan:\n{{plan.current}}\n\nVote as the risk stance. A [high] concern forbids agree. Call vote."},
       {"agent": "critic_b", "tool": "vote", "shape": "vote", "emit": "vote",
        "message": "Plan:\n{{plan.current}}\n\nVote as the cost stance. A [high] concern forbids agree. Call vote."}
     ]}
  ],
  "on_converge": [
    {"progress": "Writing the plan",
     "actions": [
       {"agent": "lead", "tool": "write_plan", "capture": "doc_markdown", "emit": "document",
        "message": "FIRST persist the document: md_file action=write path=docs/sop/debate-<slug>.md, append chunks under 8KB, read the tail back. THEN call write_plan with the same Markdown.\n\nAgreed plan:\n{{plan.current}}\n\nResidual concerns:\n{{residual_concerns}}"}
     ]}
  ]
}
```

Each prompt file follows the same three paragraphs: (1) who you are and the one stance you hold; (2) what a good answer contains and the length limit; (3) which verb to call, and that a plain-prose reply counts as a failure.

`tools/draft.json`:

```json
{"name": "draft", "description": "Submit the plan text.",
 "parameters": [{"name": "text", "type": "string", "description": "the full plan", "required": true}]}
```

`tools/vote.json` - the agreed vote shape (the engine reads exactly these keys):

```json
{"name": "vote", "description": "Structured vote on the plan.",
 "parameters": [
   {"name": "agree", "type": "boolean", "description": "true only if no [high] concern remains", "required": true},
   {"name": "concerns", "type": "array", "description": "each '[severity] issue'", "required": false},
   {"name": "suggested_tweak", "type": "string", "description": "one line, or 'none'", "required": false}
 ]}
```

`on_converge` needs its own `write_plan` verb, or you can reuse `draft` with `capture: "text"`.

---

## 11. Worked example B - live mode with a driver

The driver reads a plan, splits work, judges results, and stops when it writes a token.

```json
{
  "name": "crew",
  "description": "One lead reads the plan, dispatches workers, judges on evidence, stops on DONE.",
  "thinking": "on",
  "tools": ["read_file", "write_file", "edit_file", "grep_files", "exec_shell"],
  "tool_mode": "auto",
  "dynamic_dispatch": true,
  "dispatch_tool": "report",
  "max_tool_iters": 120,
  "submit_reserve": 2,
  "max_parallel": 4,
  "max_rounds": 10,
  "converge": "none",
  "stop_when": {"slot": "board", "equals": "DONE"},
  "agents": [
    {"id": "lead",    "display_name": "Lead",     "prompt": "lead.md",   "thinking": "on"},
    {"id": "worker_1","display_name": "Worker 1", "prompt": "worker.md", "thinking": "off"},
    {"id": "worker_2","display_name": "Worker 2", "prompt": "worker.md", "thinking": "off"}
  ],
  "round_steps": [
    {"progress": "Round {{round}}/{{max_rounds}} - lead: dispatch and judge",
     "actions": [
       {"agent": "lead", "tool": "emit", "capture": "summary", "into": "board", "emit": "message",
        "message": "Job: {{topic}}\n\nYour board line from the last round:\n{{board.prev}}\n\nRound 1: read the plan, split it into tasks with a one-line definition of done each, then dispatch with request_agent in ONE reply. Round 2+: judge what came back on evidence and re-dispatch what failed. When every task is done emit exactly DONE, otherwise emit one line."}
     ]}
  ],
  "on_converge": [
    {"actions": [
      {"agent": "lead", "tool": "write_record", "capture": "doc_markdown", "emit": "document",
       "message": "Write the final record as Markdown, persist it with md_file first (append chunks), then call write_record. Job: {{topic}}"}
    ]}
  ]
}
```

The lead's prompt must say, in as many words: **if you finish by narrating instead of calling `request_agent` or `emit`, the run fails** - because `dynamic_dispatch` forces `tool_mode: auto`, so nothing pins the tool for it.

---

## 12. Verb tool files

```json
{"name": "emit",
 "description": "Report one line of state to the board.",
 "parameters": [
   {"name": "summary", "type": "string", "description": "one line, or exactly DONE", "required": true}
 ]}
```

- `name` must match the `tool` used in actions.
- `parameters` is a flat list: `name`, `type` (`string` | `boolean` | `array` | `integer` | `number`), `description`, `required`.
- Keep descriptions imperative and short: the model reads them every turn.

---

## 13. Validate before you ship

| Step | How | Pass condition |
|---|---|---|
| Loads | `/skill list` shows it, then start it on a trivial topic | no `[skill]` error line in the chat |
| No typos | watch the first seconds of the run | no `[skill] config: unknown key` warnings |
| Prompts exist | every `agents[].prompt` file is in the folder | the run does not start with an empty prompt |
| Verbs exist | every `tool` used in an action is a `tools/*.json` `name` | the agent can call it on turn one |
| Behaviour | `tests/unit/test_skill_<name>.cpp` using `setResponder` / `setResponderMulti` / `setResponderText` - no network, no API key | asserts the order of actions, the vote counting and the stop rule |
| Registration | add the test file to `TEST_SOURCES` in `tests/CMakeLists.txt` (**not a glob**) | `ctest` runs it |
| Cost | `projv_files/logs/skill_metrics.log`: one tab-separated line per turn (agent, verb, iterations, result bytes, `report` / `NO_REPORT`) | iterations stay well below `max_tool_iters`; no `NO_REPORT` |

---

## 14. Pre-flight checklist

- [ ] `name` matches the folder name; ASCII, no spaces.
- [ ] Every agent has `id` and an existing `prompt` file.
- [ ] Every `action.agent` is declared; every `action.tool` exists in `tools/`.
- [ ] `submit_reserve` < `max_tool_iters`; `max_parallel` in 1..64.
- [ ] `when` values are only `""`, `round==1`, `round>1`.
- [ ] Every `{{...}}` in every message names a real slot, a built-in, or a `var`.
- [ ] The closing action tells the agent to write the file with `md_file` and read the tail back.
- [ ] A `stop_when` value is a token the driver is told to emit verbatim (`DONE`).
- [ ] If `dynamic_dispatch` is on, the driver prompt says "always call the verb".
- [ ] A scripted unit test exists and is registered in `tests/CMakeLists.txt`.
- [ ] You ran it once on a throwaway topic and watched the sidebar agents light up as expected.

---

## 15. Anti-patterns

| Anti-pattern | Why it fails |
|---|---|
| A "reporter" step with no `emit` | nothing appears in the chat; the run looks silent |
| Expecting the engine to save the document | `on_converge` emits; only the agent can write files |
| One giant prompt doing everything | no per-turn budget control, no observable progress |
| A driver placed in a voting step | the driver then cannot be dispatched to, and `all_agree` can never fire |
| Asking workers to exchange information | they are leaves; only the driver is a channel |
| `{{slot.x}}` | no `slot.` prefix exists; it renders empty, silently |
| Relying on `{{X.current}}` in the closing step for "last round's verdict" | the converging round does not update `.prev`; use `.prev` |
| `converge: all_agree` on agents that never vote | `votes_` stays empty, so the rule never triggers and the run hits `max_rounds` |
| `tool_mode: auto` with an action that must submit | nothing pins the verb; ask in the prompt, or turn `auto` off |
| Writing >8 KB to `write_file` in one call | the tool caps at 8 KB; use `md_file` with `append` chunks |

---

## 16. Known limits (2026-10)

1. **The engine only emits.** Nothing is persisted unless the agent writes it.
2. **No approval gate inside skills.** The main agent's delete-approval dialog does not cover a sub-agent's `exec_shell`; constrain it in the prompt (build/test only, inside the project, no push, no deletes).
3. **A failed action looks like a dissent.** An exception or an empty reply becomes `[high]` in a vote, so a transient API error can block convergence. Read `skill_metrics.log` when a run stops converging.
4. **Template typos are silent** (§5).
5. **Dispatch is one layer deep**; drivers cannot be dispatched to.
6. **Hitting `max_rounds` is not an error** - the progress line says `Done (round cap reached)` and `on_converge` still runs, so a broken stop rule looks like a slow success.
7. **One child process at a time.** `exec_shell` is serialized across the app on purpose; parallel workers cannot run shells concurrently.
8. **No conditional or loop syntax** in templates. Express branching as extra steps with `when`.
