// Bigbang Roundtable boundary tests -- offline, scripted responders (no API).
//
// These lock down the convergence contract of Roundtable::run:
//   1. unanimous agree      -> stop after one round, converged
//   2. persistent dissent   -> stop at the round cap, not converged, still doc
//   3. identical Leonard statement twice -> early stop (loop detection)
#include "doctest.h"

#include "core/roundtable.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace {

// The bigbang prompts dir resolves via SystemUtil; init the singleton once.
void ensureSysUtil() {
    static bool once = [] { SystemUtil::Init(CreateSystemUtil()); return true; }();
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

ToolCall turnCall(const std::string& statement) {
    return makeCall("bigbang_turn", nlohmann::json{{"statement", statement}});
}

ToolCall voteCall(bool agree) {
    nlohmann::json j;
    j["agree"] = agree;
    j["agreed_points"] = nlohmann::json::array();
    j["concerns"] = nlohmann::json::array();
    j["suggested_tweak"] = "none";
    return makeCall("bigbang_vote", j);
}

ToolCall docCall(const std::string& md) {
    return makeCall("write_bigbang_doc", nlohmann::json{{"doc_markdown", md}});
}

ToolCall noCall() {
    ToolCall bad;
    bad.valid = false;
    return bad;
}

// A role that always answers with the given statement / vote / doc.
BigbangResponder fixedRole(bool agree, const std::string& statement) {
    return [agree, statement](const std::string& tool, const std::vector<Message>&) {
        if (tool == "bigbang_turn") return turnCall(statement);
        if (tool == "bigbang_vote") return voteCall(agree);
        if (tool == "write_bigbang_doc") return docCall("# exec doc");
        return noCall();
    };
}

AppConfig testConfig() {
    AppConfig c;
    c.model = "deepseek-v4-flash";  // tool-capable -> passes the guard
    return c;
}

} // namespace

TEST_CASE("roundtable: unanimous agree converges after one round") {
    ensureSysUtil();
    Roundtable rt(testConfig(), nullptr, {});
    rt.setResponder(0, fixedRole(true, "sheldon proposal"));
    rt.setResponder(1, fixedRole(true, "penny proposal"));
    rt.setResponder(2, fixedRole(true, "leonard compromise"));

    rt.run("test topic");

    CHECK(rt.lastRounds() == 1);
    CHECK(rt.converged());
    CHECK_FALSE(rt.lastDoc().empty());
}

TEST_CASE("roundtable: persistent dissent stops at the round cap") {
    ensureSysUtil();
    Roundtable rt(testConfig(), nullptr, {});
    rt.setMaxRounds(3);

    // Leonard varies his statement each round so loop-detection never fires.
    int counter = 0;
    BigbangResponder leonard = [&counter](const std::string& tool,
                                          const std::vector<Message>&) {
        if (tool == "bigbang_turn") return turnCall("leonard v" + std::to_string(counter++));
        if (tool == "bigbang_vote") return voteCall(false);
        if (tool == "write_bigbang_doc") return docCall("# exec doc");
        return noCall();
    };
    rt.setResponder(0, fixedRole(false, "sheldon proposal"));
    rt.setResponder(1, fixedRole(false, "penny proposal"));
    rt.setResponder(2, leonard);

    rt.run("test topic");

    CHECK(rt.lastRounds() == 3);
    CHECK_FALSE(rt.converged());
    CHECK_FALSE(rt.lastDoc().empty());   // doc is still produced at the cap
}

TEST_CASE("roundtable: identical Leonard statement twice triggers early stop") {
    ensureSysUtil();
    Roundtable rt(testConfig(), nullptr, {});
    // Votes never agree, so only loop-detection can stop it.
    rt.setResponder(0, fixedRole(false, "sheldon proposal"));
    rt.setResponder(1, fixedRole(false, "penny proposal"));
    rt.setResponder(2, fixedRole(false, "leonard compromise"));

    rt.run("test topic");

    CHECK(rt.lastRounds() == 2);
    CHECK(rt.converged());
    CHECK_FALSE(rt.lastDoc().empty());
}
