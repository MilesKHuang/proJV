// view_model: the whole-screen contract (design 08.5, D4).
//
// Two jobs here:
//   1. pin the DERIVATION (bubble -> chat row, status line, status bar, "/" list)
//      so a change to it is a failing test instead of a quiet GUI regression;
//   2. cross-check the derivation against the TUI's own renderer: the labels this
//      file produces must appear verbatim in what chat_view draws for the same
//      bubbles. That is what makes "the GUI behaves like the TUI" machine-checked
//      rather than promised.
#include "doctest.h"

#include "core/speaker_slots.h"
#include "core/view_model.h"
#include "tui/chat_view.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>
#include <vector>

using bubble_model::Bubble;
using namespace view_model;

namespace {

Bubble mk(const std::string& role, const std::string& content) {
    Bubble b;
    b.role = role;
    b.content = content;
    return b;
}

Bubble mkSpeaker(const std::string& speaker, const std::string& content, bool vote) {
    Bubble b;
    b.role = "assistant";
    b.speaker = speaker;
    b.content = content;
    b.isVote = vote;
    return b;
}

Bubble mkThinking(const std::string& reasoning, bool withAnswer) {
    Bubble b;
    b.role = "assistant";
    b.hasReasoning = true;
    b.reasoningText = reasoning;
    if (withAnswer) b.content = "the answer";
    return b;
}

std::vector<ChatRow> rows(const std::vector<Bubble>& bubbles, bool expanded = false) {
    Source src;
    src.bubbles = bubbles;
    src.reasoningExpanded = expanded;
    return build(src).chat;
}

std::string renderTui(const std::vector<Bubble>& bubbles, bool expanded) {
    auto doc = chat_view::renderBubbles(bubbles, expanded);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(24));
    ftxui::Render(screen, doc);
    return screen.ToString();
}

} // namespace

TEST_CASE("view_model: chat labels match the TUI's own render") {
    std::vector<Bubble> bubbles = {
        mk("user", "hello world"),
        mk("assistant", "hi there"),
        mk("tool_call", "exec_shell: ls"),
        mk("tool_result", "file1"),
        mk("tool_result", "file2"),
        mk("system", "note"),
    };
    const std::string tui = renderTui(bubbles, false);
    const auto chat = rows(bubbles);

    REQUIRE(chat.size() == 4); // tool_call + both results collapse into one block
    CHECK(chat[0].kind == "user");
    CHECK(chat[1].kind == "assistant");
    CHECK(chat[2].kind == "tool");
    CHECK(chat[3].kind == "system");
    for (const auto& row : chat) {
        CHECK_MESSAGE(tui.find(row.label) != std::string::npos, row.label);
    }
    // The merged block carries both results and the separator between them.
    CHECK(chat[2].extra.find("file1") != std::string::npos);
    CHECK(chat[2].extra.find("file2") != std::string::npos);
}

TEST_CASE("view_model: a synthetic user round is labelled (auto)") {
    Bubble b = mk("user", "describe this image");
    b.synthetic = true;
    const auto chat = rows({b});
    REQUIRE(chat.size() == 1);
    CHECK(chat[0].label.find("(auto)") != std::string::npos);
}

TEST_CASE("view_model: thinking is a summary when collapsed and a tail when expanded") {
    std::string longReasoning;
    for (int i = 0; i < 40; ++i) longReasoning += "line " + std::to_string(i) + "\n";

    const auto collapsed = rows({mkThinking(longReasoning, false)}, false);
    REQUIRE(collapsed.size() == 1);
    CHECK(collapsed[0].kind == "thinking");
    CHECK(collapsed[0].reasoningSummary.find("F8 reveal") != std::string::npos);
    CHECK(collapsed[0].reasoningBody.empty());

    const auto expanded = rows({mkThinking(longReasoning, false)}, true);
    REQUIRE(expanded.size() == 1);
    CHECK(expanded[0].reasoningBody.find("line 39") != std::string::npos);
    CHECK(expanded[0].reasoningBody.find("line 0\n") == std::string::npos);
    CHECK(expanded[0].earlierOmitted);
}

TEST_CASE("view_model: an assistant answer with reasoning keeps both parts") {
    const auto chat = rows({mkThinking("because reasons", true)}, false);
    REQUIRE(chat.size() == 1);
    CHECK(chat[0].kind == "assistant");
    CHECK(chat[0].label.find("proJV") != std::string::npos);
    CHECK(chat[0].markdown);
    CHECK(!chat[0].reasoningSummary.empty());
    CHECK(chat[0].text == "the answer");
}

