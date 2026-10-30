// SOP pipeline skills (avenger / steve / musk): the three configs must load
// with zero warnings, and the two orchestration styles must actually behave.
// steve is a *script* - seven roles, nobody holds request_agent, the engine
// counts the votes and all_agree is the gate. avenger / musk are *dispatched* -
// one driver fans out to leaves until stop_when fires. All scripted, no network.
#include "doctest.h"

#include "core/skill_runner.h"
#include "core/skill_config.h"
#include "core/config.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

void ensureSysUtil() {
    static bool once = [] {
        SystemUtil::Init(CreateSystemUtil());
        return true;
    }();
    (void)once;
}

ToolCall makeCall(const std::string& name, const nlohmann::json& args) {
    ToolCall tc;
    tc.id = "call_" + name;
    tc.name = name;
    tc.arguments = args.dump();
    tc.index = 0;
    tc.valid = true;
    return tc;
}

AppConfig testConfig() {
    AppConfig c;
    c.model = "deepseek-v4-flash";
    return c;
}

std::string lastUser(const std::vector<Message>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->role == "user") return it->content;
    return "";
}

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

const char* kSkills = PROJV_EXAMPLE_SKILLS_DIR;

SkillConfig load(const std::string& name) {
    SkillConfig cfg;
    std::string err;
    const bool ok = SkillConfig::loadFromJson(std::string(kSkills) + "/" + name + "/config.json",
                                              cfg, err);
    CHECK(ok);
    CHECK(err.empty());
    return cfg;
}

// Records the last user message of every turn, in call order, for ordering and
// content assertions. Parallel steps make this non-deterministic per agent, so
// assertions below only compare agents that cannot overlap.
struct Recorder {
    std::mutex mtx;
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::string>> byAgent;
    void add(const std::string& who, const std::vector<Message>& msgs) {
        std::lock_guard<std::mutex> lk(mtx);
        order.push_back(who);
        byAgent[who].push_back(lastUser(msgs));
    }
    int count(const std::string& who) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = byAgent.find(who);
        return it == byAgent.end() ? 0 : static_cast<int>(it->second.size());
    }
    int indexOf(const std::string& who) {
        std::lock_guard<std::mutex> lk(mtx);
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == who) return static_cast<int>(i);
        return -1;
    }
};

} // namespace

TEST_CASE("sop: the three configs load with a clean bill (no unknown keys)") {
    ensureSysUtil();
    for (const char* name : {"avenger", "steve", "musk"}) {
        INFO(name);
        SkillConfig cfg = load(name);
        // A typo'd key is only a warning -- this is the assertion that catches it.
        CHECK(cfg.warnings.empty());
        CHECK(cfg.name == name);
        CHECK(cfg.agents.size() >= 5);
        for (const auto& ag : cfg.agents) {
            INFO(ag.id);
            CHECK(!ag.id.empty());
            CHECK(std::filesystem::exists(std::string(kSkills) + "/" + name + "/" + ag.promptFile));
        }
        // Every action's agent exists and every verb tool file is present.
        int actions = 0;
        for (const auto* steps : {&cfg.roundSteps, &cfg.onConverge})
            for (const auto& st : *steps)
                for (const auto& a : st.actions) {
                    ++actions;
                    CHECK(!a.tool.empty());
                    CHECK(std::filesystem::exists(std::string(kSkills) + "/" + name + "/tools/" +
                                                  a.tool + ".json"));
                }
        CHECK(actions >= 2);
        // Every declared verb tool parses and is reachable from the folder.
        for (auto& e : std::filesystem::directory_iterator(std::string(kSkills) + "/" + name +
                                                           "/tools")) {
            if (e.path().extension() != ".json") continue;
            auto j = nlohmann::json::parse(readFile(e.path().string()));
            CHECK(!j.value("name", std::string("")).empty());
        }
    }
}

