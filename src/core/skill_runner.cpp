// SKILL_ENGINE -- SkillRunner implementation (see SKILL_ENGINE.md v1.1).
#include "skill_runner.h"
#include "core/sub_agent.h"
#include "core/prompts.h"
#include "core/config.h"
#include "json.hpp"
#include "debug_log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::vector<std::string> strArr(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array())
        for (const auto& v : j[key]) if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

void writeIfMissing(const std::string& path, const std::string& content) {
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    std::ofstream ofs(path, std::ios::binary);
    if (ofs) ofs << content;
}

// Built-in verb tool defs for bigbang_debate.
const char* TOOL_TURN_JSON = R"JSON({"name":"bigbang_turn","description":"Statement of this turn. List up to 3 file paths in file_requests if you must read code first.","parameters":[{"name":"statement","type":"string","description":"This turn's statement or proposal","required":true},{"name":"file_requests","type":"array","description":"Up to 3 file paths to read first","required":false}]})JSON";
const char* TOOL_VOTE_JSON = R"JSON({"name":"bigbang_vote","description":"Vote on the current proposal. Only in the vote phase.","parameters":[{"name":"agree","type":"boolean","description":"Agree with the proposal","required":true},{"name":"agreed_points","type":"array","description":"Points you agree with","required":false},{"name":"concerns","type":"array","description":"Format: '[high|medium|low] issue'","required":false},{"name":"suggested_tweak","type":"string","description":"A concrete tweak, or 'none'","required":true}]})JSON";
const char* TOOL_DOC_JSON = R"JSON({"name":"write_bigbang_doc","description":"After convergence, emit the final execution document.","parameters":[{"name":"doc_markdown","type":"string","description":"The full execution document (Markdown)","required":true}]})JSON";

} // namespace

// ---- Built-in skill #1 config (bytes aligned with roundtable.cpp) --------
static const char* BIGBANG_CONFIG_JSON = R"JSON({
  "name": "bigbang_debate",
  "description": "Three-role engineering debate: propose, integrate, vote, emit an execution doc.",
  "tools": ["read_file", "grep_files", "file_search"],
  "agents": [
    {"id": "sheldon", "display_name": "Sheldon", "prompt": "sheldon.md"},
    {"id": "penny",   "display_name": "Penny",   "prompt": "penny.md"},
    {"id": "leonard", "display_name": "Leonard", "prompt": "leonard.md"}
  ],
  "max_rounds": 8,
  "converge": "all_agree",
  "loop_detect": "leonard",
  "round_steps": [
    {"when": "round==1", "parallel": true,
     "progress": "Round {{round}}/{{max_rounds}} - proposals (Sheldon + Penny)",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_turn", "capture": "statement", "into": "sheldon", "emit": "message",
        "message": "Topic: {{topic}}\n\nCall bigbang_turn with your proposal."},
       {"agent": "penny", "tool": "bigbang_turn", "capture": "statement", "into": "penny", "emit": "message",
        "message": "Topic: {{topic}}\n\nCall bigbang_turn with your proposal."}
     ]},
    {"when": "round>1", "parallel": true,
     "progress": "Round {{round}}/{{max_rounds}} - proposals (Sheldon + Penny)",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_turn", "capture": "statement", "into": "sheldon", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} ===\n\nLeonard compromise from previous round:\n{{leonard.prev}}\n\nPrevious vote results:\n{{round_context}}\n\nPenny's previous proposal:\n{{penny.prev}}\n\nCall bigbang_turn."},
       {"agent": "penny", "tool": "bigbang_turn", "capture": "statement", "into": "penny", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} ===\n\nLeonard compromise from previous round:\n{{leonard.prev}}\n\nPrevious vote results:\n{{round_context}}\n\nSheldon's previous proposal:\n{{sheldon.prev}}\n\nCall bigbang_turn."}
     ]},
    {"when": "round==1", "progress": "Round {{round}}/{{max_rounds}} - integration (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "bigbang_turn", "capture": "statement", "into": "leonard", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Integrate ===\n\nSheldon:\n{{sheldon.current}}\n\nPenny:\n{{penny.current}}\n\nCall bigbang_turn."}
     ]},
    {"when": "round>1", "progress": "Round {{round}}/{{max_rounds}} - integration (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "bigbang_turn", "capture": "statement", "into": "leonard", "emit": "message",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Integrate ===\n\nSheldon:\n{{sheldon.current}}\n\nPenny:\n{{penny.current}}\n\nPrevious vote results:\n{{round_context}}\n\nCall bigbang_turn."}
     ]},
    {"parallel": true, "progress": "Round {{round}}/{{max_rounds}} - vote",
     "actions": [
       {"agent": "sheldon", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."},
       {"agent": "penny", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."},
       {"agent": "leonard", "tool": "bigbang_vote", "shape": "vote", "emit": "vote",
        "message": "Topic: {{topic}}\n\n=== Round {{round}} Vote ===\n\nProposal under vote:\n{{leonard.current}}\n\nFor reference - Sheldon position:\n{{sheldon.current}}\n\nFor reference - Penny position:\n{{penny.current}}\n\nCall bigbang_vote."}
     ]}
  ],
  "on_converge": [
    {"progress": "Writing execution document (Leonard)",
     "actions": [
       {"agent": "leonard", "tool": "write_bigbang_doc", "capture": "doc_markdown", "emit": "document",
        "message": "Topic: {{topic}}\n\nFinal approved proposal:\n{{leonard.current}}\n\nResidual concerns:\n{{residual_concerns}}\n\nCall write_bigbang_doc."}
     ]}
  ]
}
)JSON";

