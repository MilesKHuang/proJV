// skill_maker capability test: the seeded skill_maker must (a) exist, (b) grant
// write tools, and (c) the engine must run its creator->verifier flow so that a
// real skill directory lands on disk and is discoverable. Scripted responders.
#include "doctest.h"

#include "core/skill_runner.h"
#include "core/config.h"
#include "tools/registry.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
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
AppConfig testConfig() { AppConfig c; c.model = "deepseek-v4-flash"; return c; }

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

const char* MINI_CONFIG = R"({"name":"mm_test","description":"mini","tools":[],"tool_mode":"verb","max_rounds":1,"converge":"none","agents":[{"id":"a","display_name":"A","prompt":"a.md"}],"round_steps":[{"actions":[{"agent":"a","tool":"emit","capture":"text","into":"a","emit":"message","message":"hi"}]}]})";

} // namespace

TEST_CASE("skill_maker: seeded, granted write tools, and runs a create flow end to end") {
    ensureSysUtil();
    std::string root = getProjvDir() + "/skills";
    SkillRunner::ensureDefaultSkills(root);

    // (a) seeded + discoverable
    bool found = false;
    for (auto& n : SkillRunner::listSkills(root)) if (n == "skill_maker") found = true;
    CHECK(found);

    // (b) its capability whitelist grants the tools needed to author a skill
    auto cfg = nlohmann::json::parse(readFile(root + "/skill_maker/config.json"));
    auto hasTool = [&](const char* t) {
        for (auto& x : cfg["tools"]) if (x.is_string() && x.get<std::string>() == t) return true;
        return false;
    };
    CHECK(hasTool("write_file"));
    CHECK(hasTool("read_file"));
    CHECK(cfg.value("tool_mode", std::string("")) == "auto");

    // (c) run skill_maker: creator writes a real mini-skill, verifier confirms
    ToolRegistry reg;
    { ToolDefinition d; d.name = "write_file"; d.description = "write";
      reg.registerTool(d, [](const std::string& args) {
          auto j = nlohmann::json::parse(args);
          std::string p = j.value("path", std::string(""));
          std::error_code ec;
          std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
          std::ofstream f(p);
          f << j.value("content", std::string(""));
          return std::string("written");
      }); }

    std::string sdir = root + "/mm_test";
    int cc = 0;
    auto creator = [&cc, sdir](const std::string&, const std::vector<Message>&) {
        int n = ++cc;
        if (n == 1) return makeCall("write_file", nlohmann::json{{"path", sdir + "/config.json"}, {"content", MINI_CONFIG}});
        if (n == 2) return makeCall("write_file", nlohmann::json{{"path", sdir + "/a.md"}, {"content", "You are A."}});
        if (n == 3) return makeCall("write_file", nlohmann::json{{"path", sdir + "/tools/emit.json"}, {"content", R"({"name":"emit","parameters":[{"name":"text","type":"string","required":true}]})"}});
        return makeCall("emit", nlohmann::json{{"summary", "wrote config.json, a.md, tools/emit.json"}});
    };
    auto verifier = [](const std::string&, const std::vector<Message>&) {
        return makeCall("emit", nlohmann::json{{"summary", "OK: mm_test"}});
    };

    std::vector<std::string> events;
    SkillRunner::Callbacks cbs;
    cbs.onEvent = [&events](const std::string& r, const std::string& k, const std::string& t) {
        events.push_back(r + "|" + k + "|" + t);
    };
    SkillRunner sr(testConfig(), &reg, cbs, "", root);
    sr.setResponder("creator", creator);
    sr.setResponder("verifier", verifier);
    sr.run("skill_maker", "make a hello-world skill");

    // files landed
    CHECK(std::filesystem::exists(sdir + "/config.json"));
    CHECK(std::filesystem::exists(sdir + "/a.md"));
    CHECK(std::filesystem::exists(sdir + "/tools/emit.json"));
    // the produced skill parses and is discoverable
    SkillConfig parsed; std::string err;
    CHECK(SkillConfig::loadFromJson(sdir + "/config.json", parsed, err));
    CHECK(parsed.name == "mm_test");
    bool discoverable = false;
    for (auto& n : SkillRunner::listSkills(root)) if (n == "mm_test") discoverable = true;
    CHECK(discoverable);
    // both agents reported
    bool ok = false;
    for (auto& e : events) if (e == "Verifier|message|OK: mm_test") ok = true;
    CHECK(ok);
}