TEST_CASE("view_model: a tool result is capped at five lines") {
    std::string many;
    for (int i = 0; i < 12; ++i) many += "row" + std::to_string(i) + "\n";
    const auto chat = rows({mk("tool_call", "exec_shell: dump"), mk("tool_result", many)});
    REQUIRE(chat.size() == 1);
    CHECK(chat[0].extra.find("row4") != std::string::npos);
    CHECK(chat[0].extra.find("row6") == std::string::npos);
    CHECK(chat[0].extra.rfind("...") == chat[0].extra.size() - 3);
}

TEST_CASE("view_model: speaker, vote and dissent colours come from the theme table") {
    const auto agree = rows({mkSpeaker("Sheldon", "AGREE: yes", true)});
    REQUIRE(agree.size() == 1);
    CHECK(agree[0].speaker == "Sheldon");
    CHECK(agree[0].vote);
    CHECK(!agree[0].dissent);
    CHECK(agree[0].textColor == "todoDone");
    CHECK(agree[0].borderColor ==
          fieldName(speaker_slots::paletteField(speaker_slots::paletteSlot("Sheldon"))));

    const auto dissent = rows({mkSpeaker("Penny", "DISSENT: no", true)});
    REQUIRE(dissent.size() == 1);
    CHECK(dissent[0].dissent);
    CHECK(dissent[0].textColor == "phaseError");

    const auto plain = rows({mkSpeaker("Leonard", "**bold** words", false)});
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].markdown);
    CHECK(plain[0].label.find("vote") == std::string::npos);
}

TEST_CASE("view_model: a compacted-context system line is the warning colour") {
    const auto chat = rows({mk("system", "[Context compacted: 40 messages]")});
    REQUIRE(chat.size() == 1);
    CHECK(chat[0].labelColor == "todoInProgress");
}

TEST_CASE("view_model: the status line is status_line's own output") {
    Source src;
    src.status.state = AgentState::Thinking;
    src.status.reasoningText = "abc";
    const Screen s = build(src);
    CHECK(s.statusText.find("Reasoning...") != std::string::npos);
    CHECK(s.statusColor == "reasoningTextColor");

    Source exec;
    exec.status.state = AgentState::ExecutingTool;
    exec.status.currentToolName = "read_file";
    exec.status.toolProgressCurrent = 2;
    exec.status.toolProgressTotal = 5;
    const Screen s2 = build(exec);
    CHECK(s2.statusText == "Running: read_file (2/5)");
    CHECK(s2.statusColor == "statusRunning");
}

TEST_CASE("view_model: the status bar segments are status_bar's own output") {
    Source src;
    src.model = "deepseek-v4-flash";
    src.promptTokens = 1200;
    src.completionTokens = 300;
    src.msgCount = 7;
    src.toolCount = 2;
    const Screen s = build(src);
    REQUIRE(s.statusBar.size() >= 5);
    CHECK(s.statusBar[0].text == "deepseek-v4-flash");
    CHECK(s.statusBar[0].color == "statusModelName");
    CHECK(s.statusBar[1].text.find("1.20K") != std::string::npos);
    CHECK(s.statusBar[3].text.find("tools: 2") != std::string::npos);
}

TEST_CASE("view_model: the slash window follows the selection") {
    Source src;
    for (int i = 0; i < 16; ++i) src.slashRows.push_back({"/cmd" + std::to_string(i), "", "desc"});
    src.slashOpen = true;
    src.slashSelected = 0;
    Screen s = build(src);
    CHECK(s.slashOpen);
    CHECK(s.slashFirst == 0);
    CHECK(s.slashAbove == 0);
    CHECK(s.slashBelow == 6); // 16 rows, 10 visible

    src.slashSelected = 11;
    s = build(src);
    CHECK(s.slashFirst == 3); // 11-8 = 3, below the n-10=6 ceiling
    CHECK(s.slashBelow == 3); // rows 13..15

    src.slashSelected = 15;
    s = build(src);
    CHECK(s.slashFirst == 6); // clamped to n-10
    CHECK(s.slashBelow == 0);
}

TEST_CASE("view_model: an empty slash list never opens the menu") {
    Source src;
    src.slashOpen = true;
    const Screen s = build(src);
    CHECK(!s.slashOpen);
}

