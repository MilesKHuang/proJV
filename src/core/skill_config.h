// SKILL_ENGINE -- skill DSL types (see SKILL_ENGINE.md v1.1).
#pragma once
#include <map>
#include <string>
#include <vector>

// Structured vote (shape == "vote").
#ifndef PROJV_VOTERESULT_DEFINED
#define PROJV_VOTERESULT_DEFINED
struct VoteResult {
    bool agree = false;
    std::vector<std::string> agreedPoints;
    std::vector<std::string> concerns;      // each "[severity] issue"
    std::string suggestedTweak;
};
#endif

struct SkillAgent {
    std::string id;
    std::string displayName;
    std::string promptFile;   // path relative to the skill directory
    std::string thinking;     // "" = inherit the skill-level setting, else "on" | "off"
};

struct SkillAction {
    std::string agent;
    std::string tool;         // verb tool name (from tools/*.json)
    std::string message;      // template
    std::string capture;      // field to extract (shape == "text")
    std::string into;         // text slot; empty -> resolved to agent id
    std::string shape = "text";   // "text" | "vote"
    std::string emit;             // "" | message | vote | document
    std::map<std::string, std::string> vars;
};

struct SkillStep {
    std::string when;         // "" | round==1 | round>1
    bool parallel = false;
    std::string progress;
    std::vector<SkillAction> actions;
};

struct SkillConfig {
    std::string name;
    std::string description;
    std::vector<std::string> tools;      // capability-tool whitelist
    std::vector<SkillAgent> agents;
    int maxRounds = 8;
    std::string converge = "none";       // "all_agree" | "none"
    std::string loopDetect;              // text slot; "" = off
    bool dynamicDispatch = false;
    int maxParallel = 16;                // concurrent request_agent groups (default 16)
    std::string dispatchTool;            // verb used for request_agent targets ("" = first tool)
    int maxToolIters = 3;                // tool-call iterations per turn (default preserves old behavior)
    // Keep the last N iterations free for the submission tool: when a turn gets
    // close to its budget, the agent is told to stop exploring and call the verb.
    // 0 (default) == off, which is byte-for-byte today's behaviour. Skills that
    // hand out big batches (monica) turn it on; short ones (bigbang, 3 iters)
    // must leave it off or the first iteration is already "nearly exhausted".
    int submitReserve = 0;
    std::string toolMode = "verb";       // "verb" (force the action tool) | "auto" (model may use any tool)
    std::string stopWhenSlot;            // optional: stop the round loop when stopWhenSlot == stopWhenValue
    std::string stopWhenValue;
    std::vector<SkillStep> roundSteps;
    std::vector<SkillStep> onConverge;

    // "on" (default) | "off". Role-level agents[i].thinking overrides this.
    std::string thinking = "on";

    // Non-fatal parse findings (unknown keys, legacy keys). Fill only; the
    // runner emits them as "system" events so typos are visible instead of
    // silently falling back to a default.
    std::vector<std::string> warnings;

    // Parse config.json. Resolves each action's empty `into` to its agent id.
    // Returns false and sets `err` on read/parse error.
    static bool loadFromJson(const std::string& path, SkillConfig& out, std::string& err);

    // Role-level thinking overrides the skill level; empty means "inherit".
    // Anything other than "off" is on (on is the API default).
    static bool thinkingEnabled(const std::string& skillLevel, const std::string& agentLevel);};
