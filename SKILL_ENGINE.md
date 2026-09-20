# SKILL_ENGINE -- Multi-Agent Workflow Engine Design (v1.1)

> Purpose: a single extension point -- drop in one JSON (+prompts/tools) and get a multi-agent workflow, with zero C++ changes. Covers the roundtable / expert panel / brainstorming / Kanban / swarm family, implemented in a **unified, simplified, bounded** way; **byte-for-byte compatible on the normal path** with the existing `/bigbang`.
> Relationship: `bigbang_debate` is this engine's **built-in skill #1**, not a special case.
> v1.1: incorporates review fixes (Bug A/B + C-G + derived tool registration + event text spec). This document is self-contained and does not depend on any older document.

---

## 0. Design Proposition (why this shape)

Do not build a general-purpose workflow engine (DAG/Turing). Build a trio of **blackboard + program + stop rule**; the diversity lives in configuration, and the engine is just a small interpreter.

| Capability a general engine would want | Cost | Needed by this family |
|--------------------|------|-------------|
| Dynamic list map/reduce | Type system + evaluator | No |
| Content-based if/branching | Expression engine | No |
| Globally mutable blackboard / transactions | Concurrency model | No |
| Async task queue | Scheduler + lifecycle | No |

Compress the engine to the minimum: auditable (~250 lines), predictable, and every workflow is data. This is not a compromise; it is a design choice.

---

## 1. Abstract Model (the five pieces)

```
 skill.json
    |
    v
 [SkillRunner] --owns--> Blackboard { slot -> text | agentId -> VoteResult }
    |  loads
    +----> roles: map<id, SubAgent(DeepSeekClient + Session + toolDefs)>
    |
    for round in 1..max_rounds:                # the only control structure
        for step in round_steps:               # program
            actions (parallel | seq), gated by `when`
            merge: capture -> slot / votes ; emit -> onEvent -> TUI
        if STOP(rule): break
    run on_converge  -> document event          # post program
```

| Object | Responsibility | Implementation |
|------|------|------|
| Role | LLM role: id + prompt + tools | `SubAgent` |
| Blackboard | Text slots `map<slot,string>` + votes `map<agentId,VoteResult>`; the only channel between steps | `SkillRunner` |
| Step group | A set of calls, parallel/sequential, gated by `when` | `SkillStep` |
| Call (action) | role + tool + message + capture/into/shape/emit | `SkillAction` |
| Program + Stop | `round_steps` run once per round until stop or max_rounds | `run()` |
| Post program | `on_converge` (produces the doc) | `run()` |
| Events | `onEvent(role, kind, text)` streams to the TUI | `Callbacks` |

### 1.1 The Type System Has Only Two Shapes

| shape | Content | Engine use |
|-------|------|---------|
| `text` | A single string | Statements / summaries (the vast majority) |
| `vote` | agree / agreed_points / concerns / suggested_tweak | Convergence decision |

**None of the words `statement` / `agree` / `doc_markdown` / `bigbang` appear anywhere in the engine** -- field names, tool names, and text all come from configuration. This is the precondition for "swap the config, swap the scenario".

### 1.2 Blackboard = Named Slots (core interface)

- Steps **do not reference each other directly**; they only read/write named slots and the vote area.
- Text: `into` determines which slot to write (default = agent id); templates `{{slot.current}}` / `{{slot.prev}}` read.
- Votes: stored by **voter agent id** (`votes_[agentId]`), and **not written to a text slot** (see 4.3 Bug-A).
- Kanban's `task1/task2`, brainstorm aggregation, and expert-panel summaries all reuse the same text-slot mechanism.
- `{{round_outputs}}` = concatenation of all text captures this round (so a synthesizer can read the whole field at once).

---

## 2. Coverage Matrix and the "Bounded" Boundary