TEST_CASE("view_model: agent strip rows carry the TUI's cell width and tag") {
    Source src;
    AgentRow a;
    a.name = "Penny";
    a.tag = "[Penny]";
    a.activity = "working";
    a.detail = "reading the code";
    a.slot = 2;
    a.idle = false;
    src.agents.push_back(a);
    const Screen s = build(src);
    CHECK(!s.agentsEmpty);
    REQUIRE(s.agents.size() == 1);
    CHECK(s.agents[0].unitCols == 39);
    CHECK(s.agents[0].tag == "[Penny]");
    CHECK(s.agents[0].slot == 2);
}

TEST_CASE("view_model: the keycap bar is the TUI's thirteen hints, in order") {
    const auto caps = keycapHints();
    REQUIRE(caps.size() == 26); // key + label per hint
    CHECK(caps[0].text == "F1");
    CHECK(caps[1].text == "help");
    CHECK(caps[24].text == "Esc");
    CHECK(caps[25].text == "quit?");
}

TEST_CASE("view_model: todo rows are classified by their mark") {
    const auto r = todoRows("[ ] open thing\n[*] doing thing\n[x] done thing\nplain line");
    REQUIRE(r.size() == 4);
    CHECK(r[0].kind == "open");
    CHECK(r[1].kind == "inprogress");
    CHECK(r[2].kind == "done");
    CHECK(r[3].kind == "pending");
    CHECK(todoRows("").empty());
}

TEST_CASE("view_model: truncateToCols counts a CJK glyph as two columns") {
    // Clipped text always gets the TUI's ".." suffix (main_tui.cpp's rule).
    CHECK(truncateToCols("abcdef", 3) == "abc..");
    CHECK(truncateToCols("abcdef", 8) == "abcdef");
    const std::string cjk = "\xe4\xbd\xa0\xe5\xa5\xbd"; // two CJK glyphs
    CHECK(truncateToCols(cjk, 4) == cjk);
    // Three columns fit only the first glyph (2 columns), so the second is cut.
    CHECK(truncateToCols(cjk, 3) == cjk.substr(0, 3) + "..");
    CHECK(truncateToCols("abc" + cjk, 4) == "abc..");
}

TEST_CASE("view_model: lastNarrationLine keeps the newest line") {
    CHECK(lastNarrationLine("first\nsecond   \n") == "second");
    CHECK(lastNarrationLine("only") == "only");
    CHECK(lastNarrationLine("") == "");
}

TEST_CASE("view_model: the JSON snapshot is one object and is deterministic") {
    Source src;
    src.version = "1.2.3";
    src.modal = "approval";
    src.bubbles = {mk("user", "hi"), mk("assistant", "there")};
    src.input.text = "/wish the moon";
    src.todoText = "[*] thinking";
    src.approval.visible = true;
    src.approval.command = "del x";
    const Screen s = build(src);
    const std::string a = toJson(s);
    const std::string b = toJson(build(src));
    CHECK(a == b); // no clock, no pointer, no unordered container
    CHECK(a.find("\"version\": \"1.2.3\"") != std::string::npos);
    CHECK(a.find("\"modal\": \"approval\"") != std::string::npos);
    CHECK(a.find("\"command\": \"del x\"") != std::string::npos);
    CHECK(a.find("\"inprogress\"") != std::string::npos);
    CHECK(a.front() == '{');
    CHECK(a.back() == '\n');
}

TEST_CASE("view_model: UTF-8 survives the snapshot unescaped") {
    Source src;
    src.bubbles = {mk("user", "\xe4\xbd\xa0\xe5\xa5\xbd")};
    const std::string json = toJson(build(src));
    CHECK(json.find("\xe4\xbd\xa0\xe5\xa5\xbd") != std::string::npos);
}

TEST_CASE("speaker_slots: stable slot per name, six theme fields") {
    CHECK(speaker_slots::paletteCount() == 6);
    CHECK(speaker_slots::paletteSlot("") == 0);
    const std::size_t a = speaker_slots::paletteSlot("Sheldon");
    CHECK(a == speaker_slots::paletteSlot("Sheldon"));
    CHECK(a < speaker_slots::paletteCount());
    CHECK(fieldName(speaker_slots::paletteField(0)) == "phaseStreaming");
    // Out of range clamps instead of reading past the table.
    CHECK(fieldName(speaker_slots::paletteField(99)) == "phaseStreaming");
}
