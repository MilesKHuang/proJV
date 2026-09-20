// SKILL_ENGINE -- skill DSL types (see SKILL_ENGINE.md v1.1).
#pragma once
#include <map>
#include <string>
#include <vector>

// Structured vote (shape == "vote"). Shared with roundtable.h during the
// migration; the guard keeps both headers includable in the same TU.
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
    int maxRecursion = 3;
    std::vector<SkillStep> roundSteps;
    std::vector<SkillStep> onConverge;

    // Parse config.json. Resolves each action's empty `into` to its agent id.
    // Returns false and sets `err` on read/parse error.
    static bool loadFromJson(const std::string& path, SkillConfig& out, std::string& err);
};