| Workflow | Program (`round_steps`) | Stop | Rounds | Status |
|--------|---------------------|------|------|------|
| Roundtable (bigbang) | propose -> integrate -> vote | `all_agree` | multi-round | built-in, byte-for-byte compatible on the normal path |
| Expert panel | N experts speak in parallel -> host summarizes | `none` | 1 | directly supported |
| Brainstorming | N diverge in parallel -> cluster | `none` | 1 | directly supported |
| Kanban | plan -> assign work (different `into` per worker) -> review | `all_agree` \| `none` | multi-round = re-plan | supported |
| Swarm | `dynamic_dispatch` + `request_agent` | `none` | max_rounds fallback | bounded |

Explicitly **not done** (honest boundary, future work): dynamic list map/reduce, content-conditional branching, globally mutable blackboard (named-slot assignment only), async/task queues.

---

## 3. DSL Specification

### 3.1 Top Level

| Field | Type | Required | Default | Description |
|------|------|------|------|------|
| `name` / `description` | string | yes | - | |
| `tools` | array\<string\> | no | [] | Whitelist of **capability tools** (read_file etc., read-only recommended). Speaking tools are not listed here |
| `agents` | array | yes | - | see 3.2 |
| `max_rounds` | int | yes | - | |
| `converge` | string | no | `"none"` | `all_agree` \| `none` |
| `loop_detect` | string | no | `""` | Text slot name; if the slot is the same this round as last round, stop early |
| `dynamic_dispatch` | bool | no | false | |
| `max_recursion` | int | no | 3 | |
| `round_steps` | array | yes | - | program |
| `on_converge` | array | no | [] | post program |

> **Two kinds of tools**: (1) **capability tools** = do the real work (read_file / pytool scripts), controlled by the `tools` whitelist, executed via `ToolRegistry`; (2) **speaking tools** = mere shells that "let the model spit out parameters in a structure", with no execution body, defined in `tools/*.json`, and auto-registered by the engine to the corresponding agent per action. **Definition sources**: capability tools come from `ToolRegistry::getToolDefinitions()`; speaking tools come from `tools/*.json`. `pytool` belongs to kind 1 and is completely unaffected.

### 3.2 agent

| Field | Type | Required | Default |
|------|------|------|------|
| `id` | string | yes | - |
| `display_name` | string | yes | - |
| `prompt` | string | yes | - | relative to the skill directory |

> v1.1 removes `can_write_doc`: which **speaking tools** each agent gets is **derived automatically** from the `(agent, tool)` pairs in the program (see 4.3 registration). One fewer field = one fewer class of inconsistency bug.

### 3.3 step group

| Field | Type | Required | Default |
|------|------|------|------|
| `when` | string | no | `""` (`""` \| `round==1` \| `round>1`) |
| `parallel` | bool | no | false |
| `progress` | string | no | `""` (onProgress text) |
| `actions` | array | yes | - |

### 3.4 action (the generic core, key to removing hard-coding)

