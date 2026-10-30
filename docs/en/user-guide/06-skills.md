# 06 - Skills

> After this page you can run a multi-agent skill, read the sidebar, and author
> your own skill folder.

## What a skill is

A skill is a folder of data, not code. Drop it into `projv_files/skills/` and it
becomes runnable; nothing is recompiled.

```
projv_files/skills/monica/
├── config.json        agents, round_steps, convergence
├── supervisor.md      one system prompt per agent
├── worker.md
└── tools/emit.json    the verb tools the agents may call
```

## Run one

```
/skill list               # what is installed
/monica D:\ws\myproject   # run "monica" on a project
/bigbang add a tile cache # three-role debate -> one execution doc
```

The skill name doubles as the command. `Esc` stops it.

## The two examples

### bigbang

Three roles debate a topic until they all agree, then produce one execution
document. Thinking mode stays on; the debate is the product.

### monica

Project tidy-up for 50-200 file repositories:

1. the **supervisor** lists the tree, splits it into batches of 6-10 coupled
   files and dispatches several batches **in one reply**;
2. **worker_1 .. worker_10** run concurrently, each on its own files: format,
   ASCII / full-width cleanup, Chinese comments translated to English, and a
   read-only dead-code audit;
3. the supervisor verifies the reports, re-dispatches failures, then writes the
   consolidated report.

Dead code is only ever reported. Workers have no shell; formatting goes through
`format_file` (clang-format when installed, a whitespace pass otherwise).

### The SOP pipeline: avenger -> steve -> musk

Three skills that chain into one commercialisation pipeline, and they show the
two ways a skill can be orchestrated:

1. **avenger** - market research. Stark is the only driver: it dispatches the
   market / users / open-source lines in parallel, may wake the optional lines
   (tech / compliance / money), audits every report, and stops on `DONE`. Writes
   `docs/sop/avenger_<slug>.md`.
2. **steve** - product design. No driver at all: seven roles sit in the script
   (four designers -> PM -> QA -> everyone votes) and the **engine** runs it.
   `converge=all_agree` means unanimity is counted, not claimed; one dissent
   sends the plan back to PM. Writes `docs/sop/steve_<slug>.md`.
3. **musk** - execution. Musk drives dev x test pairs against a one-line DoD
   each, judges on quoted evidence, escalates after three returns. Writes
   `docs/sop/musk_<slug>.md`.

    /avenger e-invoice archiving for small shops
    /steve   e-invoice archiving for small shops
    /musk    build docs\sop\steve_e-invoice-archiving.md in D:\ws\sop-test

Rule of thumb: a fixed cast that must vote unanimously -> let the engine
orchestrate (steve, bigbang); a cast that must be picked at runtime -> one
driver (monica, avenger, musk).

## The agent sidebar

While a skill runs, the right-hand panel lists every role with its status,
message count and whether it is currently busy. Workers light up in parallel -
that is how you can tell dispatch really is concurrent.

## Author your own

```
/skill skill_maker a skill that reviews a PR diff
```

`skill_maker` is compiled into the binary. It writes `config.json`, the prompts
and the verb tools, then verifies the skill loads. Alternatively, copy
`skills/monica/` and edit.

Keep in mind:

- `tools` is **skill-wide**, every agent sees the same capability tools;
- only agents that appear in `round_steps` / `on_converge` may dispatch
  (`request_agent`); workers never get it;
- an illegal config value fails loudly, an unknown key is reported as a
  warning in the chat.

---

Next -> [07 pytool](07-pytool.md) | Want to change it -> [skill engine](../dev-guide/module-map.md)
