// Phase-B parity test: drives the NEW SkillRunner engine with scripted
// responders and asserts it produces the SAME bytes the OLD Roundtable does
// (see test_bigbang_messages.cpp). If both equal the same literals, the engine
// is byte-compatible with /bigbang on the normal path.
#include "doctest.h"

#include "core/skill_runner.h"
#include "core/config.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace {

void ensureSysUtil() {
    static bool once = [] { SystemUtil::Init(CreateSystemUtil()); return true; }();
    (void)once;
}

ToolCall makeCall(const std::string& name, const nlohmann::json& args) {
    ToolCall tc; tc.id = "call_" + name; tc.name = name;
    tc.arguments = args.dump(); tc.index = 0; tc.valid = true; return tc;
}
ToolCall turnCall(const std::string& s) {
    return makeCall("bigbang_turn", nlohmann::json{{"statement", s}});
}
ToolCall voteCall(bool agree) {
    nlohmann::json j; j["agree"] = agree;
    j["agreed_points"] = nlohmann::json::array();
    j["concerns"] = nlohmann::json::array();
    j["suggested_tweak"] = "none";
    return makeCall("bigbang_vote", j);
}
ToolCall docCall(const std::string& md) {
    return makeCall("write_bigbang_doc", nlohmann::json{{"doc_markdown", md}});
}
ToolCall noCall() { ToolCall b; b.valid = false; return b; }

std::string lastUser(const std::vector<Message>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->role == "user") return it->content;
    return "";
}

AppConfig testConfig() { AppConfig c; c.model = "deepseek-v4-flash"; return c; }

bool hasEvent(const std::vector<std::string>& ev, const std::string& s) {
    for (const auto& e : ev) if (e == s) return true;
    return false;
}

} // namespace

TEST_CASE("skill: round-1 unanimous matches golden bytes + events") {
    ensureSysUtil();
    SkillRunner::ensureDefaultSkills(getProjvDir() + "/skills");

    std::vector<std::string> cap[3];
    std::vector<std::string> events;
    auto role = [&](int i, const char* stmt, bool agree) {
        return [&cap, i, stmt, agree](const std::string& tool, const std::vector<Message>& m) {
            cap[i].push_back(lastUser(m));
            if (tool == "bigbang_turn") return turnCall(stmt);
            if (tool == "bigbang_vote") return voteCall(agree);
            if (tool == "write_bigbang_doc") return docCall("# EXEC");
            return noCall();
        };
    };
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& role, const std::string& kind, const std::string& text) {
        events.push_back(role + "|" + kind + "|" + text);
    };

    SkillRunner sr(testConfig(), nullptr, cbs);
    sr.setResponder("sheldon", role(0, "S1", true));
    sr.setResponder("penny", role(1, "P1", true));
    sr.setResponder("leonard", role(2, "L1", true));
    sr.run("bigbang_debate", "T");

    CHECK(cap[0][0] == "Topic: T\n\nCall bigbang_turn with your proposal.");
    CHECK(cap[1][0] == "Topic: T\n\nCall bigbang_turn with your proposal.");
    CHECK(cap[2][0] == "Topic: T\n\n=== Round 1 Integrate ===\n\nSheldon:\nS1\n\nPenny:\nP1\n\nCall bigbang_turn.");
    CHECK(cap[0][1] == "Topic: T\n\n=== Round 1 Vote ===\n\nProposal under vote:\nL1\n\nFor reference - Sheldon position:\nS1\n\nFor reference - Penny position:\nP1\n\nCall bigbang_vote.");
    CHECK(cap[2][2] == "Topic: T\n\nFinal approved proposal:\nL1\n\nResidual concerns:\n(none)\n\n\nCall write_bigbang_doc.");

    CHECK(sr.converged());
    CHECK(sr.lastRounds() == 1);
    CHECK(hasEvent(events, "Sheldon|message|S1"));
    CHECK(hasEvent(events, "Leonard|message|L1"));
    CHECK(hasEvent(events, "Sheldon|vote|AGREE"));
    CHECK(hasEvent(events, "Leonard|document|# EXEC"));
}

TEST_CASE("skill: round-2 repeat matches golden bytes") {
    ensureSysUtil();
    SkillRunner::ensureDefaultSkills(getProjvDir() + "/skills");

    std::vector<std::string> cap[3];
    int sc = 0, pc = 0, lc = 0;
    auto role = [&](int i, int* counter, const char* pfx) {
        return [&cap, i, counter, pfx](const std::string& tool, const std::vector<Message>& m) {
            cap[i].push_back(lastUser(m));
            if (tool == "bigbang_turn") { *counter += 1; return turnCall(std::string(pfx) + std::to_string(*counter)); }
            if (tool == "bigbang_vote") return voteCall(*counter >= 2);
            if (tool == "write_bigbang_doc") return docCall("# EXEC");
            return noCall();
        };
    };
    SkillRunner::Callbacks cbs;
    SkillRunner sr(testConfig(), nullptr, cbs);
    sr.setResponder("sheldon", role(0, &sc, "S"));
    sr.setResponder("penny", role(1, &pc, "P"));
    sr.setResponder("leonard", role(2, &lc, "L"));
    sr.run("bigbang_debate", "T");

    const std::string VS1 =
        "Sheldon: dissent (concerns: none); tweak: none\n"
        "Penny: dissent (concerns: none); tweak: none\n"
        "Leonard: dissent (concerns: none); tweak: none\n";

    CHECK(cap[0][2] == "Topic: T\n\n=== Round 2 ===\n\nLeonard compromise from previous round:\nL1\n\nPrevious vote results:\n" + VS1 + "\n\nPenny's previous proposal:\nP1\n\nCall bigbang_turn.");
    CHECK(cap[1][2] == "Topic: T\n\n=== Round 2 ===\n\nLeonard compromise from previous round:\nL1\n\nPrevious vote results:\n" + VS1 + "\n\nSheldon's previous proposal:\nS1\n\nCall bigbang_turn.");
    CHECK(cap[2][2] == "Topic: T\n\n=== Round 2 Integrate ===\n\nSheldon:\nS2\n\nPenny:\nP2\n\nPrevious vote results:\n" + VS1 + "\n\nCall bigbang_turn.");
    CHECK(cap[2][3] == "Topic: T\n\n=== Round 2 Vote ===\n\nProposal under vote:\nL2\n\nFor reference - Sheldon position:\nS2\n\nFor reference - Penny position:\nP2\n\nCall bigbang_vote.");
    CHECK(cap[2][4] == "Topic: T\n\nFinal approved proposal:\nL2\n\nResidual concerns:\n(none)\n\n\nCall write_bigbang_doc.");

    CHECK(sr.converged());
    CHECK(sr.lastRounds() == 2);
}
