# bigbang -- three-role engineering debate (example skill)

A self-contained multi-agent skill for the proJV skill engine. Three roles
(Sheldon = architecture reviewer, Penny = delivery reviewer, Leonard =
engineering lead) propose, integrate, vote, and converge into one execution
document.

Skills are just folders. This repo ships the folder; you install it by copying
it into your runtime skills directory.

## Install

Copy this folder into `<projv_files>/skills/` (the `skills/` directory next to
your proJV data / exe, i.e. the one `projv_files/skills/` points at):

    projv_files/
      skills/
        bigbang/
          config.json
          sheldon.md
          penny.md
          leonard.md
          tools/
            bigbang_turn.json
            bigbang_vote.json
            write_bigbang_doc.json

## Use

    /skill list                  # should list bigbang (and monica, if installed)
    /skill bigbang <topic>       # e.g. /skill bigbang add git diff to proJV

Uninstall = delete the `bigbang/` folder. Nothing is seeded automatically.

## Notes

- `config.json` `name` must equal the folder name (`bigbang`); the engine
  resolves prompts and `tools/` under `skills/<name>/`.
- `max_rounds` and other fields are plain config; edit the JSON to tune.
