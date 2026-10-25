# 03 - Roles

> After this page you can switch roles and write your own.

## What a role is

A role is a system prompt: a `.md` file under `projv_files/prompts/`. The file
name is the role name. Four are seeded on first launch:

| Role | Use it for |
|------|-----------|
| `coder` | default. All tools: read/write/edit, shell, search, web, todo, skills |
| `designer` | read-only analysis; write tools are locked out, outputs design docs with `md_file` |
| `analyzer` | code analysis, uses `diagram_tool` for diagrams |
| `compactor` | internal: how early conversation is summarised at context pressure |

## Switch mid-conversation

Press `Tab` to cycle. Context is kept: switching role changes the system prompt,
not the conversation.

## Write your own

1. Create `projv_files/prompts/reviewer.md`.
2. Write the instructions in plain Markdown. Be concrete about the output shape
   you want, and about what the role must not do.
3. Press `Tab` (or restart) - `reviewer` now appears in the cycle.

Minimal skeleton:

```markdown
You are a code reviewer. Report only; never edit files.

## Steps
1. read_file the target.
2. List findings as `file:line - severity - issue`.
3. End with the single most important fix.
```

## What the role does *not* control

- **Tools** are a skill-level setting, not a per-role setting. The main agent
  gets the full tool set; the role prompt can only tell it what to prefer.
- **Role colours** come from the active theme (`speaker_colors`), not from the
  prompt file.

---

Next -> [04 images](04-images.md) | Want to change it -> [prompts/seeding](../dev-guide/module-map.md)