TEST_CASE("sop: steve is a script - 7 roles, no driver, engine-counted votes") {
    SkillConfig cfg = load("steve");
    CHECK(cfg.dynamicDispatch == false); // nobody here can dispatch: the engine runs the script
    CHECK(cfg.converge == "all_agree");
    CHECK(cfg.loopDetect == "design_pm");
    CHECK(cfg.agents.size() == 7);

    int voteActions = 0, voteSteps = 0, reviseSteps = 0, reviseStep = -1, voteStep = -1;
    for (size_t i = 0; i < cfg.roundSteps.size(); ++i) {
        int votes = 0;
        for (const auto& a : cfg.roundSteps[i].actions)
            if (a.shape == "vote") ++votes;
        if (votes) {
            voteActions += votes;
            if (voteSteps++ == 0) voteStep = static_cast<int>(i);
        } else if (cfg.roundSteps[i].when == "round>1") {
            ++reviseSteps;
            reviseStep = static_cast<int>(i);
        }
    }
    CHECK(voteActions == 7);   // all seven, Steve included
    CHECK(voteSteps == 1);     // exactly one vote step per round
    CHECK(reviseSteps == 1);   // the PM revision is the only round>1 text step
    CHECK(reviseStep >= 0);
    CHECK(voteStep >= 0);
    CHECK(reviseStep < voteStep); // revise first, then vote, inside the round
}

TEST_CASE("sop: steve converges in round 1 when everyone agrees") {
    ensureSysUtil();
    Recorder rec;
    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };
    SkillRunner sr(testConfig(), nullptr, cbs, "", kSkills);

    auto role = [&rec](const std::string& id, const std::string& body) {
        return [&rec, id, body](const std::string& verb, const std::vector<Message>& m) {
            rec.add(id, m);
            if (verb == "design_vote")
                return makeCall("design_vote",
                                nlohmann::json{{"agree", true},
                                               {"agreed_points", nlohmann::json::array()},
                                               {"concerns", nlohmann::json::array()},
                                               {"suggested_tweak", "none"}});
            if (verb == "write_steve_doc")
                return makeCall("write_steve_doc",
                                nlohmann::json{{"doc_markdown", "# PLAN from " + body}});
            return makeCall("design_turn", nlohmann::json{{"kind", "design"}, {"text", body}});
        };
    };
    sr.setResponder("steve", role("steve", "CORE VALUE"));
    sr.setResponder("design_ux", role("design_ux", "UX DESIGN"));
    sr.setResponder("design_ui", role("design_ui", "UI DESIGN"));
    sr.setResponder("design_backend", role("design_backend", "BACKEND DESIGN"));
    sr.setResponder("design_algo", role("design_algo", "ALGO DESIGN"));
    sr.setResponder("design_pm", role("design_pm", "PM PLAN"));
    sr.setResponder("design_qa", role("design_qa", "QA CRITERIA"));

    sr.run("steve", "sop test topic");

    CHECK(sr.converged());
    CHECK(sr.lastRounds() == 1);
    CHECK(sr.lastDoc().find("PLAN") != std::string::npos);
    // Steve spoke (brief), then the four designers, then PM unified, then QA.
    CHECK(rec.count("steve") == 2); // brief + vote
    CHECK(rec.count("design_ux") == 2);
    CHECK(rec.count("design_pm") == 3); // plan + vote + the final document
    CHECK(rec.indexOf("design_pm") > rec.indexOf("design_backend"));
    CHECK(rec.indexOf("design_qa") > rec.indexOf("design_pm"));
    int votes = 0;
    for (auto& e : events)
        if (e.find("|vote|") != std::string::npos) ++votes;
    CHECK(votes == 7);
}

