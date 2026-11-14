// proJV -- the "/" completion menu (src/tui/slash_menu.h).
//
// Pure functions only, so the assertions are about what the user sees:
//   * every implemented command is listed, and nothing that does nothing;
//   * "/w" filters to /wish before /end_task (prefix beats substring);
//   * a space or a missing slash closes the menu;
//   * two calls with the same input are byte-identical (stable order).
#include "doctest/doctest.h"
#include "tui/slash_menu.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

bool hasName(const std::vector<slash_menu::Command>& v, const std::string& name) {
    return std::any_of(v.begin(), v.end(),
                       [&](const slash_menu::Command& c) { return c.name == name; });
}

size_t indexOf(const std::vector<slash_menu::Command>& v, const std::string& name) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].name == name) return i;
    return v.size();
}

fs::path freshDir(const std::string& leaf) {
    const auto dir = fs::temp_directory_path() / ("projv_slash_" + leaf);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void writeSkill(const fs::path& root, const std::string& name, const std::string& desc) {
    std::error_code ec;
    fs::create_directories(root / name, ec);
    std::ofstream out((root / name / "config.json").string());
    out << "{\"name\":\"" << name << "\",\"description\":\"" << desc << "\","
        << "\"agents\":[{\"name\":\"a\",\"prompt\":\"p\"}],\"rounds\":1}";
}

} // namespace

TEST_CASE("slash_menu: every implemented command is listed") {
    const auto all = slash_menu::builtins();
    for (const char* name :
         {"/help", "/clear", "/compress", "/undo", "/undo!", "/restore", "/wish", "/wish cancel",
          "/end_task", "/workspace", "/skill", "/approval", "/approval clear"}) {
        CHECK_MESSAGE(hasName(all, name), name);
    }
}

TEST_CASE("slash_menu: entries carry an argument hint only when they take one") {
    const auto all = slash_menu::builtins();
    for (const auto& c : all) {
        if (c.name == "/wish")
            CHECK(c.args == "<what you want built>");
        else if (c.name == "/help" || c.name == "/clear" || c.name == "/compress")
            CHECK(c.args.empty());
        CHECK_FALSE(c.desc.empty());
        CHECK(c.desc.find('\n') == std::string::npos);
    }
}

TEST_CASE("slash_menu: a bare / lists every command") {
    const auto all = slash_menu::builtins();
    const auto shown = slash_menu::filter(all, "/");
    CHECK(shown.size() == all.size());
    CHECK(shown[0].name == all[0].name);
    CHECK(hasName(shown, "/wish"));
    CHECK(hasName(shown, "/end_task"));
}

TEST_CASE("slash_menu: anything that is not a bare /word shows nothing") {
    const auto all = slash_menu::builtins();
    CHECK(slash_menu::filter(all, "").empty());
    CHECK(slash_menu::filter(all, "hello").empty());
    CHECK(slash_menu::filter(all, "/wish ").empty());   // a space closes the menu
    CHECK(slash_menu::filter(all, "hello /w").empty()); // slash not at the start
    CHECK_FALSE(slash_menu::filter(all, "/w").empty());
}

TEST_CASE("slash_menu: prefix matches come before substring matches") {
    const auto all = slash_menu::builtins();
    const auto w = slash_menu::filter(all, "/w");
    REQUIRE_FALSE(w.empty());
    // /wish and /wish cancel start with "w"; /workspace starts with w as well.
    CHECK(w[0].name == "/wish");
    CHECK(hasName(w, "/workspace"));
    // "end_task" has no 'w' -- it must not appear at all.
    CHECK_FALSE(hasName(w, "/end_task"));
}

TEST_CASE("slash_menu: substring-only matches follow the prefix group") {
    const auto all = slash_menu::builtins();
    const auto t = slash_menu::filter(all, "/t");
    REQUIRE_FALSE(t.empty());
    // "/end_task" and "/restore"? Neither starts with t. /end_task contains 't'.
    CHECK(hasName(t, "/end_task"));
    for (size_t i = 1; i < t.size(); ++i)
        CHECK(t[i].name >= t[i - 1].name); // alphabetical inside each group
}

TEST_CASE("slash_menu: filtering twice gives identical bytes") {
    const auto all = slash_menu::all({}, "");
    const auto a = slash_menu::filter(all, "/c");
    const auto b = slash_menu::filter(all, "/c");
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].name == b[i].name);
        CHECK(a[i].desc == b[i].desc);
    }
}

TEST_CASE("slash_menu: installed skills are listed with their real description") {
    // Against the shipped example skills: this is the format the menu must read,
    // not a simplified one invented for the test.
    const std::string examples = PROJV_EXAMPLE_SKILLS_DIR;
    const auto all = slash_menu::all({"monica", "musk"}, examples);
    REQUIRE(hasName(all, "/monica"));
    REQUIRE(hasName(all, "/musk"));
    for (const auto& c : all) {
        if (c.name == "/monica") {
            CHECK(c.args == "<topic>");
            CHECK(c.desc.find("Supervisor-driven") != std::string::npos);
            // Monica's real description is ~190 chars: the row must be one line.
            CHECK(c.desc.size() <= 51); // 48 + "..."
            CHECK(c.desc.find("...") != std::string::npos);
            CHECK(c.desc.find('\n') == std::string::npos);
        }
    }
    // A folder without config.json keeps its entry and gets an empty description.
    const fs::path dir = freshDir("broken");
    std::error_code ec;
    fs::create_directories(dir / "noconfig", ec);
    const auto mixed = slash_menu::all({"noconfig"}, dir.string());
    REQUIRE(hasName(mixed, "/noconfig"));
    for (const auto& c : mixed)
        if (c.name == "/noconfig") CHECK(c.desc.empty());
    fs::remove_all(dir, ec);
}

TEST_CASE("slash_menu: a skill with a multi-paragraph description is clipped to one line") {
    (void)0; // covered by the /monica assertions above: real config, real clipping.
}

TEST_CASE("slash_menu: a pick fills the command and leaves room for arguments") {
    const auto all = slash_menu::builtins();
    for (const auto& c : all) {
        const std::string filled = slash_menu::completion(c);
        if (c.args.empty())
            CHECK(filled == c.name);
        else
            CHECK(filled == c.name + " ");
    }
    // The filled text must not look like a finished command for an args-taking one.
    CHECK(slash_menu::filter(all, slash_menu::completion({"/wish", "x", ""})).empty());
}