// ---- SkillConfig::loadFromJson -------------------------------------------
bool SkillConfig::loadFromJson(const std::string& path, SkillConfig& out, std::string& err) {
    std::ifstream ifs(path);
    if (!ifs) { err = "cannot open skill config: " + path; return false; }
    nlohmann::json j;
    try { ifs >> j; } catch (const std::exception& e) {
        err = std::string("parse error: ") + e.what(); return false;
    }
    try {
        out.name = j.value("name", std::string(""));
        out.description = j.value("description", std::string(""));
        out.maxRounds = j.value("max_rounds", 8);
        out.converge = j.value("converge", std::string("none"));
        out.loopDetect = j.value("loop_detect", std::string(""));
        out.dynamicDispatch = j.value("dynamic_dispatch", false);
        out.maxRecursion = j.value("max_recursion", 3);
        if (j.contains("tools") && j["tools"].is_array())
            for (const auto& t : j["tools"]) if (t.is_string()) out.tools.push_back(t.get<std::string>());
        if (j.contains("agents") && j["agents"].is_array())
            for (const auto& a : j["agents"]) {
                SkillAgent ag;
                ag.id = a.value("id", std::string(""));
                ag.displayName = a.value("display_name", ag.id);
                ag.promptFile = a.value("prompt", std::string(""));
                out.agents.push_back(ag);
            }
        auto loadSteps = [](const nlohmann::json& arr, std::vector<SkillStep>& dst) {
            if (!arr.is_array()) return;
            for (const auto& s : arr) {
                SkillStep st;
                st.when = s.value("when", std::string(""));
                st.parallel = s.value("parallel", false);
                st.progress = s.value("progress", std::string(""));
                if (s.contains("actions") && s["actions"].is_array())
                    for (const auto& a : s["actions"]) {
                        SkillAction ac;
                        ac.agent = a.value("agent", std::string(""));
                        ac.tool = a.value("tool", std::string(""));
                        ac.message = a.value("message", std::string(""));
                        ac.capture = a.value("capture", std::string(""));
                        ac.into = a.value("into", std::string(""));
                        ac.shape = a.value("shape", std::string("text"));
                        ac.emit = a.value("emit", std::string(""));
                        if (a.contains("vars") && a["vars"].is_object())
                            for (auto it = a["vars"].begin(); it != a["vars"].end(); ++it)
                                if (it.value().is_string()) ac.vars[it.key()] = it.value().get<std::string>();
                        if (ac.into.empty()) ac.into = ac.agent;
                        st.actions.push_back(std::move(ac));
                    }
                dst.push_back(std::move(st));
            }
        };
        if (j.contains("round_steps")) loadSteps(j["round_steps"], out.roundSteps);
        if (j.contains("on_converge")) loadSteps(j["on_converge"], out.onConverge);
    } catch (const std::exception& e) { err = e.what(); return false; }
    if (out.name.empty()) { err = "skill has no name"; return false; }
    return true;
}