TEST_CASE("sop: one dissent sends the plan back to PM, then it converges") {
    ensureSysUtil();
    Recorder rec;
    int uxVotes = 0, pmTurns = 0;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", kSkills);

    auto role = [&rec](const std::string& id, const std::string& body) {
        return [&rec, id, body](const std::string& verb, const std::vector<Message>& m) {
            rec.add(id, m);
            if (verb == "design_vote")
                return makeCall("design_vote",
                                nlohmann::json{{"agree", true},
                                               {"agreed_points", nlohmann::json::array()},
                                               {"concerns", nlohmann::json::array()},
                                               {"suggested_tweak", "none"}});
            if (verb == "write_steve_doc")
                return makeCall("write_steve_doc",
                                nlohmann::json{{"doc_markdown", "# PLAN from " + body}});
            return makeCall("design_turn", nlohmann::json{{"kind", "design"}, {"text", body}});
        };
    };
    sr.setResponder("steve", role("steve", "CORE VALUE"));
    // UX dissents in round 1, agrees afterwards -> exactly one revision round.
    sr.setResponder("design_ux", [&](const std::string& verb, const std::vector<Message>& m) {
        rec.add("design_ux", m);
        if (verb == "design_vote") {
            const bool first = (++uxVotes == 1);
            return makeCall("design_vote",
                            nlohmann::json{
                                {"agree", !first},
                                {"agreed_points", nlohmann::json::array()},
                                {"concerns", first ? nlohmann::json::array({"broken flow"})
                                                   : nlohmann::json::array()},
                                {"suggested_tweak", first ? "add the empty state" : "none"}});
        }
        return makeCall("design_turn", nlohmann::json{{"kind", "design"}, {"text", "UX DESIGN"}});
    });
    sr.setResponder("design_ui", role("design_ui", "UI DESIGN"));
    sr.setResponder("design_backend", role("design_backend", "BACKEND DESIGN"));
    sr.setResponder("design_algo", role("design_algo", "ALGO DESIGN"));
    // PM: the plan text must change per round, otherwise loop_detect stops the run.
    sr.setResponder("design_pm", [&](const std::string& verb, const std::vector<Message>& m) {
        rec.add("design_pm", m);
        if (verb == "design_vote")
            return makeCall("design_vote",
                            nlohmann::json{{"agree", true},
                                           {"agreed_points", nlohmann::json::array()},
                                           {"concerns", nlohmann::json::array()},
                                           {"suggested_tweak", "none"}});
        if (verb == "write_steve_doc")
            return makeCall("write_steve_doc", nlohmann::json{{"doc_markdown", "# REVISED PLAN"}});
        return makeCall("design_turn",
                        nlohmann::json{{"kind", "plan"},
                                       {"text", "PLAN v" + std::to_string(++pmTurns)}});
    });
    sr.setResponder("design_qa", role("design_qa", "QA CRITERIA"));

    sr.run("steve", "sop test topic");

    CHECK(sr.converged());
    CHECK(sr.lastRounds() == 2); // round 1 dissent -> round 2 revision + vote
    CHECK(pmTurns == 2);         // the plan, then the revision
    bool sawDissent = false;
    for (const auto& msg : rec.byAgent["design_pm"])
        if (msg.find("broken flow") != std::string::npos) sawDissent = true;
    CHECK(sawDissent); // {{residual_concerns}} reached the revision step
    CHECK(sr.lastDoc().find("REVISED") != std::string::npos);
}

