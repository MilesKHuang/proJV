// SKILL_ENGINE -- SkillRunner implementation (see SKILL_ENGINE.md v1.1).
#include "skill_runner.h"
#include "core/sub_agent.h"
#include "core/config.h"
#include "core/skills_builtin.h"
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

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// True when `v` is one of the allowed literals. Reports the legal set on miss.
bool checkEnum(const std::string& key, const std::string& v,
               std::initializer_list<const char*> allowed, std::string& err) {
    for (const char* a : allowed) if (v == a) return true;
    std::string list;
    for (const char* a : allowed) { if (!list.empty()) list += " | "; list += std::string("\"") + a + "\""; }
    err = "invalid '" + key + "' value \"" + v + "\" (allowed: " + list + ")";
    return false;
}

size_t editDistance(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<size_t> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[m];
}

// Unknown keys are noisy bugs, not fatal ones: report them, keep running.
void warnUnknownKeys(const nlohmann::json& j, const std::vector<std::string>& known,
                     const std::string& prefix, std::vector<std::string>& warnings) {
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string key = it.key();
        bool ok = false;
        std::string best;
        size_t bestDist = std::string::npos;
        for (const auto& k : known) {
            if (k == key) { ok = true; break; }
            const size_t d = editDistance(toLower(key), toLower(k));
            if (d < bestDist) { bestDist = d; best = k; }
        }
        if (ok) continue;
        std::string msg = "unknown key '" + prefix + key + "'";
        if (bestDist <= 3) msg += " (did you mean '" + prefix + best + "'?)";
        warnings.push_back(msg);
    }
}

} // namespace

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
        out.dispatchTool = j.value("dispatch_tool", std::string(""));
        out.maxToolIters = j.value("max_tool_iters", 3);
        out.submitReserve = j.value("submit_reserve", 0);
        out.toolMode = j.value("tool_mode", std::string("verb"));
        out.thinking = j.value("thinking", std::string("on"));
        out.maxParallel = j.value("max_parallel", 16);
        out.warnings.clear();

        // Known literals are validated: a typo here used to fall back to the
        // default silently, which is how "toolMode" cost a whole evening.
        if (!checkEnum("tool_mode", out.toolMode, {"verb", "auto"}, err)) return false;
        if (!checkEnum("converge", out.converge, {"none", "all_agree"}, err)) return false;
        if (!checkEnum("thinking", out.thinking, {"on", "off"}, err)) return false;
        if (out.maxRounds < 1) { err = "max_rounds must be >= 1"; return false; }
        if (out.maxToolIters < 1) { err = "max_tool_iters must be >= 1"; return false; }
        if (out.submitReserve < 0 || out.submitReserve >= out.maxToolIters) {
            err = "submit_reserve must be >= 0 and < max_tool_iters (got " +
                  std::to_string(out.submitReserve) + " with max_tool_iters " +
                  std::to_string(out.maxToolIters) + ")";
            return false;
        }
        if (out.maxParallel < 1 || out.maxParallel > 64) {
            err = "max_parallel must be between 1 and 64 (got " +
                  std::to_string(out.maxParallel) + ")";
            return false;
        }

        if (j.contains("stop_when") && j["stop_when"].is_object()) {
            out.stopWhenSlot = j["stop_when"].value("slot", std::string(""));
            out.stopWhenValue = j["stop_when"].value("equals", std::string(""));
        }
        if (j.contains("tools") && j["tools"].is_array())
            for (const auto& t : j["tools"]) if (t.is_string()) out.tools.push_back(t.get<std::string>());
        if (j.contains("agents") && j["agents"].is_array())
            for (const auto& a : j["agents"]) {
                SkillAgent ag;
                ag.id = a.value("id", std::string(""));
                ag.displayName = a.value("display_name", ag.id);
                ag.promptFile = a.value("prompt", std::string(""));
                ag.thinking = a.value("thinking", std::string(""));
                if (!ag.thinking.empty() &&
                    !checkEnum("agents[].thinking", ag.thinking, {"on", "off"}, err)) return false;
                if (ag.id.empty()) { err = "every agent needs an 'id'"; return false; }
                warnUnknownKeys(a, {"id", "display_name", "prompt", "thinking"},
                                "agents." + ag.id + ".", out.warnings);
                out.agents.push_back(ag);
            }

        auto loadSteps = [&](const nlohmann::json& arr, std::vector<SkillStep>& dst) -> bool {
            if (!arr.is_array()) return true;
            for (const auto& s : arr) {
                SkillStep st;
                st.when = s.value("when", std::string(""));
                st.parallel = s.value("parallel", false);
                st.progress = s.value("progress", std::string(""));
                if (!checkEnum("when", st.when, {"", "round==1", "round>1"}, err)) return false;
                warnUnknownKeys(s, {"when", "parallel", "progress", "actions"}, "", out.warnings);
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
                        if (!checkEnum("shape", ac.shape, {"text", "vote"}, err)) return false;
                        if (!checkEnum("emit", ac.emit, {"", "message", "vote", "document"}, err))
                            return false;
                        warnUnknownKeys(a, {"agent", "tool", "message", "capture", "into",
                                            "shape", "emit", "vars"}, "", out.warnings);
                        st.actions.push_back(std::move(ac));
                    }
                dst.push_back(std::move(st));
            }
            return true;
        };
        if (j.contains("round_steps") && !loadSteps(j["round_steps"], out.roundSteps)) return false;
        if (j.contains("on_converge") && !loadSteps(j["on_converge"], out.onConverge)) return false;

        // Every action must point at a declared agent: a typo here silently
        // skips the action and looks like "the agent did nothing".
        for (const auto* steps : {&out.roundSteps, &out.onConverge})
            for (const auto& st : *steps)
                for (const auto& ac : st.actions) {
                    bool known = false;
                    for (const auto& ag : out.agents) if (ag.id == ac.agent) { known = true; break; }
                    if (!known) { err = "action references unknown agent '" + ac.agent + "'"; return false; }
                }

        warnUnknownKeys(j, {"name", "description", "tools", "agents", "max_rounds", "converge",
                            "loop_detect", "dynamic_dispatch", "dispatch_tool", "max_tool_iters",
                            "submit_reserve", "tool_mode", "thinking", "max_parallel",
                            "stop_when", "round_steps", "on_converge"},
                        "", out.warnings);
    } catch (const std::exception& e) { err = e.what(); return false; }
    if (out.name.empty()) { err = "skill has no name"; return false; }
    return true;
}

