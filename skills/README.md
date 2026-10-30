# skills -- installable proJV skills (example source)

Each subfolder here is a **self-contained skill**: a `config.json`, one `.md`
per agent, and a `tools/` folder with the verb tool definitions. The engine
treats every folder the same -- there are no built-in skills and no special
cases in code.

## Install / uninstall

The app loads skills from:

    {exeDir}/projv_files/skills/

So installing a skill is just **copying its folder there**:

    skills\bigbang  ->  projv_files\skills\bigbang
    skills\monica   ->  projv_files\skills\monica

Uninstall = delete the folder. That is the whole story.

## Build-time deploy

The build already copies this whole `skills/` directory next to the built exe
(`<build>/.../projv_files/skills/`) after linking -- see the `POST_BUILD` step
in `CMakeLists.txt`. So a freshly built binary can run the examples right away,
and any release that ships `projv_files/` carries them automatically.

## Run

    /skill list                 # lists installed skills
    /skill bigbang <topic>
    /skill monica <project-root>

## Write your own

A skill is a folder, not code: `config.json` + one `.md` prompt per agent +
`tools/*.json`. The complete authoring reference - every config key, the
template variables and the traps, the driver rule, the two modes, two worked
examples and a pre-flight checklist - is
[docs/en/dev-guide/skills-authoring.md](../docs/en/dev-guide/skills-authoring.md)
(mirror: [docs/zh/dev-guide/skills-authoring.md](../docs/zh/dev-guide/skills-authoring.md)).
It is written so an agent can produce a working skill without reading the C++.

## Skills in this folder

| Folder | What it does |
|--------|--------------|
| `bigbang/` | three-role engineering debate (propose / integrate / vote) -> one execution doc |
| `monica/`  | supervisor-driven project tidy-up: format, translate comments, audit dead code |
| `avenger/` | SOP 1/3: market research swarm - Stark dispatches parallel research lines, audits them, writes `docs/sop/avenger_<slug>.md` |
| `steve/`   | SOP 2/3: multi-expert product design - script mode, no driver, seven roles vote and `all_agree` is engine-enforced |
| `musk/`    | SOP 3/3: kanban execution - Musk pairs dev x test against a one-line DoD and judges on quoted evidence |