TEST_CASE("sop: avenger dispatches the core three in one reply, then stops") {
    ensureSysUtil();
    Recorder rec;
    int starkTurns = 0;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", kSkills);

    sr.setResponderMulti("stark", [&](const std::string& verb, const std::vector<Message>& m) {
        rec.add("stark", m);
        std::vector<ToolCall> out;
        if (verb == "write_avenger_doc") {
            out.push_back(makeCall("write_avenger_doc",
                                   nlohmann::json{{"doc_markdown", "# AVENGER REPORT"}}));
            return out;
        }
        if (++starkTurns == 1) {
            for (const char* id : {"research_mkt", "research_user", "research_oss"}) {
                ToolCall tc = makeCall(
                    "request_agent",
                    nlohmann::json{{"agent_id", id},
                                   {"message", "Topic: sop test, slug sop-test, write "
                                               "docs/sop/avenger_sop-test_x.md"}});
                tc.id = std::string("ra_") + id;
                out.push_back(tc);
            }
            return out;
        }
        out.push_back(makeCall("emit", nlohmann::json{{"summary", "DONE"}}));
        return out;
    });
    for (const char* id : {"research_mkt", "research_user", "research_oss"}) {
        sr.setResponder(id, [&rec, id](const std::string&, const std::vector<Message>& m) {
            rec.add(id, m);
            return makeCall("research_turn", nlohmann::json{{"summary", "line report"},
                                                            {"report_path", "docs/sop/x.md"}});
        });
    }
    // The optional lines must stay asleep: recording them proves it happened.
    for (const char* id : {"research_tech", "research_comp", "research_money"}) {
        sr.setResponder(id, [&rec, id](const std::string&, const std::vector<Message>& m) {
            rec.add(id, m);
            return makeCall("research_turn", nlohmann::json{{"summary", "woken"},
                                                            {"report_path", "docs/sop/x.md"}});
        });
    }

    sr.run("avenger", "sop test topic");

    CHECK(sr.converged()); // stop_when: judgment == DONE
    CHECK(sr.lastRounds() == 1);
    CHECK(rec.count("research_mkt") == 1);
    CHECK(rec.count("research_user") == 1);
    CHECK(rec.count("research_oss") == 1);
    CHECK(rec.count("research_tech") == 0);
    CHECK(rec.count("research_comp") == 0);
    CHECK(rec.count("research_money") == 0);
    CHECK(sr.lastDoc().find("AVENGER") != std::string::npos);
}

TEST_CASE("sop: musk pairs a task - dev, then an independent test, then stops") {
    ensureSysUtil();
    Recorder rec;
    int muskTurns = 0;
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs, "", kSkills);

    sr.setResponderMulti("musk", [&](const std::string& verb, const std::vector<Message>& m) {
        rec.add("musk", m);
        std::vector<ToolCall> out;
        if (verb == "write_musk_doc") {
            out.push_back(
                makeCall("write_musk_doc", nlohmann::json{{"doc_markdown", "# MUSK RECORD"}}));
            return out;
        }
        const int n = ++muskTurns;
        if (n == 1)
            out.push_back(makeCall(
                "request_agent",
                nlohmann::json{{"agent_id", "dev_1"}, {"message", "DoD: cli prints 3 rows"}}));
        else if (n == 2)
            out.push_back(makeCall(
                "request_agent",
                nlohmann::json{{"agent_id", "test_1"}, {"message", "verify: cli prints 3 rows"}}));
        else
            out.push_back(makeCall("emit", nlohmann::json{{"summary", "DONE"}}));
        return out;
    });
    sr.setResponder("dev_1", [&rec](const std::string&, const std::vector<Message>& m) {
        rec.add("dev_1", m);
        return makeCall("work_turn", nlohmann::json{{"status", "done"}, {"summary", "written"}});
    });
    sr.setResponder("test_1", [&rec](const std::string&, const std::vector<Message>& m) {
        rec.add("test_1", m);
        return makeCall("work_turn", nlohmann::json{{"status", "pass"}, {"summary", "3 rows"}});
    });
    for (const char* id : {"dev_2", "test_2"}) {
        sr.setResponder(id, [&rec, id](const std::string&, const std::vector<Message>& m) {
            rec.add(id, m);
            return makeCall("work_turn", nlohmann::json{{"status", "done"}, {"summary", "idle"}});
        });
    }

    sr.run("musk", "sop test job");

    CHECK(sr.converged()); // stop_when: kanban == DONE
    CHECK(sr.lastRounds() == 1);
    CHECK(rec.count("dev_1") == 1);
    CHECK(rec.count("test_1") == 1);
    CHECK(rec.indexOf("test_1") > rec.indexOf("dev_1")); // verify after implement
    CHECK(rec.count("dev_2") == 0);
    CHECK(rec.count("test_2") == 0);
    CHECK(sr.lastDoc().find("MUSK") != std::string::npos);
}