bool SkillConfig::thinkingEnabled(const std::string& skillLevel, const std::string& agentLevel) {
    const std::string& v = agentLevel.empty() ? skillLevel : agentLevel;
    return v != "off";
}

// ---- lifecycle -----------------------------------------------------------
SkillRunner::SkillRunner(AppConfig cfg, ToolRegistry* tools, Callbacks cbs,
                         std::string sharedContext, std::string skillsDir)
    : cfg_(std::move(cfg)), tools_(tools), cbs_(std::move(cbs)),
      sharedContext_(std::move(sharedContext)) {
    skillsDir_ = skillsDir.empty() ? (getProjvDir() + "/skills") : skillsDir;
}

SkillRunner::~SkillRunner() = default;

void SkillRunner::cancel() {
    cancelRequested_.store(true);
    std::lock_guard<std::mutex> lk(snapMutex_);
    for (auto& kv : agents_) if (kv.second) kv.second->cancel();
}

// Boundary probe: called between actions, steps and rounds. A probe hit is
// promoted to a full cancel() so nested loops and sub-agent HTTP calls stop.
bool SkillRunner::checkCancel() {
    if (!cancelRequested_.load() && cancelProbe_ && cancelProbe_()) {
        debugLog("[Skill] external cancel probe hit -- stopping the run");
        cancel();
    }
    return cancelRequested_.load();
}

