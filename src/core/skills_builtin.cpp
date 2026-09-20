// SKILL_ENGINE -- built-in example skills. Content lives here as constants and
// is written under projv_files/skills/ on first run (mirrors ensureDefaultPrompts).
#include "skills_builtin.h"
#include "debug_log.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
void writeIfMissing(const std::string& path, const std::string& content) {
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    std::ofstream ofs(path, std::ios::binary);
    if (ofs) ofs << content;
}
} // namespace

// ---- skill_maker ---------------------------------------------------------
static const char* SKILL_MAKER_CONFIG = R"JSON({
  "name": "skill_maker",
  "description": "Author a new skill (config + prompts + tools) and verify it.",
  "tools": ["read_file", "write_file", "edit_file", "md_file", "grep_files", "file_search", "exec_shell"],
  "tool_mode": "auto",
  "max_tool_iters": 80,
  "max_rounds": 1,
  "converge": "none",
  "agents": [
    {"id": "creator",  "display_name": "Creator",  "prompt": "creator.md"},
    {"id": "verifier", "display_name": "Verifier", "prompt": "verifier.md"}
  ],
  "round_steps": [
    {"actions": [
      {"agent": "creator", "tool": "emit", "capture": "summary", "into": "draft", "emit": "message",
       "message": "Skills directory: {{skills_dir}}\n\nUser request:\n{{topic}}\n\nCreate the skill now: write config.json, one .md per agent, and tools/<verb>.json for every verb tool. Then call emit(summary)."},
      {"agent": "verifier", "tool": "emit", "capture": "summary", "into": "verify", "emit": "message",
       "message": "A skill was just written under {{skills_dir}}. Verify it (read-only) and call emit with OK or PROBLEM."}
    ]}
  ]
}
)JSON";

static const char* SKILL_MAKER_CREATOR_MD = R"MD(You are the Skill Creator for proJV. A "skill" is a directory that turns a
multi-agent workflow into data. You produce files. You do not run the skill.

## Where files go
Write every file under:  <skills_dir>/<skill_name>/
  config.json
  <agent>.md            (one system prompt per agent)
  tools/<verb>.json     (one file per verb tool used)

## config.json schema
{
  "name": "<skill_name>",
  "description": "one line",
  "tools": ["read_file", "write_file", "exec_shell", "grep_files"],
  "tool_mode": "auto",
  "max_tool_iters": 40,
  "max_rounds": 1,
  "converge": "none",
  "agents": [{"id": "a1", "display_name": "A1", "prompt": "a1.md"}],
  "round_steps": [
    {"parallel": false, "actions": [
      {"agent": "a1", "tool": "<verb>", "capture": "<field>", "into": "<slot>",
       "emit": "message", "message": "text with {{topic}} and {{<slot>.current}}"}
    ]}
  ],
  "on_converge": []
}

Rules:
- Each action "tool" MUST match a tools/<verb>.json you write.
- capture = field of the verb args to store; into = blackboard slot name.
- emit is "" | "message" | "vote" | "document".
- Template vars: {{topic}} {{round}} {{max_rounds}} {{round_outputs}}
  {{<slot>.current}} {{<slot>.prev}}.
- Use "when": "round==1" / "round>1" to branch by round.

## tools/<verb>.json schema
{"name": "<verb>", "description": "...",
 "parameters": [{"name": "<field>", "type": "string", "description": "...", "required": true}]}

## Hard Rules
- Output ONLY with write_file / edit_file.
- Valid JSON only: no comments, no trailing commas.
- Every action "tool" has a matching tools/<verb>.json; every agent "prompt" has a matching .md.
- ASCII + English inside JSON/code. No emoji.
- When finished, call emit with a one-line summary that lists the files you wrote.
)MD";

static const char* SKILL_MAKER_VERIFIER_MD = R"MD(You are the Skill Verifier. A skill was just written under <skills_dir>/<name>/.
Validate it READ-ONLY. Do not modify anything.

Steps:
1. read_file the config.json. Confirm it is valid JSON and has name/agents/round_steps.
2. Confirm every action "tool" has a tools/<verb>.json, and every agent "prompt" has a .md file.
3. Run exec_shell to JSON-parse the config, e.g.:
   python -c "import json;json.load(open(r'<path>/config.json'));print('ok')"
4. Call emit with a short verdict: "OK: <name>" or "PROBLEM: <details>".
)MD";

static const char* SKILL_MAKER_EMIT_JSON = R"JSON({
  "name": "emit",
  "description": "Report a short summary and finish this turn.",
  "parameters": [{"name": "summary", "type": "string", "description": "short summary", "required": true}]
}
)JSON";

void seedBuiltinSkills(const std::string& skillsDir) {
    std::error_code ec;
    fs::create_directories(skillsDir, ec);

    std::string sd = skillsDir + "/skill_maker";
    fs::create_directories(sd + "/tools", ec);
    writeIfMissing(sd + "/config.json", SKILL_MAKER_CONFIG);
    writeIfMissing(sd + "/creator.md", SKILL_MAKER_CREATOR_MD);
    writeIfMissing(sd + "/verifier.md", SKILL_MAKER_VERIFIER_MD);
    writeIfMissing(sd + "/tools/emit.json", SKILL_MAKER_EMIT_JSON);
}
