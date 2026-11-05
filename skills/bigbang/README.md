# bigbang -- three-role debate on any topic (example skill)

A self-contained multi-agent skill for the proJV skill engine. Three roles
(Sheldon = structural reviewer, Penny = delivery reviewer, Leonard = lead)
propose, integrate, vote, and converge into one plan document.

The topic does not have to be software. The rules are written so that "file path
/ class / interface" reads as "part / step / material / person": a hardware
enclosure, a course outline, a shop opening plan and a refactor all go through
the same three reviewers. Output opens with five lines a new graduate can follow,
then the detail.

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
    /skill bigbang add git diff to proJV
    /skill bigbang plan a four-week pop-up shop for a coffee roaster

Uninstall = delete the `bigbang/` folder. Nothing is seeded automatically.

## Notes

- `config.json` `name` must equal the folder name (`bigbang`); the engine
  resolves prompts and `tools/` under `skills/<name>/`.
- `tools` carries `web_search` and `fetch_url`, so a topic outside the workspace
  can still be researched. A claim must come from a file read this round or a
  source fetched this round; anything else is marked `[assumed]`.
- `max_rounds` and other fields are plain config; edit the JSON to tune.
