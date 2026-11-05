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
only bought a narrower tool list while costing a phase-switching habit. The old
files stay on disk for existing installs, but they no longer appear in the cycle.

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
  read-only role must list its tools.
- **Behaviour**: what to read first, the shape of the answer, what not to touch.

## What the role does *not* control

- **Role colours** come from the active theme (`speaker_colors`), not from the
  prompt file.

---

Next -> [04 images](04-images.md) | Want to change it -> [prompts/seeding](../dev-guide/module-map.md)
