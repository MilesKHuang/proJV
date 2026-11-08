# bigbang -- three-role debate on any topic (example skill)

A self-contained multi-agent skill for the proJV skill engine. Three roles
(Sheldon = structural reviewer, Penny = delivery reviewer, Leonard = lead)
propose, integrate, vote, and converge into one plan document.

The topic does not have to be software. The rules are written so that "file path
/ class / interface" reads as "part / step / material / person": a hardware
enclosure, a course outline, a shop opening plan and a refactor all go through
the same three reviewers. Output opens with five lines a new graduate can follow,
then the detail.

## Numbers carry sources

Both reviewers bring their own numbers, independently, and every number carries
value | source | date - a URL, a file path, or the exact table that was read. A
number nobody can source is written as a range and marked `[estimate]`, never as
a bare precise figure. The two reviewers attack each other's provenance (no
date / vendor blog / one country / stale), not with a new unsourced number.
Leonard audits the numbers before deciding: a decision-critical number with no
source is a `[high]` concern and forces a no vote.

The plan document therefore contains a numbers table (Number | Value | Source |
Date | Used for | Brought by) and a risk table (Risk | Trigger | Severity |
Immediate Fix | Long-term Fix) where every trigger is a sourced number or a
marked range.

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
- bigbang does not read an avenger report. It is meant to be runnable on its own,
  from a topic sentence and nothing else.
- `max_rounds` and other fields are plain config; edit the JSON to tune.