| Field | Type | Required | Default | Description |
|------|------|------|------|------|
| `agent` | string | yes | - | role id |
| `tool` | string | yes | - | speaking tool name (from tools/*.json) |
| `message` | string | yes | - | template |
| `capture` | string | no | `""` | which field of args to take (`shape=text`); if empty, take the whole args JSON |
| `into` | string | no | =`agent` | text slot name (effective for `shape=text`) |
| `shape` | string | no | `"text"` | `text` \| `vote` |
| `emit` | string | no | `""` | `""` \| `message` \| `vote` \| `document` |
| `vars` | object | no | `{}` | local template variables for this action |

> `shape=vote`: ignore `capture`/`into`, parse the whole args as `VoteResult`, store by **voter agent id** in `votes_[agent]`, and **do not write a text slot**.

### 3.5 Template Variables

`{{topic}}` `{{round}}` `{{max_rounds}}` `{{round_context}}` `{{residual_concerns}}` `{{round_outputs}}` `{{<slot>.current}}` `{{<slot>.prev}}` `{{<var>}}`

- `slot` = the `into` value (default agent id). `current` = that slot this round; `prev` = that slot last round (snapshotted at the end of each round).
- `{{round_context}}` = the previous round's `voteSummary()` (empty before this round's vote).
- `{{residual_concerns}}` = `residualConcerns()`.
- `{{round_outputs}}` = all text captures this round, concatenated in action order: `"[" + display_name + "]\n" + text + "\n\n"`.
- Shared context (`{{session_context}}` removed): the old implementation injected it into each agent as a **system message** (seedSharedContext); v1.1 keeps that and does not expose it as a template variable.
- Missing/undefined -> empty string; `fillTemplate` never throws.

### 3.6 tools/*.json

Skill-level **speaking tool** definitions (name/description/parameters). Lets each skill customize its tool names (bigbang uses `bigbang_turn/vote/doc`; other skills can call theirs `speak`/`check`).

---

## 4. Engine Interfaces

### 4.1 SubAgent (role runtime, `sub_agent.h/.cpp`)

| Member | Type | Description |
|------|------|------|
| `name_` `systemPrompt_` `cfg_` `tools_` | | |
| `toolWhitelist_` | vector\<string\> | capability tool whitelist |
| `toolDefs_` | vector\<ToolDefinition\> | speaking tool + capability tool definitions |
| `client_` / `session_` | DeepSeekClient / Session | |
| `lastReasoning_` / `lastError_` | string | |
| `toolInterceptor_` | function\<string(string,string)\> | request_agent |
| `responder_` | BigbangResponder | test seam |

Methods: `configure(cfg,tools,whitelist,maxFileRounds)` / `registerToolDef` / `seedSharedContext` / `setToolInterceptor` / `setResponder` / `turn(message,verb,force)->string` / `cancel()`.

- `requestTool(verb,force,out)`: `doRequest(verb,force)`; if `!valid` and `lastError_` contains `"tool_choice"` -> retry once with `force=false`.
- Inside `doRequest`: `toolChoice = force ? verb : "auto"`; **`lastReasoning_` is cleared only here** (required for multi-round thinking).

### 4.2 `turn()` Exact Flow

```
turn(msg, verb, force):
  session_.addMessage(Message::User(msg))
  for r in 0..maxFileRounds_:
    string text; tc = requestTool(verb, force, text)
    if !tc.valid:
       out = !lastError_.empty() ? "[error] " + lastError_
           : !text.empty()      ? text
           :                      ""                       // no tool_call and no text
       if (!out.empty()) session_.addMessage(Message::Assistant(out))
       return out
    Message am = Message::Assistant(); am.toolCalls={tc}; am.reasoningContent=lastReasoning_
    session_.addMessage(am)
    if tc.name == verb:
       files = args["file_requests"]
       if (!files.empty() && r < maxFileRounds_):
          session_.addMessage(Message::Tool(tc.id, verb, executeFileRequests(files))); continue
       session_.addMessage(Message::Tool(tc.id, verb, "OK")); return tc.arguments   // args JSON as-is
    else:                                                                            // side tool
       session_.addMessage(Message::Tool(tc.id, tc.name, dispatchSide(tc))); continue
  return ""
```

`dispatchSide(tc)`: `request_agent` with an interceptor -> call the interceptor; otherwise if `tc.name` is whitelisted -> `tools_->execute`; otherwise `"Error: tool not allowed"`.

**Boundary behavior (compared with the old `roundtable.cpp`, deviations marked):**

| Case | Old behavior | v1.1 behavior |
|------|--------|-----------|
| API error | returns `"[bigbang error] " + msg` | `"[error] " + msg` (**deviation**, the engine contains no bigbang wording) |
| No tool_call but has text | returns that text | same |
| No tool_call and no text | returns `"(no statement)"` | returns `""` (**deviation**, SkillRunner does not push a bubble) |
| File request round-trip | max 2 rounds, each `Tool(...,"OK"\|file content)` | same |

> Deviations occur only on **exception/boundary paths**; the normal path (the five message types in 8) is byte-for-byte identical. The golden-master comparison uses a scripted responder along the normal path.

### 4.3 SkillRunner (blackboard interpreter, `skill_runner.{h,cpp}`)

Members: `cfg_ tools_ cbs_ topic_ sharedContext_ skillsDir_ config_` / `agents_: map<id,unique_ptr<SubAgent>>` / `slots_ prevSlots_: map<slot,string>` / `votes_: map<agentId,VoteResult>` / `roundContext_` / `cancelRequested_`.

**Concurrency rule:** in `executeSteps`, each action writes only its **local slot** `result[i]` in its own thread; after all join, the runner thread **merges them sequentially** into `slots_/votes_` and fires events. The shared maps are touched only by the runner thread -> no locks, no races.

```
run(topic):
  if !modelSupportsTools(cfg_.model): event(system, msg); return
  load config.json ; load tools/*.json -> map<name,ToolDefinition>
  # derived registration (v1.1): scan all actions, obtain perAgent[agent] = {tool...}
  for a in agents:
     verbDefs = { jsonDef[t] : t in perAgent[a.id] }   # speaking tools (from tools/*.json, derived per action)
     capDefs  = { regDef[t]  : t in config_.tools }    # capability tools (from ToolRegistry definitions)
     defs = verbDefs + capDefs
     ag = SubAgent(a.display_name, read(a.prompt))
     ag.configure(cfg_, tools_, config_.tools, 2); for d in defs: ag.registerToolDef(d)
     ag.seedSharedContext(sharedContext_)
     if dynamic_dispatch: ag.registerToolDef(defs["request_agent"]); ag.setToolInterceptor(...)
     agents_[a.id] = ag
  for round in 1..max_rounds:
     if cancelRequested_: break
     executeSteps(round_steps, round)
     if cancelRequested_: break
     if stopRule(): converged = true; break
     roundContext_ = voteSummary(); prevSlots_ = slots_
  if cancelRequested_: progress("Cancelled"); return
  executeSteps(on_converge, lastRound)          # emit the doc
  progress(converged ? "Done (converged)" : "Done (round cap reached)")
```

`executeSteps(steps, round)`: for each step: `whenMatches` else skip; if `progress` -> progress(fill); parallel -> threads write `result[i]` then join, else sequential; after join, `mergeAction(a, result[i])` one by one. (Check cancel before each step.)
`whenMatches(w, round)`: `""`->true; `"round==1"`->round==1; `"round>1"`->round>1.

`mergeAction(a, r)` -- **driven by config, not by tool name** (**Bug A fix: vote does not write a text slot**):
```
if a.shape == "vote":
   if r.empty():          votes_[a.agent] = {agree=false, concerns=["[high] (no structured vote)"]}
   else if isJson(r):     votes_[a.agent] = parseVote(fromJson(r))
   else:                  votes_[a.agent] = {agree=false, concerns=["[high] "+r]}   // r = "[error] ..."
   if a.emit=="vote": event(display(a.agent), vote, voteLine(votes_[a.agent]))
   return                                              // does not write slots_ (in the old code votes_ and leonardS_ were separate anyway)
else: // text
   s = r.empty() ? "" : (a.capture.empty() ? trim(r) : value(fromJson(r), a.capture))
   slots_[a.into] = s
   if (!s.empty()):
      if a.emit=="message":  event(display(a.agent), message, s)
      if a.emit=="document": event(display(a.agent), document, s)
```

`stopRule()` (**Bug B fix: only consider entries that actually voted**):
```
if loop_detect nonempty && round>1 && !slots_[ld].empty() && slots_[ld]==prevSlots_[ld]: return true
if converge=="all_agree" && !votes_.empty() && all(v.agree for v in votes_.values()): return true
return false
```

`voteSummary()` / `residualConcerns()`: in `agents` config order, read `votes_[agent.id]` per agent (skip if absent); the format strictly reproduces the old `buildVoteSummary`/`buildResidualConcerns` (see 8 byte baseline).
`voteLine(vr)`: `(vr.agree?"AGREE":"DISSENT") + (vr.concerns non-empty ? " (concerns: "+join("; ")+")" : "")`.

`handleRequestAgent(id,msg,depth)`: `depth>=max_recursion` -> `"[error] max recursion depth exceeded"`; unknown id -> `"[error] unknown agent"`; otherwise `agents_[id]->turn(msg, firstVerbToolOf(id), false)` returns the raw result (`firstVerbToolOf(id)` = the first speaking tool this agent appears with in this program; the engine contains no workflow-specific names).

### 4.4 Events (unified outlet, strictly faithful)

`onEvent(role, kind, text)`, `kind in {message, vote, document, system}`. The TUI generates bubbles by kind (**must reproduce the old `app_tui.cpp` byte for byte**):

| kind | Bubble text | Old counterpart |
|------|---------|--------|
| `message` | `"**[" + role + "]** " + text` | onStatement |
| `vote` | `"**[" + role + " vote]** " + text` (text = voteLine) | onVote |
| `document` | `text` (as-is, no prefix) | onDoc |
| `system` | `"**[" + role + "]** " + text` (role="system") | onStatement("system",...) |

---

## 5. bigbang Compatibility (built-in skill #1)

1. **Config instance:** `bigbang_debate` is the JSON in 8; the program = propose/integrate/vote, stop = `all_agree`, post program = doc.
2. **Byte baseline (self-contained):** the five message types in 8 are **byte-for-byte equal** to the C++ concatenation in the old `roundtable.cpp run()`; for `voteSummary`/`residualConcerns`/`<<display>>` prefixes see 4.3/4.4. Zero deviation on the normal path; for exception-path deviations see the 4.2 table.
3. **Four-phase migration (low risk):** coexist (new engine + `/skill`, old `/bigbang` untouched) -> golden-master comparison (drive the old `Roundtable` with a scripted responder, record the actual messages sent, assert `fillTemplate(config) == original string`) -> switch the alias (`/bigbang` -> `startSkill("bigbang_debate")`) -> delete the old code (roundtable.* + prompts dead code).
4. **Regression protection:** the old 3 convergence contracts (unanimous convergence / round cap still emits a doc / loop-detect) are rewritten as SkillRunner tests in phase D to lock them down.

---

## 6. File List and Phases

| File | Action | Phase |
|------|------|------|
| `src/core/skill_config.h` | new (SkillConfig/Agent/Step/Action + VoteResult) | A |
| `src/core/sub_agent.{h,cpp}` | new (role runtime) | A |
| `src/core/skill_runner.{h,cpp}` | new (interpreter + templates + seeding + scheduling) | A |
| `src/tui/app_tui.{h,cpp}` | modify (`/skill` routing + `onEvent` callback + `startSkill`) | A/C/D |
| `src/core/roundtable.{h,cpp}` | delete | D |
| `src/core/prompts_loader.cpp` / `prompts.h` | delete bigbang dead code (content migrated into `ensureDefaultSkills` seeding) | D |
| `src/tui/main_tui.cpp` | `[bigbang]` -> `[skill]` | D |
| `tests/unit/test_skill_{messages,runner}.cpp` | new (golden master + convergence contracts) | B/D |
| `CMakeLists.txt` / `tests/CMakeLists.txt` | modify | A/D |

No new dependencies on the engine side; reuses `ToolRegistry` / `Session` / `DeepSeekClient`; does not change IProcessRunner or the TUI render path. Skill files (config/prompts/tools) are seeded at runtime by `ensureDefaultSkills()` into `projv_files/skills/`, removing the dependency on the exe directory.

---

## 7. Verification Checklist

| # | Verification item | Phase |
|---|--------|------|
| 1 | Golden master: the five bigbang message types are byte-for-byte equal (normal path) | B |
| 2 | `/bigbang` UX unchanged (bubbles/status bar/Esc) | C |
| 3 | Three convergence contracts (unanimous / round cap emits doc / loop-detect) | D |
| 4 | Whitelist effective: debate roles cannot `exec_shell`/`write_file` | A |
| 5 | Generalization: renaming bigbang tools to `speak` is equivalent with only a config change | A |
| 6 | `{{round_outputs}}` summarizes the whole field; `{{slot.current/prev}}` correct | A |
| 7 | `converge:none` runs the full rounds and executes `on_converge` | A |
| 8 | One config each for the five workflow types all run (roundtable/expert/brainstorm/kanban/swarm) | A |
| 9 | `request_agent` exceeding `max_recursion` returns an error without crashing | A |
| 10 | Bug A regression: after voting, `{{leonard.prev}}` is still the plan, not the vote text | D |
| 11 | Bug B regression: an agent that did not vote does not make `all_agree` permanently false | D |
| 12 | Full build + existing unit tests pass | each phase |

> Accepted deviations (not counted as failures): exception-path wording (`[error] ` vs `[bigbang error] `; empty reply `""` vs `"(no statement)"`), see 4.2.

---

## 8. `skills/bigbang_debate/config.json` (built-in skill #1)

> Byte-aligned with the old `roundtable.cpp` normal path. The fields `capture/into/shape/emit` are the generalized form; vote has no `into` (stored by agent).

```json
{
  "name": "bigbang_debate",
  "description": "Three-role engineering debate: propose, integrate, vote, emit an execution doc.",
  "tools": ["read_file", "grep_files", "file_search"],
  "agents": [
    {"id": "sheldon", "display_name": "Sheldon", "prompt": "sheldon.md"},
    {"id": "penny",   "display_name": "Penny",   "prompt": "penny.md"},
    {"id": "leonard", "display_name": "Leonard", "prompt": "leonard.md"}
  ],
  "max_rounds": 8,
  "converge": "all_agree",
  "loop_detect": "leonard",
  "round_steps": [
    {"when": "round==1", "parallel": true,
     "progress": "Round {{round}}/{{max_rounds}} - proposals (Sheldon + Penny)",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_turn", "capture": "statement", "into": "sheldon", "emit": "message",
        "message": "Topic: {{topic}}\n\nCall bigbang_turn with your proposal."},
       {"agent": "penny", "tool": "bigbang_turn", "capture": "statement", "into": "penny", "emit": "message",
        "message": "Topic: {{topic}}\n\nCall bigbang_turn with your proposal."}
     ]},
    {"when": "round>1", "parallel": true,
     "progress": "Round {{round}}/{{max_rounds}} - proposals (Sheldon + Penny)",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_turn", "capture": "statement", "into": "sheldon", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} ===\n\nLeonard compromise from previous round:\n{{leonard.prev}}\n\nPrevious vote results:\n{{round_context}}\n\nPenny's previous proposal:\n{{penny.prev}}\n\nCall bigbang_turn."},
       {"agent": "penny", "tool": "bigbang_turn", "capture": "statement", "into": "penny", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} ===\n\nLeonard compromise from previous round:\n{{leonard.prev}}\n\nPrevious vote results:\n{{round_context}}\n\nSheldon's previous proposal:\n{{sheldon.prev}}\n\nCall bigbang_turn."}
     ]},
    {"when": "round==1", "progress": "Round {{round}}/{{max_rounds}} - integration (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "bigbang_turn", "capture": "statement", "into": "leonard", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Integrate ===\n\nSheldon:\n{{sheldon.current}}\n\nPenny:\n{{penny.current}}\n\nCall bigbang_turn."}
     ]},
    {"when": "round>1", "progress": "Round {{round}}/{{max_rounds}} - integration (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "bigbang_turn", "capture": "statement", "into": "leonard", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Integrate ===\n\nSheldon:\n{{sheldon.current}}\n\nPenny:\n{{penny.current}}\n\nPrevious vote results:\n{{round_context}}\n\nCall bigbang_turn."}
     ]},
    {"parallel": true, "progress": "Round {{round}}/{{max_rounds}} - vote",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."},
       {"agent": "penny", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."},
       {"agent": "leonard", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."}
     ]}
  ],
  "on_converge": [
    {"progress": "Writing execution document (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "write_bigbang_doc", "capture": "doc_markdown", "emit": "document",
        "message": "Topic: {{topic}}\n\nFinal approved proposal:\n{{leonard.current}}\n\nResidual concerns:\n{{residual_concerns}}\n\nCall write_bigbang_doc."}
     ]}
  ]
}
```

Accompanying `tools/*.json` (speaking tool definitions, seeded by `ensureDefaultSkills`):

```text
bigbang_turn.json : {"name":"bigbang_turn","parameters":[{"name":"statement","type":"string","required":true},{"name":"file_requests","type":"array","required":false}]}
bigbang_vote.json : {"name":"bigbang_vote","parameters":[{"name":"agree","type":"boolean","required":true},{"name":"agreed_points","type":"array","required":false},{"name":"concerns","type":"array","required":false},{"name":"suggested_tweak","type":"string","required":true}]}
write_bigbang_doc.json : {"name":"write_bigbang_doc","parameters":[{"name":"doc_markdown","type":"string","required":true}]}
```

---

## 9. Covering Other Workflows with the Same Engine (config only)

**Expert panel** (1 round, host summarizes):
```json
{ "name": "expert_panel", "max_rounds": 1, "converge": "none",
  "agents": [
    {"id": "e1", "display_name": "Expert A", "prompt": "e1.md"},
    {"id": "e2", "display_name": "Expert B", "prompt": "e2.md"},
    {"id": "host", "display_name": "Host", "prompt": "host.md"}
  ],
  "round_steps": [
    {"parallel": true, "actions": [
      {"agent": "e1", "tool": "speak", "capture": "text", "into": "e1", "emit": "message",
       "message": "Topic: {{topic}}\nGive your expert view."},
      {"agent": "e2", "tool": "speak", "capture": "text", "into": "e2", "emit": "message", "message": "Topic: {{topic}}."}
    ]},
    {"actions": [
      {"agent": "host", "tool": "speak", "capture": "text", "into": "summary", "emit": "document",
       "message": "Topic: {{topic}}\n\nAll views:\n{{round_outputs}}\n\nSynthesize."}
    ]}
  ]}
```

**Brainstorming** (N diverge -> cluster): isomorphic to the expert panel, `max_rounds:1`, the last step `synthesizer` reads `{{round_outputs}}` to cluster.

**Kanban** (multi-round = re-plan; different `into` per worker; review votes):
```json
{ "name": "kanban", "max_rounds": 4, "converge": "all_agree", "loop_detect": "review",
  "agents": [
    {"id": "planner", "display_name": "Planner", "prompt": "planner.md"},
    {"id": "w1", "display_name": "Worker 1", "prompt": "worker.md"},
    {"id": "w2", "display_name": "Worker 2", "prompt": "worker.md"},
    {"id": "w3", "display_name": "Worker 3", "prompt": "worker.md"},
    {"id": "review", "display_name": "Reviewer", "prompt": "review.md"}
  ],
  "round_steps": [
    {"actions": [
      {"agent": "planner", "tool": "speak", "capture": "text", "into": "plan", "emit": "message",
       "message": "Topic: {{topic}}\nPlan the tasks."}
    ]},
    {"parallel": true, "actions": [
      {"agent": "w1", "tool": "speak", "capture": "text", "into": "task1", "emit": "message", "vars": {"task": "task 1"},
       "message": "Do {{task}} per plan:\n{{plan.current}}"},
      {"agent": "w2", "tool": "speak", "capture": "text", "into": "task2", "emit": "message", "vars": {"task": "task 2"},
       "message": "Do {{task}} per plan:\n{{plan.current}}"},
      {"agent": "w3", "tool": "speak", "capture": "text", "into": "task3", "emit": "message", "vars": {"task": "task 3"},
       "message": "Do {{task}} per plan:\n{{plan.current}}"}
    ]},
    {"actions": [
      {"agent": "review", "tool": "check", "shape": "vote", "emit": "vote",
       "message": "Plan:\n{{plan.current}}\n\nResults:\n{{round_outputs}}\n\nApprove?"}
    ]}
  ]}
```

**Swarm** (bounded): `dynamic_dispatch:true`, `max_recursion:3`, `converge:"none"`, a single-step `round_steps` where the manager dispatches using `request_agent`.

> Key conclusion: from roundtable to kanban, **zero engine changes, only config swaps**. Tool names (`speak`/`check`) are defined by each skill's own `tools/*.json`; the engine preloads no workflow-specific semantics.

---

## 11. Implementation Corrections (v1.2) -- code is authoritative

The following differences exist between the spec (1-9) and the shipped implementation; this section is authoritative.

### 11.1 New/Revised DSL Fields

| Field | Default | Description |
|------|------|------|
| `dispatch_tool` | `""` | the verb tool for `request_agent` targets; if empty, take the agent's first tool |
| `max_tool_iters` | 3 | number of tool calls allowed per `turn()` (the old implementation hard-coded 2->3) |
| `tool_mode` | `"verb"` | `verb`=lock to the action's tool; `auto`=the model may use any tool in the registry |
| `stop_when` | none | `{"slot":"x","equals":"y"}`: stop the round loop when the slot equals y |

The defaults deliberately preserve the old behavior -> zero changes to the bigbang config; the comparison tests are unaffected.

### 11.2 Tool Location (revision to 3.6)

Speaking tools are **skill-local**: `projv_files/skills/<name>/tools/*.json`
(3.6 originally said shared `skills/tools/`; changed. Skill-local -> a determinate tool pool, no cross-skill name collisions.)

### 11.3 New Template Variable

`{{skills_dir}}` = the skill's root directory (so skill_maker knows where to write).

### 11.4 SkillRunner Construction

`SkillRunner(cfg, tools, cbs, sharedContext="", skillsDir="")`; if `skillsDir` is empty, `getProjvDir()+"/skills"` (tests may point to any skill directory).

### 11.5 Swarm Measured Boundary (revision to the "bounded" wording in 2)

Measured via `test_skill_swarm.cpp`: the swarm was previously **effectively unusable** (`request_agent` targets could not get a verb). After the fix:

| Capability | Conclusion |
|------|------|
| manager -> request_agent -> worker dispatch | works |
| Dispatches per round | <= `max_tool_iters` (default 3) |
| Parallel fan-out | none (only parts[0] is taken from multiple tool_calls in one response) |
| Mode | serial, blocking, fixed role pool |

### 11.6 Built-in and Example Skills

| Skill | Type | Location |
|------|------|------|
| `bigbang_debate` | built-in | seeded by `ensureDefaultSkills` |
| `skill_maker` | built-in | seeded by `skills_builtin.cpp` (the creator builds skills with `write_file`, the verifier validates read-only) |
| `monica` | example (can stand alone as a repo) | repo `skills/monica/`; copy into `projv_files/skills/` and it works |

### 11.7 Tests (runnable automatically)

`tests/unit/`: bigbang comparison (test_skill_runner), swarm (test_skill_swarm), engine capabilities (test_skill_engine), skill_maker (test_skill_maker), monica (test_skill_monica). **79 cases / 370 assertions** total, all green.

### 11.8 Explicit "Not Doing" (continuing 2)

- **Automatic deletion** of dead code: not done (LLM call graphs are unreliable); `monica`'s auditor only **reports**.
- Engine-level `foreach`/dynamic file sets: not done (handled by the supervisor's round loop + request_agent).

