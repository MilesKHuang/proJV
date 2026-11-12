# v0.7.5 - reversible by default

Two milestones landed in one release: the guard rails, and turn snapshots. The theme
is simple -- proJV now either refuses what it cannot undo, or makes it undoable.

## TLS verification is on, and there is no switch

Every curl handle was created with `VERIFYPEER=0` / `VERIFYHOST=0`. That means anyone
who could answer DNS for `api.deepseek.com` received the bearer token and the code in
your workspace. Both lines are gone; libcurl's strict defaults apply.

A certificate failure now says what happened, why it usually happens (a proxy that
re-signs traffic, an expired or self-signed certificate, a wrong clock) and how to fix
it (install that root CA in the OS trust store). It never suggests turning verification
off, because there is no such option. The raw curl string goes to the log instead of
the screen.

## A shell command that deletes or overwrites asks first

`exec_shell` is classified before it runs (`src/core/delete_guard.cpp`):

- deletes (`rm`, `del`, `rmdir`, `Remove-Item`), overwrites (`mv`/`cp` onto an existing
  file, `>` to a real file), git destruction (`git clean`, `reset --hard`,
  `checkout --`, `stash drop`) and inline interpreter code (`python -c`, `node -e`) ask
  once;
- "Always" is bound to that exact command text, not to the session;
- "No" drops the rest of the batch and tells the model; the turn continues;
- wrappers are peeled (`cmd /c`, `sh -c`, `sudo`, `powershell -c`), quotes are
  respected, and a command nested too deeply asks instead of guessing.

## Disk formatting is refused, not asked

`format`, `diskpart`, `Format-Volume`, `mkfs.*`, `fdisk`, `parted`, `wipefs`, `sgdisk`,
`dd of=/dev/*` and `diskutil eraseDisk` are blocked outright: a partition table is not
something a backup restores, so there is nothing to ask. Matching is on the command
token, so `clang-format`, `git format-patch` and `npm run format` are untouched.

`shutdown` and `taskkill` no longer prompt. That is deliberate: a process restarts, a
partition does not.

## Release builds write logs

`PROJV_RELEASE` used to compile every logging macro to nothing, so a user report had no
evidence behind it. Release now opens `projv_files/logs/proJV-YYYY-MM-DD.log` at WARNING
and keeps seven days; debug writes everything. About fifty failure paths were
reclassified by hand so that the release log is not empty.

The feared formatting cost was unfounded: loguru checks the verbosity cutoff before it
evaluates arguments, so an INFO call in release costs one integer comparison.

## Crashes leave a dump

`InstallCrashHandler()` existed but was never called. It is installed at startup now and
writes `crash-<timestamp>.log` and `.dmp` next to the daily logs. The handler itself uses
only Win32 calls (or `open`+`write` on Linux) and a static buffer -- iostreams and
`std::string` inside a crash callback are how you get a second crash.

## `projv --version` and `--help`

They run before the TUI, print, and exit 0. `--help` wins over `--install`, so
`projv --install --help` no longer starts the installer.

## `/undo` and `/restore N`

Every turn takes a snapshot: a stat-only scan when it starts, a stat-only scan when it
ends, and only files whose size or mtime moved are read, hashed (SHA-256) and stored as
content-addressed blobs. No git, no git metadata in your workspace, works in a directory
that has never seen git.

- `/undo` restores what the last turn changed: modified content is written back, files
  the turn created are deleted, files it deleted come back.
- If a file changed after that turn, `/undo` refuses and touches nothing; `/undo!`
  discards those manual edits too.
- `/restore N confirm` rolls the workspace back to before turn N, reverting every later
  turn as well.
- The result arrives as a `[Snapshot]` message that both you and the model see, ending
  with "do not assume the files still contain them" -- otherwise the next reply keeps
  talking about edits that no longer exist.

Limits, stated plainly: files over 256 KB, binary files and build/VCS directories are
not tracked; writes made by `exec_shell` are caught by the end-of-turn scan, not
individually; edits you make while a turn is running are attributed to that turn.

## Configuration

```toml
snapshots_max_workspace_gb = 2   # 0 disables the gate
snapshots_keep_days = 7
```

A workspace over the gate disables undo with an explicit message instead of filling the
disk. Stores live in `projv_files/snapshots/<workspace-hash>/`.

## Upgrade notes

- Nothing to migrate. `projv_files/logs` and `projv_files/snapshots` are created on the
  first run.
- The old `<exeDir>/proJV.log` is no longer written; the daily file replaces it.
- The safety boundary is now written down, including what is *not* sandboxed:
  `docs/en/dev-guide/security-model.md` and `docs/zh/dev-guide/security-model.md`.