// ---- lifecycle -----------------------------------------------------------
SkillRunner::SkillRunner(AppConfig cfg, ToolRegistry* tools, Callbacks cbs,
                         std::string sharedContext)
    : cfg_(std::move(cfg)), tools_(tools), cbs_(std::move(cbs)),
      sharedContext_(std::move(sharedContext)) {
    skillsDir_ = getProjvDir() + "/skills";
}

SkillRunner::~SkillRunner() = default;

void SkillRunner::cancel() {
    cancelRequested_.store(true);
    for (auto& kv : agents_) if (kv.second) kv.second->cancel();
}

// ---- seeding / discovery -------------------------------------------------
void SkillRunner::ensureDefaultSkills(const std::string& skillsDir) {
    std::error_code ec;
    fs::create_directories(skillsDir, ec);
    std::string sdir = skillsDir + "/bigbang_debate";
    fs::create_directories(sdir, ec);
    writeIfMissing(sdir + "/config.json", BIGBANG_CONFIG_JSON);
    writeIfMissing(sdir + "/sheldon.md", loadBigbangPrompt("sheldon"));
    writeIfMissing(sdir + "/penny.md",   loadBigbangPrompt("penny"));
    writeIfMissing(sdir + "/leonard.md", loadBigbangPrompt("leonard"));
    std::string tdir = skillsDir + "/tools";
    fs::create_directories(tdir, ec);
    writeIfMissing(tdir + "/bigbang_turn.json", TOOL_TURN_JSON);
    writeIfMissing(tdir + "/bigbang_vote.json", TOOL_VOTE_JSON);
    writeIfMissing(tdir + "/write_bigbang_doc.json", TOOL_DOC_JSON);
}

