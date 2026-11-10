# 03 - Roles

> After this page you can switch roles and write your own.

## What a role is

A role is a system prompt: a `.md` file under `projv_files/prompts/`. The file
name is the role name. One role is seeded on first launch:

| Role | Use it for |
|------|-----------|
| `assistant` | default, and for almost everything. Plans with `update_todo`, writes code and design documents, and holds the full tool set |
| `compactor` | internal: how early conversation is summarised at context pressure (never in the `Tab` cycle) |

Earlier releases seeded three roles - `coder`, `designer`, `analyzer`. They were
merged into `assistant`: the plan lives in the TODO list, so a second prompt file
only bought a narrower tool list while costing a phase-switching habit. Nothing
is reserved: an install that still has those files keeps them as ordinary roles,
the file name is the role name, and `Tab` shows them. Delete a file you do not
want in the cycle - only `compactor.md` is special (it is the `/compress` prompt,
so it is never a role).

## Switch mid-conversation

Press `Tab` to cycle. Context is kept: switching role changes the system prompt,
not the conversation. Your own role files join the same cycle.

## Write your own

1. Create `projv_files/prompts/reviewer.md`.
2. Write the instructions in plain Markdown. Be concrete about the output shape
   you want, and about what the role must not do.
3. List what it may use on one line: `tools available: read_file, grep_files`.
4. Press `Tab` (or restart) - `reviewer` now appears in the cycle.

Minimal skeleton:

```markdown
You are a code reviewer. Report only; never edit files.

tools available: read_file, grep_files

## Steps
1. read_file the target.
2. List findings as `file:line - severity - issue`.
3. End with the single most important fix.
```

## What the role does control

- **Tools.** The `tools available:` line is a permission table, not a hint: the
  agent only offers those tools to the model, and a call to anything else is
  rejected. Leave the line out and the role gets **every** tool (fail-open), so a
  read-only role must list its tools. The seeded `assistant.md` deliberately leaves
  it out: one list in the prompt was already out of date, so the inventory now
  lives in the per-turn capability map instead.
- **What the role may touch.** Every role runs with the same filesystem rights:
  read+write in the workspace and in `projv_files` (prompts, skills, theme, pytool,
  `mcp.json`), read-only in `projv_files/sessions` and `projv_files/logs`, refused
  everywhere else. A role can therefore create its own successor: "add a skill" or
  "add this MCP server" are ordinary file writes, not special commands.
- **Behaviour**: what to read first, the shape of the answer, what not to touch.

A role's prompt is message **P0** of the conversation. The environment block
(P1) and the project block (P2) follow it, and the capability map (P3) is rebuilt
before every turn -- which is why adding a skill or an MCP server does not need a
new role file.

## When an edit takes effect

| You changed | Takes effect |
|-------------|--------------|
| `projv_files/prompts/*.md` | Next `F3` (new chat) or `/clear` |
| `projv_files/skills/<name>/` | Next `run_skill` call, no restart |
| `projv_files/mcp.json` | Your next message |
| `projv_files/pytool/<name>/` | After a config save (`F2`) or a restart |
| `projv_files/theme/*.json` | After a restart (the theme menu is built at start-up) |

## What the role does *not* control

- **Role colours** come from the active theme (`speaker_colors`), not from the
  prompt file.

---

Next -> [04 images](04-images.md) | Want to change it -> [prompts/seeding](../dev-guide/module-map.md)
