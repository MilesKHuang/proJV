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

## Skills in this folder

| Folder | What it does |
|--------|--------------|
| `bigbang/` | three-role engineering debate (propose / integrate / vote) -> one execution doc |
| `monica/`  | supervisor-driven project tidy-up: format, translate comments, audit dead code |