std::vector<std::string> SkillRunner::listSkills(const std::string& skillsDir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(skillsDir, ec)) {
        if (ec) break;
        if (!e.is_directory()) continue;
        if (fs::exists(e.path() / "config.json")) out.push_back(e.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- agent pool ----------------------------------------------------------
void SkillRunner::loadAgents() {
    std::string sdir = skillsDir_ + "/" + config_.name;

    // Verb tools per agent: derived from the program (no can_write_doc needed).
    std::map<std::string, std::vector<std::string>> perAgent;
    auto scan = [&perAgent](const std::vector<SkillStep>& steps) {
        for (const auto& st : steps)
            for (const auto& a : st.actions) {
                auto& v = perAgent[a.agent];
                if (std::find(v.begin(), v.end(), a.tool) == v.end()) v.push_back(a.tool);
            }
    };
    scan(config_.roundSteps);
    scan(config_.onConverge);

    // Verb tool defs from tools/*.json.
    std::map<std::string, ToolDefinition> jsonDefs;
    {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(skillsDir_ + "/tools", ec)) {
            if (ec) break;
            if (!e.is_regular_file() || e.path().extension() != ".json") continue;
            std::ifstream f(e.path());
            if (!f) continue;
            try {
                auto j = nlohmann::json::parse(f);
                ToolDefinition d;
                d.name = j.value("name", std::string(""));
                d.description = j.value("description", std::string(""));
                if (j.contains("parameters") && j["parameters"].is_array())
                    for (const auto& p : j["parameters"]) {
                        ToolParameter tp;
                        tp.name = p.value("name", std::string(""));
                        tp.type = p.value("type", std::string("string"));
                        tp.description = p.value("description", std::string(""));
                        tp.required = p.value("required", false);
                        d.parameters.push_back(tp);
                    }
                if (!d.name.empty()) jsonDefs[d.name] = d;
            } catch (...) {}
        }
    }

    // Capability tool defs from the registry.
    std::map<std::string, ToolDefinition> regDefs;
    if (tools_)
        for (const auto& d : tools_->getToolDefinitions()) regDefs[d.name] = d;

    for (const auto& ag : config_.agents) {
        std::ifstream f(sdir + "/" + ag.promptFile, std::ios::binary);
        std::string prompt((std::istreambuf_iterator<char>(f)), {});

        auto sa = std::make_unique<SubAgent>(ag.displayName, prompt);
        sa->configure(cfg_, tools_, config_.tools, 2);
        sa->seedSharedContext(sharedContext_);
        {
            auto rit = responders_.find(ag.id);
            if (rit != responders_.end()) sa->setResponder(rit->second);
        }
        for (const auto& t : perAgent[ag.id]) if (jsonDefs.count(t)) sa->registerToolDef(jsonDefs[t]);
        for (const auto& t : config_.tools) if (regDefs.count(t)) sa->registerToolDef(regDefs[t]);

        if (config_.dynamicDispatch) {
            ToolDefinition ra;
            ra.name = "request_agent";
            ra.description = "Ask another agent (by id) and get its raw reply.";
            ra.parameters = {
                {"agent_id", "string", "target agent id", true},
                {"message", "string", "your request to that agent", true}
            };
            sa->registerToolDef(ra);
            sa->setToolInterceptor([this](const std::string&, const std::string& args) -> std::string {
                static thread_local int depth = 0;
                if (++depth > config_.maxRecursion) { --depth; return "[error] max recursion depth exceeded"; }
                std::string id, msg;
                try {
                    auto j = nlohmann::json::parse(args);
                    id = j.value("agent_id", std::string(""));
                    msg = j.value("message", std::string(""));
                } catch (...) {}
                std::string r = handleRequestAgent(id, msg, depth);
                --depth;
                return r;
            });
        }

        displayNames_[ag.id] = ag.displayName;
        agents_[ag.id] = std::move(sa);
    }
}

// ---- main loop -----------------------------------------------------------
void SkillRunner::run(const std::string& skillName, const std::string& topic) {
    topic_ = topic;
    if (!modelSupportsTools(cfg_.model)) {
        std::string msg = "[skill] model '" + cfg_.model +
            "' does not support tool calls; cannot run.";
        if (cbs_.onEvent) cbs_.onEvent("system", "system", msg);
        return;
    }

    std::string err;
    if (!SkillConfig::loadFromJson(skillsDir_ + "/" + skillName + "/config.json", config_, err)) {
        if (cbs_.onEvent) cbs_.onEvent("system", "system", "[skill] " + err);
        return;
    }
    loadAgents();

    for (int round = 1; round <= config_.maxRounds; ++round) {
        if (cancelRequested_.load()) break;
        lastRounds_ = round;
        executeSteps(config_.roundSteps, round);
        if (cancelRequested_.load()) break;
        if (stopRule(round)) { converged_ = true; break; }
        roundContext_ = voteSummary();
        prevSlots_ = slots_;
    }

    if (cancelRequested_.load()) {
        if (cbs_.onProgress) cbs_.onProgress("Cancelled");
        return;
    }
    executeSteps(config_.onConverge, lastRounds_);
    if (cbs_.onProgress)
        cbs_.onProgress(converged_ ? "Done (converged)" : "Done (round cap reached)");
}

// ---- step execution ------------------------------------------------------
void SkillRunner::executeSteps(const std::vector<SkillStep>& steps, int round) {
    for (const auto& st : steps) {
        if (cancelRequested_.load()) return;
        if (!whenMatches(st.when, round)) continue;
        if (!st.progress.empty() && cbs_.onProgress) cbs_.onProgress(fill(st.progress, round, {}));

        std::vector<std::string> results(st.actions.size());
        if (st.parallel && st.actions.size() > 1) {
            std::vector<std::thread> th;
            th.reserve(st.actions.size());
            for (size_t i = 0; i < st.actions.size(); ++i)
                th.emplace_back([&, i] {
                    const SkillAction& a = st.actions[i];
                    results[i] = agents_[a.agent]->turn(fill(a.message, round, a.vars),
                                                        a.tool, !config_.dynamicDispatch);
                });
            for (auto& t : th) t.join();
        } else {
            for (size_t i = 0; i < st.actions.size(); ++i) {
                const SkillAction& a = st.actions[i];
                results[i] = agents_[a.agent]->turn(fill(a.message, round, a.vars),
                                                    a.tool, !config_.dynamicDispatch);
            }
        }
        for (size_t i = 0; i < st.actions.size(); ++i)
            mergeAction(st.actions[i], results[i], round);
    }
}

void SkillRunner::mergeAction(const SkillAction& a, const std::string& r, int round) {
    (void)round;
    if (a.shape == "vote") {
        VoteResult vr;
        bool ok = false;
        if (!r.empty()) {
            try {
                auto j = nlohmann::json::parse(r);
                if (j.is_object()) {
                    ok = true;
                    if (j.contains("agree") && j["agree"].is_boolean()) vr.agree = j["agree"].get<bool>();
                    vr.agreedPoints = strArr(j, "agreed_points");
                    vr.concerns = strArr(j, "concerns");
                    vr.suggestedTweak = j.value("suggested_tweak", std::string("none"));
                }
            } catch (...) {}
        }
        if (!ok) {
            vr.agree = false;
            vr.concerns.clear();
            vr.concerns.push_back(r.empty() ? "[high] (no structured vote)" : "[high] " + r);
        }
        votes_[a.agent] = vr;
        if (a.emit == "vote" && cbs_.onEvent)
            cbs_.onEvent(display(a.agent), "vote", voteLine(vr));
        return;
    }

    std::string s;
    if (!r.empty()) {
        if (a.capture.empty()) s = r;
        else {
            try {
                auto j = nlohmann::json::parse(r);
                if (j.contains(a.capture) && j[a.capture].is_string())
                    s = j[a.capture].get<std::string>();
            } catch (...) {}
        }
    }
    slots_[a.into] = s;
    if (!s.empty() && cbs_.onEvent) {
        if (a.emit == "message")  cbs_.onEvent(display(a.agent), "message", s);
        else if (a.emit == "document") cbs_.onEvent(display(a.agent), "document", s);
    }
}

// ---- stop rule / when ----------------------------------------------------
bool SkillRunner::stopRule(int round) const {
    if (!config_.loopDetect.empty() && round > 1) {
        auto it = slots_.find(config_.loopDetect);
        auto pit = prevSlots_.find(config_.loopDetect);
        if (it != slots_.end() && pit != prevSlots_.end() &&
            !it->second.empty() && it->second == pit->second)
            return true;
    }
    if (config_.converge == "all_agree" && !votes_.empty()) {
        for (const auto& kv : votes_) if (!kv.second.agree) return false;
        return true;
    }
    return false;
}

bool SkillRunner::whenMatches(const std::string& when, int round) const {
    if (when.empty()) return true;
    if (when == "round==1") return round == 1;
    if (when == "round>1")  return round > 1;
    return true;
}

// ---- templates -----------------------------------------------------------
std::string SkillRunner::fill(const std::string& tmpl, int round,
                              const std::map<std::string, std::string>& vars) const {
    std::string out;
    out.reserve(tmpl.size() + 64);
    size_t i = 0;
    while (i < tmpl.size()) {
        size_t b = tmpl.find("{{", i);
        if (b == std::string::npos) { out += tmpl.substr(i); break; }
        out += tmpl.substr(i, b - i);
        size_t e = tmpl.find("}}", b + 2);
        if (e == std::string::npos) { out += tmpl.substr(b); break; }
        std::string key = tmpl.substr(b + 2, e - b - 2);
        size_t s = key.find_first_not_of(" \t");
        size_t t = key.find_last_not_of(" \t");
        key = (s == std::string::npos) ? std::string() : key.substr(s, t - s + 1);

        std::string val;
        if (key == "topic") val = topic_;
        else if (key == "round") val = std::to_string(round);
        else if (key == "max_rounds") val = std::to_string(config_.maxRounds);
        else if (key == "round_context") val = roundContext_;
        else if (key == "residual_concerns") val = residualConcerns();
        else if (key == "round_outputs") val = roundOutputs();
        else {
            auto vit = vars.find(key);
            if (vit != vars.end()) val = vit->second;
            else if (key.size() > 8 && key.compare(key.size() - 8, 8, ".current") == 0) {
                auto it = slots_.find(key.substr(0, key.size() - 8));
                if (it != slots_.end()) val = it->second;
            } else if (key.size() > 5 && key.compare(key.size() - 5, 5, ".prev") == 0) {
                auto it = prevSlots_.find(key.substr(0, key.size() - 5));
                if (it != prevSlots_.end()) val = it->second;
            }
        }
        out += val;
        i = e + 2;
    }
    return out;
}

std::string SkillRunner::roundOutputs() const {
    std::string out;
    for (const auto& st : config_.roundSteps)
        for (const auto& a : st.actions)
            if (a.shape == "text" && a.emit == "message") {
                auto it = slots_.find(a.into);
                if (it != slots_.end() && !it->second.empty())
                    out += "[" + display(a.agent) + "]\n" + it->second + "\n\n";
            }
    return out;
}

// ---- summary helpers (byte-aligned with old roundtable.cpp) --------------
std::string SkillRunner::voteSummary() const {
    std::string out;
    for (const auto& ag : config_.agents) {
        auto it = votes_.find(ag.id);
        if (it == votes_.end()) continue;
        const VoteResult& v = it->second;
        out += ag.displayName;
        out += v.agree ? ": agree" : ": dissent";
        out += " (concerns: ";
        if (v.concerns.empty()) out += "none";
        else for (size_t k = 0; k < v.concerns.size(); ++k) { if (k) out += "; "; out += v.concerns[k]; }
        out += "); tweak: " + v.suggestedTweak + "\n";
    }
    return out;
}

std::string SkillRunner::residualConcerns() const {
    std::string out;
    for (const auto& ag : config_.agents) {
        auto it = votes_.find(ag.id);
        if (it == votes_.end() || it->second.agree) continue;
        for (const auto& c : it->second.concerns) out += "- " + ag.displayName + ": " + c + "\n";
    }
    if (out.empty()) out = "(none)\n";
    return out;
}

std::string SkillRunner::voteLine(const VoteResult& vr) const {
    std::string s = vr.agree ? "AGREE" : "DISSENT";
    if (!vr.concerns.empty()) {
        s += " (concerns: ";
        for (size_t k = 0; k < vr.concerns.size(); ++k) { if (k) s += "; "; s += vr.concerns[k]; }
        s += ")";
    }
    return s;
}

std::string SkillRunner::display(const std::string& agentId) const {
    auto it = displayNames_.find(agentId);
    return it != displayNames_.end() ? it->second : agentId;
}

std::string SkillRunner::handleRequestAgent(const std::string& id, const std::string& msg, int depth) {
    (void)depth;
    if (!agents_.count(id)) return "[error] unknown agent '" + id + "'";
    std::string verb;
    for (const auto& st : config_.roundSteps)
        for (const auto& a : st.actions) if (a.agent == id) { verb = a.tool; break; }
    if (verb.empty())
        for (const auto& st : config_.onConverge)
            for (const auto& a : st.actions) if (a.agent == id) { verb = a.tool; break; }
    if (verb.empty()) return "[error] no tool for agent '" + id + "'";
    return agents_[id]->turn(msg, verb, /*force=*/false);
}
