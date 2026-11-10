#include "core/context_assembler.h"

#include "core/skill_config.h"
#include "debug_log.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace ctx {

const char* const kEnvironmentPrefix = "[ENVIRONMENT]";
const char* const kProjectPrefix = "[PROJECT]";

namespace {

// -- The builtin table -------------------------------------------------------
// Group and hard limits for every tool compiled into the binary. Parameters are
// NOT repeated here: they live in the tool definitions of the same request, and a
// second copy would drift. A new builtin must add a row -- test_capabilities
// asserts that the table and the registry agree, so forgetting is a red test.
struct BuiltinInfo {
    const char* name;
    const char* group;
    const char* hint; // "" = no parenthetical
};

const BuiltinInfo kBuiltins[] = {
    {"read_file", "File",
     "whole <=100KB; start_line/end_line slice; >2MB refused, open those with the shell"},
    {"write_file", "File", "<=8KB per call, prefer edit_file for changes"},
    {"edit_file", "File", ""},
    {"list_dir", "File", "depth 1-6, default 2"},
    {"grep_files", "File", "100 hits default, 500 max"},
    {"file_search", "File", "<=50 paths"},
    {"format_file", "File", "clang-format, else a whitespace pass"},
    {"exec_shell", "Run",
     "timeout_ms default = max = 1800000; stdout 32000; silence is not failure"},
    {"web_search", "Web", "count 1-50, default 10, billed"},
    {"fetch_url", "Web", "one page as Markdown, 100KB"},
    {"md_file", "Docs", ".md only, 8KB per call, append supported"},
    {"update_todo", "Plan", "init / update / done -- the panel the user watches"},
    {"view_image", "Image", "<=8 images, 30MB each, ends the turn, Flash only"},
    {"run_skill", "Skill", "minutes, ONE per turn -- the call ends the turn"},
};

const BuiltinInfo* findBuiltin(const std::string& name) {
    for (const auto& b : kBuiltins) {
        if (name == b.name) return &b;
    }
    return nullptr;
}

// Fixed rendering order: a byte-stable block is what keeps the KV prefix cache
// hitting across turns.
const char* const kGroupOrder[] = {"File",  "Run",   "Web",  "Docs", "Plan",
                                   "Image", "Skill", "Tool", "Other"};

std::string firstSentence(const std::string& s, size_t maxLen) {
    std::string out;
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else {
            out.push_back(c);
        }
        if (out.size() >= maxLen) {
            out += "...";
            break;
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

bool allowedHas(const std::vector<std::string>& allowed, const std::string& name) {
    // An empty allowlist means "no restriction" -- the same rule the Agent applies
    // in isToolAllowed(), so P3 and req.tools cannot disagree.
    if (allowed.empty()) return true;
    return std::find(allowed.begin(), allowed.end(), name) != allowed.end();
}

// Emit "Label   item | item | item", wrapping at 96 columns with an 8-space
// continuation indent. Deterministic for a given ordered input.
void emitGroup(std::ostringstream& out, const std::string& label,
               const std::vector<std::string>& items) {
    if (items.empty()) return;
    std::string line = label;
    while (line.size() < 8)
        line += ' ';
    size_t col = line.size();
    std::string rendered = line;
    for (size_t i = 0; i < items.size(); ++i) {
        const std::string piece = (i == 0 ? std::string() : std::string(" | ")) + items[i];
        if (col + piece.size() > 96 && col > 8) {
            out << rendered << "\n";
            rendered = "        ";
            col = 8;
            rendered += items[i];
            col += items[i].size();
        } else {
            rendered += piece;
            col += piece.size();
        }
    }
    if (rendered.size() > 8) out << rendered << "\n";
}

std::string stub(const std::string& v, const char* fallback) {
    return v.empty() ? std::string(fallback) : v;
}

} // namespace

const char* groupOf(const std::string& toolName) {
    const BuiltinInfo* b = findBuiltin(toolName);
    if (b) return b->group;
    // A pytool's name comes from its tool.json, so a static table cannot know it.
    // "Tool" is the honest answer: it exists, the schema is in req.tools.
    return "Tool";
}

std::string buildEnvironment(const std::string& workspace, const std::string& projvFiles,
                             const std::string& shellCwd, const std::string& cppCompiler,
                             const std::string& python, const std::string& clangFormat) {
    std::ostringstream out;
    out << kEnvironmentPrefix << " Fixed for this session.\n";
    out << "workspace:    " << stub(workspace, "(not set -- the launch directory)") << "\n";
    out << "projv_files:  "
        << stub(projvFiles, "(not found next to the exe -- prompts, skills, theme and pytool "
                            "are unavailable)")
        << "\n";
    out << "shell cwd:    " << stub(shellCwd, "(the launch directory)") << "\n";

    // Always present, even when nothing was found: "toolchains: none" is
    // information, a missing line is a question mark.
    std::string tc;
    if (!cppCompiler.empty()) tc += "cpp=" + cppCompiler;
    if (!python.empty()) tc += (tc.empty() ? "" : " | ") + std::string("python=") + python;
    if (!clangFormat.empty())
        tc += (tc.empty() ? "" : " | ") + std::string("clang-format=") + clangFormat;
    out << "toolchains:   " << (tc.empty() ? "none (set them in F2 if a build is needed)" : tc)
        << "\n";

    out << "access:       read+write: the workspace";
    if (!projvFiles.empty())
        out << " and projv_files (prompts/, skills/, theme/, pytool/, mcp.json)";
    out << " | read only: projv_files/sessions and projv_files/logs | everything else is "
           "refused.\n";
    return out.str();
}

} // namespace ctx

namespace ctx {

namespace {

// Skill metadata comes from config.json; the name alone tells the model nothing
// about whether the workflow is worth its minutes.
struct SkillMeta {
    std::string description;
    int agents = 0;
    int rounds = 0;
    bool ok = false;
};

SkillMeta readSkillMeta(const std::string& skillsDir, const std::string& name) {
    SkillMeta m;
    if (skillsDir.empty()) return m;
    SkillConfig cfg;
    std::string err;
    if (!SkillConfig::loadFromJson(skillsDir + "/" + name + "/config.json", cfg, err)) return m;
    m.description = cfg.description;
    m.agents = static_cast<int>(cfg.agents.size());
    m.rounds = cfg.maxRounds;
    m.ok = true;
    return m;
}

std::string skillCast(const SkillMeta& m) {
    if (!m.ok) return std::string();
    std::string out;
    if (m.agents > 0) out += std::to_string(m.agents) + (m.agents == 1 ? " agent" : " agents");
    if (m.rounds > 0) {
        if (!out.empty()) out += ", ";
        out += std::to_string(m.rounds) + " rounds";
    }
    return out;
}

} // namespace

std::string buildCapabilities(const CapabilityInputs& in) {
    // -- 1. Filter and sort -------------------------------------------------
    // ToolRegistry stores definitions in an unordered_map, so the raw order changes
    // when MCP rehashes it. Sorting here is what makes two consecutive turns
    // byte-identical (V9) and keeps the prefix cache alive.
    std::vector<std::string> names;
    names.reserve(in.tools.size());
    for (const auto& d : in.tools) {
        if (d.name.rfind("mcp__", 0) == 0) continue; // rendered per server below
        if (!allowedHas(in.allowed, d.name)) continue;
        names.push_back(d.name);
    }
    std::sort(names.begin(), names.end());

    std::set<std::string> registered(names.begin(), names.end());
    std::vector<std::string> missing;
    for (const auto& a : in.allowed) {
        if (a.rfind("mcp__", 0) == 0) continue;
        if (!registered.count(a)) missing.push_back(a);
    }
    std::sort(missing.begin(), missing.end());

    std::map<std::string, std::vector<std::string>> byGroup;
    for (const auto& name : names) {
        const BuiltinInfo* b = findBuiltin(name);
        std::string item = name;
        if (b) {
            if (b->hint[0] != '\0') item += std::string(" (") + b->hint + ")";
            byGroup[b->group].push_back(item);
        } else {
            // A pytool (or anything registered from outside): group "Tool", with a
            // shortened description so it is not just an unexplained name.
            std::string desc;
            for (const auto& d : in.tools) {
                if (d.name == name) {
                    desc = firstSentence(d.description, 60);
                    break;
                }
            }
            if (!desc.empty()) item += " (" + desc + ")";
            byGroup["Tool"].push_back(item);
        }
    }
    for (const auto& m : missing)
        byGroup["Other"].push_back(m + " (not installed)");
    if (!missing.empty())
        debugLogf("[ctx] allowlist names %zu tool(s) that are not registered", missing.size());

    // -- 2. Render ----------------------------------------------------------
    std::ostringstream out;
    out << "[CAPABILITIES] Generated every turn from what is registered right now. This is a\n"
           "map, not a schema: parameter names and types live in the tool definitions of this\n"
           "request; this block only gives grouping, purpose and hard limits.\n";

    for (const char* g : kGroupOrder) {
        auto it = byGroup.find(g);
        if (it == byGroup.end() || it->second.empty()) continue;
        std::string label = g;
        if (label == "Other") label = "Missing"; // allowlist entries that do not exist
        emitGroup(out, label, it->second);
    }

    // -- 3. Skills ----------------------------------------------------------
    std::vector<std::string> skills = in.skills;
    std::sort(skills.begin(), skills.end());
    if (skills.empty()) {
        out << "Skills  none installed. A skill is a folder under projv_files/skills/ with a "
               "config.json; the user can ask for one to be written.\n";
    } else {
        out << "Skills  " << skills.size()
            << " installed. Each runs as its own phase, not inside this turn.\n";
        const bool full = skills.size() <= 3;
        const bool brief = skills.size() <= 6;
        for (const auto& s : skills) {
            const SkillMeta m = readSkillMeta(in.skillsDir, s);
            std::string line = "        - " + s;
            // Degrade by count: <=3 up to 200 chars, 4-6 one clipped line, >6 names
            // only. Never the raw description: an author may write a paragraph, and
            // three of those would crowd out the conversation.
            std::string desc;
            if (full)
                desc = firstSentence(m.description, 200);
            else if (brief)
                desc = firstSentence(m.description, 60);
            if (!desc.empty()) line += " -- " + desc;
            const std::string cast = skillCast(m);
            if (!cast.empty()) line += " | " + cast;
            out << line << "\n";
        }
        if (!brief)
            out << "        (descriptions omitted: " << skills.size()
                << " skills installed. Read projv_files/skills/<name>/config.json.)\n";
    }
    if (!in.skillsDir.empty())
        out << "        Authoring: " << in.skillsDir << "/_authoring/skill-authoring.md\n";

    // -- 4. MCP -------------------------------------------------------------
    if (in.mcp.empty()) {
        // One line, not a section: no server is harmless, but "how do I add one"
        // must be answerable from here.
        out << "MCP     none running. Servers live in projv_files/mcp.json (name + description + "
               "command); a saved file is reloaded before your next turn.\n";
    } else {
        out << "MCP     " << in.mcp.size()
            << " server(s); naming is mcp__<server>__<tool>, schemas are in the tool "
               "definitions.\n";
        for (const auto& s : in.mcp) {
            std::string line = "        - " + s.name;
            if (!s.description.empty()) line += " (" + firstSentence(s.description, 80) + ")";
            line +=
                ": " + std::to_string(s.tools.size()) + (s.tools.size() == 1 ? " tool" : " tools");
            if (!s.tools.empty()) {
                line += " --";
                for (const auto& t : s.tools)
                    line += " " + t;
            }
            out << line << "\n";
        }
        out << "        A server you need may be missing: read projv_files/mcp.json, say which "
               "entry to enable, or add it (with its \"description\"). It takes effect on the "
               "next message.\n";
    }

    if (!missing.empty())
        out << "Note    Entries marked (not installed) are named by this role's allowlist but do "
               "not exist; do not call them. Sources of truth: projv_files/skills/<name>/"
               "config.json, projv_files/pytool/<name>/tool.json, projv_files/mcp.json.\n";

    std::string text = out.str();
    // Hard ceiling: the map must never crowd out the conversation. Never silently:
    // a cut map says where the rest is.
    constexpr size_t kMaxCapabilityChars = 4000;
    if (text.size() > kMaxCapabilityChars) {
        size_t cut = text.rfind('\n', kMaxCapabilityChars);
        if (cut == std::string::npos) cut = kMaxCapabilityChars;
        text = text.substr(0, cut) +
               "\n[... capability map truncated. Read projv_files/mcp.json and the skill "
               "config.json files for the rest.]";
    }
    return text;
}

std::string describe(const std::string& block) {
    const size_t chars = block.size();
    const size_t tokens = (chars + 2) / 3; // ~3 chars per token, deliberately rough
    return std::to_string(chars) + " chars / ~" + std::to_string(tokens) + " tokens";
}

} // namespace ctx
