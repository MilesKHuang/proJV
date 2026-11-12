# Security model

> What proJV can and cannot touch. Written down because "the file tools are
> path-checked" is easy to mistake for a sandbox. It is not one.

## What is bounded

- **File tools** (`read_file`, `write_file`, `edit_file`, `md_file`,
  `format_file`, `file_search`, `grep_files`) go through `ToolPaths`
  (`src/tools/tool_paths.cpp`): two writable roots (the workspace and
  `projv_files/`), plus `projv_files/sessions/` and `projv_files/logs/` as
  read-only children. A path outside those roots is refused with
  `__WORKSPACE_VIOLATION__` and never reaches the filesystem.
- **Shell commands that delete or overwrite.** `exec_shell` is classified
  before it runs (`src/core/delete_guard.cpp`):
  - `Ask` -- deletes (`rm`, `del`, `rmdir`, `Remove-Item`), overwrites (`mv`,
    `cp`, shell redirects to a real file), git destruction (`git clean`,
    `reset --hard`, `checkout --`, `stash drop`), inline interpreter code
    (`python -c`, `node -e`). One dialog per exact command text; "always"
    applies to that command only, not to the session.
  - `Deny` -- disk formatting and partitioning (`format`, `diskpart`, `mkfs.*`,
    `fdisk`, `parted`, `wipefs`, `sgdisk`, `dd of=/dev/*`). No dialog, no
    override.
  - Everything else runs without asking.

## What is not bounded

- **`exec_shell` is a shell.** After approval it can read and write any path
  the user can, reach the network, and start arbitrary programs. The
  classifier above only gates commands it can recognise; a shell script that
  deletes files at runtime is not inspected.
- **Skills and pytools** run with the same privileges. A skill whose
  `config.json` grants `exec_shell` has the full power of the shell. A pytool
  is a Python process started by the binary.
- **MCP servers** are external processes spoken to over stdio JSON-RPC. They
  run as the current user and are trusted once configured in
  `projv_files/mcp.json`.
- **There is no OS sandbox.** No seatbelt, no bubblewrap, no container, no
  seccomp. The Windows Job Object used by the process runner
  (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) is lifetime management, not isolation.

## If you need real isolation

Run proJV inside a container, mounting only the project:

```bash
docker run --rm -it \
  -v "$PWD:/work" -w /work \
  -v "$HOME/.config/projv:/opt/projv" \
  --network none \
  ghcr.io/example/base:latest /work/projv
```

Two rules that matter more than the image: do not mount `~/.ssh`, and pass the
API key through an env var or a mounted config file rather than baking it into
the image.

## What is stored on disk

| Data | Where | Protection |
|---|---|---|
| API key | `projv_files/config.toml` | plain text |
| Sessions | `projv_files/sessions/*.db` | plain SQLite, unencrypted |
| Logs | `projv_files/logs/proJV-YYYY-MM-DD.log` | plain text, 7 days |
| Crash dumps | `projv_files/logs/crash-*.*` | plain text + minidump |
| Skills / pytools / MCP config | `projv_files/` | plain text, user-editable |

Nothing leaves the machine except the API requests you make. There is no
telemetry.
