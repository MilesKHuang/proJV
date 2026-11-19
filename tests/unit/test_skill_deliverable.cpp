// proJV -- the skill deliverable contract.
//
// Three things must agree for a skill to hand over a report: the verb tool
// declares a boolean "finish" parameter, the config stops on that flag instead of
// on a magic word, and the role prompt tells the agent to use it. A mismatch is
// silent at runtime (the run simply never stops, or stops without a report), so
// it is checked here instead.
#include "doctest.h"

#include "core/skill_config.h"
#include "core/skill_text.h"

#include "json.hpp"

#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

const char* kSkills = PROJV_EXAMPLE_SKILLS_DIR;

} // namespace

TEST_SUITE("skill deliverable") {

    TEST_CASE("stop-words are not deliverables, short verdicts are") {
        CHECK(skill_text::isTrivialDeliverable(""));
        CHECK(skill_text::isTrivialDeliverable("DONE"));
        CHECK(skill_text::isTrivialDeliverable("done."));
        CHECK(skill_text::isTrivialDeliverable(" OK "));
        CHECK(skill_text::isTrivialDeliverable("Finished!"));
        // A short answer that carries a decision must survive: the rule is a stop-word
        // list, never a length test.
        CHECK_FALSE(skill_text::isTrivialDeliverable("ship it"));
        CHECK_FALSE(skill_text::isTrivialDeliverable("DONE: 3 files formatted"));
    }

    TEST_CASE("substantive means prose, not an acknowledgement") {
        const std::string report = "src/a.cpp - 3 lines formatted, one full-width comma replaced\n"
                                   "src/b.h   - no change\n";
        CHECK(skill_text::isSubstantive(report));
        CHECK_FALSE(skill_text::isSubstantive("DONE"));
        CHECK_FALSE(skill_text::isSubstantive("ok"));
        CHECK_FALSE(skill_text::isSubstantive("short"));
    }

    TEST_CASE("a verb call is trivial when every string it carries is a stop-word") {
        CHECK(skill_text::payloadIsTrivial(R"({"summary":"DONE"})"));
        CHECK(skill_text::payloadIsTrivial(R"({"summary":"done."})"));
        CHECK(skill_text::payloadIsTrivial("{}"));
        CHECK_FALSE(skill_text::payloadIsTrivial(R"({"summary":"dispatched 34 / reported 34"})"));
        // Non-JSON is judged as text, which is how a narration payload arrives.
        CHECK(skill_text::payloadIsTrivial("DONE"));
        CHECK_FALSE(skill_text::payloadIsTrivial("the report body"));
    }

    TEST_CASE("every shipped dispatch skill stops on the finish flag, not on a word") {
        struct Skill {
            const char* name;
            const char* promptFile;
        };
        const Skill skills[] = {
            {"monica", "supervisor.md"},
            {"musk", "musk.md"},
            {"avenger", "stark.md"},
        };
        for (const auto& s : skills) {
            CAPTURE(s.name);
            const std::string dir = std::string(kSkills) + "/" + s.name;

            SkillConfig cfg;
            std::string err;
            REQUIRE_MESSAGE(SkillConfig::loadFromJson(dir + "/config.json", cfg, err), err);
            CHECK(cfg.stopWhenFlag == "finish");
            CHECK(cfg.stopWhenValue.empty()); // the magic word is gone

            // The verb must declare the flag, or the model can never set it.
            const std::string tool = readFile(dir + "/tools/emit.json");
            REQUIRE_FALSE(tool.empty());
            auto tj = nlohmann::json::parse(tool, nullptr, false);
            REQUIRE(tj.is_object());
            bool declaresFinish = false;
            for (const auto& p : tj["parameters"])
                if (p.value("name", std::string()) == "finish" &&
                    p.value("type", std::string()) == "boolean")
                    declaresFinish = true;
            CHECK(declaresFinish);

            // And the prompt must tell the agent to use it, instead of the old word.
            const std::string prompt = readFile(dir + "/" + s.promptFile);
            REQUIRE_FALSE(prompt.empty());
            CHECK(prompt.find("finish: true") != std::string::npos);
            CHECK(prompt.find("exactly `DONE`") == std::string::npos);
        }
    }

    TEST_CASE("the legacy slot/equals form still parses") {
        // Skills that do not use the flag must keep working unchanged.
        const std::string path = "test_skill_deliverable_legacy.json";
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << R"({"name":"legacy","description":"x","max_rounds":2,
                 "stop_when":{"slot":"judgment","equals":"DONE"},
                 "agents":[{"id":"a","display_name":"A","prompt_file":"a.md"}],
                 "round_steps":[],"on_converge":[]})";
        }
        SkillConfig cfg;
        std::string err;
        const bool ok = SkillConfig::loadFromJson(path, cfg, err);
        std::remove(path.c_str());
        REQUIRE_MESSAGE(ok, err);
        CHECK(cfg.stopWhenSlot == "judgment");
        CHECK(cfg.stopWhenValue == "DONE");
        CHECK(cfg.stopWhenFlag.empty());
    }

} // TEST_SUITE("skill deliverable")