// ---- seeding / discovery -------------------------------------------------
void SkillRunner::ensureDefaultSkills(const std::string& skillsDir) {
    std::error_code ec;
    fs::create_directories(skillsDir, ec);
    seedBuiltinSkills(skillsDir);
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

    // Verb tool defs from tools/*.json.
    std::map<std::string, ToolDefinition> jsonDefs;
    {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(sdir + "/tools", ec)) {
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

    // One layer only: an agent that appears in the program drives the flow and
    // may dispatch; everyone else is a leaf worker and never sees request_agent.
    std::set<std::string> drivers;
    for (const auto* steps : {&config_.roundSteps, &config_.onConverge})
        for (const auto& st : *steps)
            for (const auto& ac : st.actions) drivers.insert(ac.agent);
    drivers_ = drivers;

    for (const auto& ag : config_.agents) {
        std::ifstream f(sdir + "/" + ag.promptFile, std::ios::binary);
        std::string prompt((std::istreambuf_iterator<char>(f)), {});

        auto sa = std::make_unique<SubAgent>(ag.displayName, prompt);
        // Role-level thinking overrides the skill-level setting.
        const bool thinkingOn = SkillConfig::thinkingEnabled(config_.thinking, ag.thinking);
        sa->configure(cfg_, tools_, config_.tools, config_.maxToolIters, thinkingOn);
        sa->setMaxParallel(config_.maxParallel);
        sa->setSubmitReserve(config_.submitReserve);
        sa->seedSharedContext(sharedContext_);
        {
            auto rit = responders_.find(ag.id);
            if (rit != responders_.end()) sa->setResponder(rit->second);
            auto mit = multiResponders_.find(ag.id);
            if (mit != multiResponders_.end()) sa->setResponderMulti(mit->second);
            auto tit = textResponders_.find(ag.id);
            if (tit != textResponders_.end()) sa->setResponderText(tit->second);
        }
        // Every verb tool is available to every agent (skill-local, no conflict
        // with capability tools). This lets request_agent workers -- which have
        // no explicit action in the program -- still have a verb to answer with.
        for (const auto& kv : jsonDefs) sa->registerToolDef(kv.second);
        for (const auto& t : config_.tools) if (regDefs.count(t)) sa->registerToolDef(regDefs[t]);

        if (config_.dynamicDispatch && drivers.count(ag.id)) {
            ToolDefinition ra;
            ra.name = "request_agent";
            ra.description =
                "Ask another agent (by id) and get its raw reply. Calls to different "
                "agents in one reply run concurrently; calls to the same agent run in order.";
            ra.parameters = {
                {"agent_id", "string", "target agent id", true},
                {"message", "string", "your request to that agent", true}
            };
            sa->registerToolDef(ra);
            sa->setToolInterceptor([this](const std::string&, const std::string& args) -> std::string {
                std::string id, msg;
                try {
                    auto j = nlohmann::json::parse(args);
                    id = j.value("agent_id", std::string(""));
                    msg = j.value("message", std::string(""));
                } catch (...) {}
                return handleRequestAgent(id, msg);
            });
        }

        {
            std::lock_guard<std::mutex> lk(snapMutex_);
            displayNames_[ag.id] = ag.displayName;
            agents_[ag.id] = std::move(sa);
        }
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
    // Non-fatal config findings (unknown / legacy keys). Visible, not fatal.
    for (const auto& w : config_.warnings)
        if (cbs_.onEvent) cbs_.onEvent("system", "system", "[skill] config: " + w);
    loadAgents();

    for (int round = 1; round <= config_.maxRounds; ++round) {
        if (checkCancel()) break;
        lastRounds_ = round;
        executeSteps(config_.roundSteps, round);
        if (checkCancel()) break;
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
        if (checkCancel()) return;
        if (!whenMatches(st.when, round)) continue;
        if (!st.progress.empty() && cbs_.onProgress) cbs_.onProgress(fill(st.progress, round, {}));

        // force = "verb" mode: pin the action tool; "auto": let the model choose
        // (needed for capability tools). dynamic_dispatch always implies auto.
        const bool force = (config_.toolMode != "auto") && !config_.dynamicDispatch;
        std::vector<std::string> results(st.actions.size());
        if (st.parallel && st.actions.size() > 1) {
            std::vector<std::thread> th;
            th.reserve(st.actions.size());
            for (size_t i = 0; i < st.actions.size(); ++i)
                th.emplace_back([&, i] {
                    if (checkCancel()) return;   // probe hit -> skip this action
                    const SkillAction& a = st.actions[i];
                    try {
                        if (SubAgent* ag = agentById(a.agent))
                            results[i] = ag->turn(fill(a.message, round, a.vars), a.tool, force);
                    } catch (const std::exception& e) {
                        results[i] = std::string("[error] ") + e.what();
                    } catch (...) {
                        results[i] = "[error] unknown exception in action thread";
                    }
                });
            for (auto& t : th) t.join();
        } else {
            for (size_t i = 0; i < st.actions.size(); ++i) {
                if (checkCancel()) return;
                const SkillAction& a = st.actions[i];
                try {
                    if (SubAgent* ag = agentById(a.agent))
                        results[i] = ag->turn(fill(a.message, round, a.vars), a.tool, force);
                } catch (const std::exception& e) {
                    results[i] = std::string("[error] ") + e.what();
                } catch (...) {
                    results[i] = "[error] unknown exception in action";
                }
            }
        }
        if (cancelRequested_.load()) return;
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
        bool got = false;
        if (!a.capture.empty()) {
            try {
                auto j = nlohmann::json::parse(r);
                if (j.is_object() && j.contains(a.capture) && j[a.capture].is_string()) {
                    s = j[a.capture].get<std::string>();
                    got = true;
                }
            } catch (...) {}
        }
        if (!got) s = r;   // fallback: raw text (narration / non-JSON reply)
    }
    slots_[a.into] = s;
    if (a.emit == "document") lastDoc_ = s;   // independent of callback presence
    if (!s.empty() && cbs_.onEvent) {
        if (a.emit == "message")  cbs_.onEvent(display(a.agent), "message", s);
        else if (a.emit == "document") cbs_.onEvent(display(a.agent), "document", s);
    }
}

// ---- stop rule / when ----------------------------------------------------
bool SkillRunner::stopRule(int round) const {
    if (!config_.stopWhenSlot.empty()) {
        auto it = slots_.find(config_.stopWhenSlot);
        if (it != slots_.end() && it->second == config_.stopWhenValue) return true;
    }
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
        else if (key == "skills_dir") val = skillsDir_;
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
    std::lock_guard<std::mutex> lk(snapMutex_);
    auto it = displayNames_.find(agentId);
    return it != displayNames_.end() ? it->second : agentId;
}

SubAgent* SkillRunner::agentById(const std::string& id) {
    std::lock_guard<std::mutex> lk(snapMutex_);
    auto it = agents_.find(id);
    return it == agents_.end() ? nullptr : it->second.get();
}

std::vector<SkillRunner::AgentSnapshot> SkillRunner::agentStatuses() const {
    std::vector<AgentSnapshot> out;
    std::lock_guard<std::mutex> lk(snapMutex_);
    out.reserve(agents_.size());
    for (const auto& kv : agents_) {
        AgentSnapshot s;
        s.id = kv.first;
        auto dn = displayNames_.find(kv.first);
        s.displayName = dn != displayNames_.end() ? dn->second : kv.first;
        if (kv.second) {
            s.msgCount = kv.second->messageCount();
            s.busy = kv.second->busy();
            s.status = kv.second->lastNarration();
            if (s.busy) s.activity = AgentActivity::Working;
            else if (s.status.rfind("[error]", 0) == 0) s.activity = AgentActivity::Error;
            else s.activity = AgentActivity::Idle;
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::string SkillRunner::handleRequestAgent(const std::string& id, const std::string& msg) {
    if (id.empty()) return "[error] request_agent needs an 'agent_id'";
    if (drivers_.count(id)) return "[error] cannot dispatch to a driver agent ('" + id + "')";
    SubAgent* ag = agentById(id);
    if (!ag) return "[error] unknown agent '" + id + "'";
    if (checkCancel()) return "[cancelled]";
    std::string verb = config_.dispatchTool;
    if (verb.empty()) verb = ag->firstToolName();
    if (verb.empty()) return "[error] no tool for agent '" + id + "'";
    return ag->turn(msg, verb, /*force=*/false);
}
