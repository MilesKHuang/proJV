// proJV -- tests for the headless event stream and (later) the session runtime.
//
// The event stream is a wire format: its tests are byte-level on purpose, because
// the determinism acceptance test of design 08 compares two runs byte for byte.
#include "doctest.h"

#include "core/events.h"

#include <cstdint>
#include <set>
#include <string>

using nlohmann::json;

TEST_SUITE("events") {

    TEST_CASE("every kind has a unique, non-empty name") {
        std::set<std::string> names;
        for (int i = 0; i < events::kKindCount; ++i) {
            const char* n = events::name(static_cast<events::Kind>(i));
            REQUIRE(n != nullptr);
            CHECK(std::string(n) != "unknown");
            CHECK(std::string(n).find(' ') == std::string::npos);
            names.insert(n);
        }
        CHECK(names.size() == static_cast<size_t>(events::kKindCount));
        // Out of range never crashes and never claims to be a real event.
        CHECK(std::string(events::name(events::Kind::Count)) == "unknown");
    }

    TEST_CASE("one event is exactly one line of JSON") {
        events::Event e(events::Kind::TextDelta, 7);
        e.text = "hi";
        const std::string line = events::toLine(e, false);
        CHECK(line == "{\"seq\":7,\"t\":\"text_delta\",\"text\":\"hi\"}\n");
        CHECK(line.find('\n') == line.size() - 1); // the only newline is the last byte
        CHECK(line.find('\r') == std::string::npos);
    }

    TEST_CASE("a multi-line payload stays on one line and round-trips") {
        events::Event e(events::Kind::Notice, 1);
        e.text = "line one\nline \"two\"\ttab";
        const std::string line = events::toLine(e, false);
        CHECK(line.find('\n') == line.size() - 1);
        const auto j = json::parse(line);
        CHECK(j["text"].get<std::string>() == "line one\nline \"two\"\ttab");
    }

    TEST_CASE("timestamps and token counts") {
        events::Event e(events::Kind::Usage, 2);
        e.prompt = 11;
        e.completion = 3;
        e.ts = 1700000000123;

        const auto off = json::parse(events::toLine(e, false));
        CHECK_FALSE(off.contains("ts")); // --no-ts must be byte-identical twice
        CHECK(off["prompt"].get<int>() == 11);

        const auto on = json::parse(events::toLine(e, true));
        CHECK(on["ts"].get<int64_t>() == 1700000000123);
    }

    TEST_CASE("turn_start carries the schema version, the others do not") {
        events::Event start(events::Kind::TurnStart, 1);
        start.turn = "1";
        start.model = "deepseek-v4-flash";
        start.session = "session_20260101_000000";
        const auto s = json::parse(events::toLine(start, false));
        CHECK(s["schema"].get<std::string>() == events::kSchema);
        CHECK(s["turn"].get<std::string>() == "1");
        CHECK(s["model"].get<std::string>() == "deepseek-v4-flash");

        events::Event delta(events::Kind::TextDelta, 2);
        delta.text = "x";
        CHECK_FALSE(json::parse(events::toLine(delta, false)).contains("schema"));
    }

    TEST_CASE("tool_result flags the diff, the diff event carries it") {
        events::Event result(events::Kind::ToolResult, 5);
        result.id = "call_1";
        result.name = "write_file";
        result.ok = true;
        result.hasDiff = true;
        result.text = "Wrote 12 lines";
        const auto r = json::parse(events::toLine(result, false));
        CHECK(r["has_diff"].get<bool>());
        CHECK(r["ok"].get<bool>());
        CHECK(r["name"].get<std::string>() == "write_file");
        CHECK(r["text"].get<std::string>() == "Wrote 12 lines");

        events::Event diff(events::Kind::Diff, 6);
        diff.id = "call_1";
        diff.path = "src/main.cpp";
        diff.text = "--- a/src/main.cpp\n+++ b/src/main.cpp\n";
        const auto d = json::parse(events::toLine(diff, false));
        CHECK(d["path"].get<std::string>() == "src/main.cpp");
        CHECK(d["text"].get<std::string>().find("+++ b/") != std::string::npos);
    }

    TEST_CASE("false, zero and empty fields are omitted") {
        events::Event e(events::Kind::ApprovalResult, 9);
        e.id = "a1";
        e.ok = false; // a refusal: no "ok" key, but the id must be there
        const auto j = json::parse(events::toLine(e, false));
        CHECK(j.contains("id"));
        CHECK_FALSE(j.contains("ok"));
        CHECK(j.contains("seq"));
        CHECK(j["seq"].get<int>() == 9);

        events::Event zero; // seq 0 -> omitted
        CHECK_FALSE(json::parse(events::toLine(zero, false)).contains("seq"));
    }

    TEST_CASE("task reports an exit code of zero") {
        events::Event e(events::Kind::Task, 3);
        e.id = "bg#1";
        e.activity = "done";
        e.exitCode = 0;
        e.hasExit = true; // 0 is meaningful for a process, unlike other numbers
        const auto j = json::parse(events::toLine(e, false));
        CHECK(j["exit"].get<int>() == 0);
        CHECK(j["activity"].get<std::string>() == "done");
    }

    TEST_CASE("todo carries the open count and the item lines") {
        events::Event e(events::Kind::Todo, 4);
        e.pending = 2;
        e.items = {"write the test", "run ctest"};
        const auto j = json::parse(events::toLine(e, false));
        CHECK(j["pending"].get<int>() == 2);
        CHECK(j["items"].size() == 2u);
    }

    TEST_CASE("the schema document names every kind") {
        const std::string doc = events::schemaText();
        CHECK(doc.find(events::kSchema) != std::string::npos);
        for (int i = 0; i < events::kKindCount; ++i) {
            CHECK(doc.find(events::name(static_cast<events::Kind>(i))) != std::string::npos);
        }
        CHECK(events::schemaTable().size() == static_cast<size_t>(events::kKindCount));
    }

} // TEST_SUITE("events")
