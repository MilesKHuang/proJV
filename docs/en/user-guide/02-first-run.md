# 02 - First run

> After this page you have sent a message, approved a tool call, and know how
> to stop a run.

## 1. Enter your API key

1. Get a key at [platform.deepseek.com/api_keys](https://platform.deepseek.com/api_keys).
2. Start `proJV_tui`. If no key is configured, the configuration dialog opens.
3. Paste the key and press **Save & Connect**.

The key is stored in `projv_files/config.toml` (`api_key = "..."`). That file
is git-ignored and deliberately excluded from release packages.

## 2. Set the workspace (optional but recommended)

The workspace is the only directory the file tools may touch.

- `/workspace` prints the current one.
- `F2` (configuration) edits it.
- Empty means "the directory the executable sits in".

Set it to the project you want the agent to work on, not to your home folder.

## 3. Send a message

Type into the input box and press `Enter`. Example:

```
Read src/main.cpp and summarise what it does
```

You will see, in order: a spinner, the streaming answer (with a live Markdown
renderer), and tool calls as they happen.

## 4. Approve a tool call

Destructive tools (writing files, running commands) ask for confirmation. The
dialog shows the tool name and its arguments.

- Approve -> the tool runs and the result goes back to the model.
- Reject -> the model is told the call was rejected and adapts.

## 5. Stop a run

- Press `Esc` while it is busy: the turn is cancelled, the current tool call
  is interrupted, and the app stays open.
- Press `Esc` while idle: quits.
- `Ctrl+C` quits at any time; the session database is checkpointed and closed.

## 6. Where to look

| Key | Panel |
|-----|-------|
| `F8` | reasoning / thinking chain (collapsed summary -> full window) |
| `F9` | TODO panel |
| `F2` | configuration |
| `F5` | session history |

## Check it worked

- Your message is in the chat, an answer follows it.
- The status bar shows model, role, token counts, cost estimate and context
  pressure.

## If it does not work

| Symptom | Cause |
|---------|-------|
| "does not accept image input" on `@path` | images need the Flash line; switch to `deepseek-v4-flash` (`F10`) |
| every tool call is rejected with "outside the workspace" | workspace is set to a parent folder that does not contain the file |
| no answer, spinner forever | network / key problem; check `projv_files/logs` |

---

Next -> [03 roles](03-roles.md) | Want to change it -> [API client](../dev-guide/module-map.md)
