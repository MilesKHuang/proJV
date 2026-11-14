// proJV -- the "/" command list (see slash_menu.h).
#include "tui/slash_menu.h"

#include "core/skill_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace slash_menu {
namespace {

constexpr size_t kDescMax = 48;

std::string clipDesc(std::string s) {
    // One line only: a menu row is one row.
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    if (s.size() > kDescMax) {
        s.resize(kDescMax);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
            s.pop_back();
        s += "...";
    }
    return s;
}

std::string toLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::vector<Command> builtins() {
    // One entry per handler that actually exists:
    //   handleQuickCommand (agent.cpp) -> /clear /help /undo /undo! /restore
    //   TuiApp::sendMessage            -> /compress /wish /end_task /workspace
    //   skill_trigger                  -> /skill list, /<skill> <topic>
    return {
        {"/help", "", "Show the command help in chat"},
        {"/clear", "", "Clear the session (keeps the session file)"},
        {"/compress", "", "Summarize early messages to free context"},
        {"/undo", "", "Undo the file changes of the last turn"},
        {"/undo!", "", "Undo, discarding later hand edits too"},
        {"/restore", "<turn> confirm", "Roll the workspace back to before turn N"},
        {"/wish", "<what you want built>",
         "Decide it yourself, keep going until the TODO is empty"},
        {"/wish cancel", "", "Stop the wish; the TODO list is kept"},
        {"/end_task", "<id> | all", "Stop a background task (kills the process tree)"},
        {"/approval", "", "Show the approval mode and the remembered commands"},
        {"/approval clear", "", "Forget every remembered command"},
        {"/workspace", "", "Print the current workspace path"},
        {"/skill", "list", "List installed skills"},
    };
}

std::vector<Command> all(const std::vector<std::string>& installedSkills,
                         const std::string& skillsDir) {
    std::vector<Command> out = builtins();
    std::vector<std::string> skills = installedSkills;
    std::sort(skills.begin(), skills.end());
    for (const auto& name : skills) {
        SkillConfig cfg;
        std::string err;
        std::string desc;
        if (SkillConfig::loadFromJson(skillsDir + "/" + name + "/config.json", cfg, err))
            desc = cfg.description;
        out.push_back({"/" + name, "<topic>", clipDesc(desc)});
    }
    return out;
}

std::vector<Command> filter(const std::vector<Command>& all, const std::string& input) {
    std::vector<Command> out;
    if (input.empty() || input[0] != '/') return out;
    if (input.find_first_of(" \t") != std::string::npos) return out;
    const std::string needle = toLower(input.substr(1));
    if (needle.empty()) {
        // Bare "/" lists everything: that is the whole point of the menu -- you do
        // not need to remember a prefix to discover a command.
        return all;
    }

    std::vector<Command> prefix, contains;
    for (const auto& c : all) {
        // Compare against the first word: "/wish cancel" is reachable by typing
        // "/wish", and must not shadow the plain "/wish" entry either.
        const std::string name = toLower(c.name);
        const std::string bare = name.substr(1);
        if (bare.rfind(needle, 0) == 0)
            prefix.push_back(c);
        else if (bare.find(needle) != std::string::npos)
            contains.push_back(c);
    }
    auto byName = [](const Command& a, const Command& b) { return a.name < b.name; };
    std::stable_sort(prefix.begin(), prefix.end(), byName);
    std::stable_sort(contains.begin(), contains.end(), byName);
    out = prefix;
    out.insert(out.end(), contains.begin(), contains.end());
    return out;
}

std::string completion(const Command& c) {
    if (c.args.empty()) return c.name;
    return c.name + " ";
}

} // namespace slash_menu
