// proJV -- whole-screen view model (see view_model.h).
#include "view_model.h"

#include "json.hpp"
#include "core/speaker_slots.h"
#include "core/session.h"
#include "tui/status_bar.h"
#include "tui/status_line.h"

#include <algorithm>
#include <cstdio>

namespace view_model {
namespace {

// --- chat_view.cpp's display constants, kept identical on purpose ----------
constexpr int kReasoningTailLines = 18; // visible tail of a long chain of thought
constexpr int kToolResultLines = 5;     // a tool block shows this many result lines
constexpr int kDiffMaxRows = 40;        // a diff block's row cap (studio draws it)
constexpr int kSlashVisible = 10;       // rows of the "/" menu window

int countLines(const std::string& text) {
    int n = 1;
    for (char c : text)
        if (c == '\n') ++n;
    return n;
}

// Trailing window of `text`: the newest lines are what the reader wants.
std::string tailLines(const std::string& text, int maxLines) {
    const int total = countLines(text);
    if (total <= maxLines) return text;
    const int skip = total - maxLines;
    size_t pos = 0;
    for (int i = 0; i < skip; ++i) {
        pos = text.find('\n', pos);
        if (pos == std::string::npos) break;
        ++pos;
    }
    return text.substr(pos);
}

// Collapse runs of blank lines: a streamed reasoning block arrives loose.
std::string collapseBlankLines(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    int nl = 0;
    for (char c : text) {
        if (c == '\n') {
            ++nl;
            if (nl <= 2) out += c;
        } else if (c == '\r') {
            continue;
        } else {
            nl = 0;
            out += c;
        }
    }
    return out;
}

// "Thinking (1234 chars, 42 lines) . last line . F8 reveal"
std::string reasoningSummary(const std::string& text) {
    std::string s = "Thinking (" + std::to_string(text.size()) + " chars, " +
                    std::to_string(countLines(text)) + " lines)";
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == ' ' ||
                       text[end - 1] == '\t')) {
        --end;
    }
    size_t start = (end == 0) ? 0 : text.rfind('\n', end - 1);
    start = (start == std::string::npos) ? 0 : start + 1;
    std::string lastLine = text.substr(start, end - start);
    if (lastLine.size() > 60) lastLine = lastLine.substr(0, 57) + "...";
    if (!lastLine.empty()) s += " | " + lastLine;
    s += " | F8 reveal";
    return s;
}

// A tool result block larger than kToolResultLines is cut at the cap.
std::string capToolResult(const std::string& results) {
    int nl = 0;
    for (char c : results)
        if (c == '\n') ++nl;
    if (nl < kToolResultLines) return results;
    int found = 0;
    size_t pos = 0;
    while (found < kToolResultLines && pos < results.size()) {
        pos = results.find('\n', pos);
        if (pos == std::string::npos) break;
        ++pos;
        ++found;
    }
    return results.substr(0, pos) + "...";
}

std::string paletteFieldName(const std::string& speakerRole) {
    if (speakerRole.empty()) return fieldName(&ThemeColors::border);
    return fieldName(speaker_slots::paletteField(speaker_slots::paletteSlot(speakerRole)));
}

// One assistant/speaker thinking section, shared by the two branches that draw it.
void addThinking(ChatRow& row, const std::string& reasoningText, bool expanded) {
    row.reasoningExpanded = expanded;
    if (expanded) {
        row.earlierOmitted = countLines(reasoningText) > kReasoningTailLines;
        row.reasoningBody =
            collapseBlankLines(tailLines(reasoningText, kReasoningTailLines));
    } else {
        row.reasoningSummary = reasoningSummary(reasoningText);
    }
}

} // namespace

std::string truncateToCols(const std::string& s, int maxCols) {
    int cols = 0;
    size_t i = 0, last = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        int w = 1;
        if (c >= 0xF0) {
            len = 4;
            w = 2;
        } else if (c >= 0xE0) {
            len = 3;
            w = 2;
        } else if (c >= 0xC0) {
            len = 2;
            w = 1;
        }
        if (cols + w > maxCols) break;
        cols += w;
        i += len;
        last = i;
    }
    std::string out = s.substr(0, last);
    if (last < s.size()) out += "..";
    return out;
}

std::string lastNarrationLine(const std::string& status) {
    std::string st = status;
    while (!st.empty() && (st.back() == '\n' || st.back() == '\r' || st.back() == ' '))
        st.pop_back();
    const auto nl = st.rfind('\n');
    if (nl != std::string::npos) st = st.substr(nl + 1);
    return st;
}

std::string fieldName(const std::string ThemeColors::*field) {
    if (!field) return std::string();
    for (const auto& f : themeColorFields()) {
        if (f.member == field) return f.name;
    }
    return std::string();
}

const char* busyRefusal() {
    // Wording is deliberately a statement of fact, not an apology: the key DID
    // something (it said why), and the turn is the thing in the way.
    return "busy -- finish the current turn first";
}

std::vector<TodoRow> todoRows(const std::string& pendingTodo) {
    std::vector<TodoRow> rows;
    if (pendingTodo.empty()) return rows;
    size_t pos = 0;
    while (pos <= pendingTodo.size()) {
        size_t nl = pendingTodo.find('\n', pos);
        if (nl == std::string::npos) nl = pendingTodo.size();
        const std::string line = pendingTodo.substr(pos, nl - pos);
        TodoRow r;
        r.text = line;
        if (line.find("[x]") != std::string::npos)
            r.kind = "done";
        else if (line.find("[*]") != std::string::npos)
            r.kind = "inprogress";
        else if (line.find("[ ]") != std::string::npos)
            r.kind = "open";
        else
            r.kind = "pending";
        rows.push_back(std::move(r));
        if (nl == pendingTodo.size()) break;
        pos = nl + 1;
    }
    return rows;
}

std::vector<Segment> keycapHints() {
    // The TUI's bottom bar, in its order. Two segments per entry (the keycap and
    // its label) so a front-end can draw the cap with its own background.
    return {
        {"F1", "key"},    {"help", "label"},   {"F2", "key"},    {"cfg", "label"},
        {"F3", "key"},    {"new", "label"},    {"F4", "key"},    {"open", "label"},
        {"F5", "key"},    {"mcp", "label"},    {"F6", "key"},    {"theme", "label"},
        {"F7", "key"},    {"edit", "label"},   {"F8", "key"},    {"reveal", "label"},
        {"F9", "key"},    {"dock", "label"},   {"F10", "key"},   {"model", "label"},
        {"\xe2\x86\x91\xe2\x86\x93", "key"},   {"scroll", "label"},
        {"Enter", "key"}, {"send", "label"},   {"Esc", "key"},   {"quit?", "label"},
    };
}

namespace {

// "──" is U+2500 BOX DRAWINGS LIGHT HORIZONTAL, written as escapes so this file
// stays ASCII while the label BYTES match tui/chat_view.cpp exactly (the parity
// test compares the two strings, not their appearance).
constexpr const char* kRule = "\xe2\x94\x80\xe2\x94\x80";
constexpr const char* kMidDot = "\xc2\xb7"; // U+00B7

std::string label(const std::string& what, const std::string& suffix = std::string()) {
    return std::string(kRule) + " " + what + suffix + " " + kRule;
}

// chat_view.cpp's separator between merged tool results.
constexpr const char* kToolResultSep = "\n--------------------\n";

std::string phaseNameOf(AgentPhase p) {
    switch (p) {
    case AgentPhase::Streaming:
        return "Streaming";
    case AgentPhase::ExecutingTools:
        return "ExecutingTools";
    case AgentPhase::AwaitApproval:
        return "AwaitApproval";
    case AgentPhase::Error:
        return "Error";
    case AgentPhase::Idle:
    default:
        return "Idle";
    }
}

std::string phaseColorOf(AgentPhase p) {
    switch (p) {
    case AgentPhase::Streaming:
        return fieldName(&ThemeColors::phaseStreaming);
    case AgentPhase::ExecutingTools:
        return fieldName(&ThemeColors::phaseExecutingTools);
    case AgentPhase::AwaitApproval:
        return fieldName(&ThemeColors::phaseAwaitApproval);
    case AgentPhase::Error:
        return fieldName(&ThemeColors::phaseError);
    case AgentPhase::Idle:
    default:
        return fieldName(&ThemeColors::phaseIdle);
    }
}

// The line a folded row shows: the first call of the block.
// "850ms" / "12.4s" / "3m12s" -- a tool row's own clock, never a wall clock.
std::string formatDuration(int64_t ms) {
    if (ms <= 0) return std::string();
    if (ms < 1000) return std::to_string(ms) + "ms";
    if (ms < 60000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(ms) / 1000.0);
        return buf;
    }
    return std::to_string(ms / 60000) + "m" + std::to_string((ms % 60000) / 1000) + "s";
}

std::string firstLine(const std::string& text) {
    const size_t nl = text.find('\n');
    return nl == std::string::npos ? text : text.substr(0, nl);
}

// "+12 -3" for the diffs a tool row carries. File headers ("--- a/x", "+++ b/x")
// are not line changes and a hunk header is not one either, so only a leading
// '+'/'-' that is not a three-dash header counts.
std::string diffSummaryOf(const std::string& diffs) {
    if (diffs.empty()) return std::string();
    int added = 0, removed = 0, files = 0;
    size_t pos = 0;
    while (pos <= diffs.size()) {
        size_t nl = diffs.find('\n', pos);
        if (nl == std::string::npos) nl = diffs.size();
        const std::string line = diffs.substr(pos, nl - pos);
        if (line.rfind("+++ ", 0) == 0)
            ++files;
        else if (line.rfind("--- ", 0) == 0)
            ; // the other half of the same header, already counted
        else if (!line.empty() && line[0] == '+')
            ++added;
        else if (!line.empty() && line[0] == '-')
            ++removed;
        if (nl == diffs.size()) break;
        pos = nl + 1;
    }
    std::string s = "+" + std::to_string(added) + " -" + std::to_string(removed);
    if (files > 1) s += std::string(" ") + kMidDot + " " + std::to_string(files) + " files";
    return s;
}

bool containsKey(const std::vector<std::string>& keys, const std::string& key) {
    if (key.empty()) return false;
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// The bubble -> chat-row mapping. This is chat_view.cpp's renderBubbles(), rule
// for rule: the same labels, the same colour reference for each, the same
// merging of tool results into the call that produced them, the same caps.
//
// The folding fields are derived HERE, not in the front-end: the merge rule that
// decides which bubble belongs to which row lives in this function, so a second
// copy of it in studio/ would be the drift this file exists to prevent.
std::vector<ChatRow> deriveChat(const Source& src) {
    const std::vector<bubble_model::Bubble>& bubbles = src.bubbles;
    const bool reasoningExpanded = src.reasoningExpanded;
    std::vector<ChatRow> rows;
    const std::string cText = fieldName(&ThemeColors::text);
    const std::string cBorder = fieldName(&ThemeColors::border);
    const std::string cTool = fieldName(&ThemeColors::toolTitleColor);
    const std::string cToolResult = fieldName(&ThemeColors::toolResultText);
    const std::string cReasoning = fieldName(&ThemeColors::reasoningTextColor);
    const std::string cReasoningBody = fieldName(&ThemeColors::reasoningBodyText);
    const std::string cWarning = fieldName(&ThemeColors::todoInProgress);

    // One speaker block, shared by the flat branch and by a skill-run container's
    // children: two copies of "which role, vote, colour" would drift apart.
    const auto fillSpeaker = [&](ChatRow& row, const bubble_model::Bubble& b) {
        row.kind = "speaker";
        row.speaker = b.speaker;
        row.vote = b.isVote;
        row.label = label(b.speaker, b.isVote ? std::string(" ") + kMidDot + " vote" : "");
        row.labelColor = paletteFieldName(b.speaker);
        row.borderColor = paletteFieldName(b.speaker);
        row.text = b.content;
        if (b.isVote) {
            row.dissent = b.content.rfind("DISSENT", 0) == 0;
            row.bold = true;
            row.textColor = row.dissent ? fieldName(&ThemeColors::phaseError)
                                        : fieldName(&ThemeColors::todoDone);
        } else {
            row.markdown = true;
        }
    };
    // Which skill run a container belongs to, counted in transcript order: the
    // header says "run #2" without anything extra being persisted.
    std::vector<std::string> runOrder;

    const int total = static_cast<int>(bubbles.size());
    for (int i = 0; i < total; ++i) {
        const auto& b = bubbles[i];
        ChatRow row;
        row.borderColor = cBorder;
        row.textColor = cText;
        row.labelColor = cText;
        row.extraColor = cToolResult;
        row.synthetic = b.synthetic;
        // The key travels beside `bubbles`: a front-end that folds rows needs an
        // identity that survives streaming appends and the oldest-200 trim, and a
        // row index is neither.
        row.key = i < static_cast<int>(src.bubbleKeys.size())
                      ? src.bubbleKeys[static_cast<size_t>(i)]
                      : std::string();

        // A skill run's role messages: consecutive ones from the same run and the
        // same role become ONE collapsible block. The grouping key is the id the
        // run stamped on every message it wrote, so a replayed session groups
        // exactly as the live one did -- and the id is not an index, so streaming
        // appends cannot re-group what is already on screen.
        if (b.role == "assistant" && !b.speaker.empty() && !b.skillRunId.empty()) {
            ChatRow box;
            box.borderColor = paletteFieldName(b.speaker);
            box.labelColor = paletteFieldName(b.speaker);
            box.textColor = cText;
            box.extraColor = cToolResult;
            box.kind = "skillrun";
            box.speaker = b.speaker;
            box.key = "s:" + b.skillRunId + ":" + b.speaker;
            int j = i;
            while (j < total) {
                const auto& c = bubbles[j];
                if (c.role != "assistant" || c.speaker != b.speaker || c.skillRunId != b.skillRunId)
                    break;
                ChatRow child;
                child.borderColor = paletteFieldName(c.speaker);
                child.textColor = cText;
                child.labelColor = paletteFieldName(c.speaker);
                child.extraColor = cToolResult;
                child.key = j < static_cast<int>(src.bubbleKeys.size())
                                ? src.bubbleKeys[static_cast<size_t>(j)]
                                : std::string();
                fillSpeaker(child, c);
                box.children.push_back(std::move(child));
                ++j;
            }
            // "run #2" is counted in transcript order: nothing extra to persist.
            if (std::find(runOrder.begin(), runOrder.end(), b.skillRunId) == runOrder.end())
                runOrder.push_back(b.skillRunId);
            const int ordinal = static_cast<int>(std::find(runOrder.begin(), runOrder.end(),
                                                           b.skillRunId) -
                                                 runOrder.begin()) +
                                1;
            box.label = label(b.speaker);
            box.summary = b.speaker + std::string(" ") + kMidDot + " run #" +
                          std::to_string(ordinal) + std::string(" ") + kMidDot + " " +
                          std::to_string(box.children.size()) +
                          (box.children.size() == 1 ? " msg" : " msgs");
            box.collapsible = true;
            box.collapsed = src.skillsCollapsed && !containsKey(src.expandedKeys, box.key);
            i = j - 1;
            rows.push_back(std::move(box));
            continue;
        }

        if (b.role == "user") {
            row.kind = "user";
            row.label = label(b.synthetic ? "You (auto)" : "You");
            row.text = b.content;
        } else if (b.role == "assistant" && b.hasReasoning && b.content.empty()) {
            row.kind = "thinking";
            row.label = label("Thinking");
            row.labelColor = cReasoning;
            row.textColor = cReasoningBody;
            addThinking(row, b.reasoningText, reasoningExpanded);
        } else if (b.role == "assistant" && !b.speaker.empty()) {
            fillSpeaker(row, b);
        } else if (b.role == "assistant") {
            row.kind = "assistant";
            if (b.hasReasoning) addThinking(row, b.reasoningText, reasoningExpanded);
            if (!b.content.empty()) {
                row.label = label("proJV");
                row.text = b.content;
                row.markdown = true;
            }
        } else if (b.role == "diff") {
            row.kind = "diff";
            row.text = b.content;
            // diff_view draws its own "── Diff ──" title in the tool colour.
            row.label = label("Diff");
            row.labelColor = cTool;
            row.textColor = cText;
        } else if (b.role == "tool_call") {
            row.kind = "tool";
            row.label = label("Tool");
            row.labelColor = cTool;
            row.text = b.content;
            row.textColor = cTool;
            // Absorb BOTH kinds that belong to this call: its results and the
            // diff a writing tool produced. Absorbing only tool_result stopped
            // the loop dead on the diff, so with two calls in one turn the
            // second result fell out as a stray "Result" card and each diff
            // became a card of its own -- one call turned into four cards.
            std::string results; // the compact one-liners (what the TUI draws)
            std::string bodies;  // the real result texts (what an open row shows)
            std::string diffs;
            bool anyError = false;
            int resultCount = 0;
            int64_t totalMs = 0;
            while (i + 1 < total) {
                const auto& next = bubbles[i + 1];
                if (next.role == "tool_result") {
                    if (!results.empty()) results += kToolResultSep;
                    results += next.content;
                    if (!bodies.empty()) bodies += kToolResultSep;
                    bodies += next.body.empty() ? next.content : next.body;
                    if (next.durationMs > 0) totalMs += next.durationMs;
                    // The persistence layer writes "[Error] " for a failed call
                    // (models.cpp, Message::Tool), and storage keeps the content,
                    // so the marker survives a replay.
                    if (next.content.find("[Error]") != std::string::npos) anyError = true;
                    ++resultCount;
                    ++i;
                } else if (next.role == "diff") {
                    if (!diffs.empty()) diffs += "\n";
                    diffs += next.content;
                    ++i;
                } else {
                    break;
                }
            }
            row.extra = capToolResult(results);
            row.extraFull = bodies;
            row.lines = resultCount > 0 ? countLines(bodies) : 0;
            row.diffText = diffs;
            row.diffSummary = diffSummaryOf(diffs);
            row.durationText = formatDuration(totalMs);
            row.status = resultCount == 0 ? std::string("pending")
                                          : (anyError ? std::string("failed") : std::string("ok"));
            row.summary = firstLine(b.content);
            row.collapsible = resultCount > 0 || !diffs.empty();
            if (row.collapsible)
                row.collapsed = src.toolsCollapsed && !containsKey(src.expandedKeys, row.key);
        } else if (b.role == "tool_result") {
            row.kind = "result";
            row.label = label("Result");
            row.labelColor = cToolResult;
            row.text = b.content;
            row.textColor = cToolResult;
        } else if (b.role == "system") {
            row.kind = "system";
            row.label = label("System");
            const bool compacted = b.content.find("[Context compacted:") != std::string::npos;
            row.labelColor = compacted ? cWarning : cText;
            row.text = b.content;
        } else {
            row.kind = "text";
            row.text = b.content;
        }
        rows.push_back(std::move(row));
    }
    // A run that is still writing messages is the last block on screen and the
    // agent is busy: that is the only "in progress" the view model can state
    // without reading a clock.
    if (src.busy && !rows.empty() && rows.back().kind == "skillrun")
        rows.back().status = "running";
    return rows;
}

} // namespace

std::string phaseName(AgentPhase p) {
    return phaseNameOf(p);
}

namespace {

StreamingView deriveStreaming(const AgentPhase phase, const AgentStatus& st) {
    StreamingView v;
    if (phase != AgentPhase::Streaming) return v;
    if (st.streamingText.empty() && st.reasoningText.empty()) return v;
    v.visible = true;
    if (!st.reasoningText.empty()) {
        v.hasReasoning = true;
        v.reasoningHead = label("Thinking");
        v.earlierOmitted = countLines(st.reasoningText) > kReasoningTailLines;
        v.reasoningBody = collapseBlankLines(tailLines(st.reasoningText, kReasoningTailLines));
    }
    if (!st.streamingText.empty()) {
        v.hasContent = true;
        v.contentHead = label("proJV");
        v.content = st.streamingText;
    }
    return v;
}

} // namespace

Screen build(const Source& src) {
    Screen s;
    s.version = src.version;
    s.modal = src.modal;
    s.dock = src.dock;
    s.phaseName = phaseNameOf(src.phase);
    s.phaseColor = phaseColorOf(src.phase);
    s.busy = src.busy;
    s.spinnerFrame = src.spinnerFrame;
    s.model = src.model;
    s.workspace = src.workspace;
    s.roleName = src.roleName;
    s.roleIndex = src.roleIndex;
    s.promptFiles = src.promptFiles;
    s.queued = src.queued;

    s.chat = deriveChat(src);
    s.streaming = deriveStreaming(src.phase, src.status);

    s.input = src.input;

    // The "/" menu: a scrolling window that follows the selection, not a hard
    // cut at the first N rows (the arrow keys must be able to reach every row).
    s.slashRows = src.slashRows;
    s.slashOpen = src.slashOpen && !src.slashRows.empty();
    s.slashSelected = src.slashSelected;
    const int n = static_cast<int>(src.slashRows.size());
    int first = 0;
    if (n > kSlashVisible) {
        first = src.slashSelected - (kSlashVisible - 2);
        first = std::max(0, std::min(first, n - kSlashVisible));
    }
    const int last = std::min(n, first + kSlashVisible);
    s.slashFirst = first;
    s.slashAbove = first;
    s.slashBelow = n - last;

    s.agents = src.agents;
    s.agentsEmpty = src.agents.empty();
    s.tasks = src.tasks;
    s.tasksEmpty = src.tasks.empty();

    s.todoTitle = "TODO";
    s.todo = todoRows(src.todoText);
    s.todoEmpty = s.todo.empty();

    // Status line: exactly status_line::render(), so the text and the colour
    // reference cannot drift from the TUI's.
    {
        const status_line::Line line = status_line::render(src.status);
        s.statusText = line.text;
        s.statusColor = fieldName(line.color);
        s.statusSpinner = src.busy;
        s.skillStatus = src.skillStatus;
    }

    // Status bar: status_bar::renderSegments() fed from the same numbers.
    {
        status_bar::Data d;
        d.model = src.model;
        d.promptTokens = src.promptTokens;
        d.completionTokens = src.completionTokens;
        d.msgCount = static_cast<size_t>(src.msgCount);
        d.toolCount = src.toolCount;
        d.status = src.status;
        d.pressure = static_cast<Session::PressureLevel>(src.pressure);
        d.estimatedTokens = src.estimatedTokens;
        d.windowTokens = src.windowTokens;
        d.workspace = src.workspace;
        d.roleName = src.roleName;
        d.cost = src.cost;
        for (const auto& seg : status_bar::renderSegments(d))
            s.statusBar.push_back({seg.text, fieldName(seg.color)});
    }

    s.keycaps = keycapHints();

    s.approval = src.approval;
    s.config = src.config;
    s.mcp = src.mcp;
    s.open = src.open;
    s.modelList = src.modelList;
    s.themeList = src.themeList;
    s.themeEditor = src.themeEditor;
    s.help = src.help;
    s.quit = src.quit;
    s.welcome = src.welcome;
    return s;
}

namespace {

nlohmann::json chatRowJson(const ChatRow& r) {
    nlohmann::json j;
    j["kind"] = r.kind;
    j["label"] = r.label;
    j["label_color"] = r.labelColor;
    j["border_color"] = r.borderColor;
    j["speaker"] = r.speaker;
    j["text"] = r.text;
    j["text_color"] = r.textColor;
    j["markdown"] = r.markdown;
    j["bold"] = r.bold;
    j["extra"] = r.extra;
    j["extra_color"] = r.extraColor;
    j["reasoning_head"] = r.reasoningHead;
    j["reasoning_body"] = r.reasoningBody;
    j["reasoning_summary"] = r.reasoningSummary;
    j["reasoning_expanded"] = r.reasoningExpanded;
    j["earlier_omitted"] = r.earlierOmitted;
    j["vote"] = r.vote;
    j["dissent"] = r.dissent;
    j["synthetic"] = r.synthetic;
    j["key"] = r.key;
    j["collapsible"] = r.collapsible;
    j["collapsed"] = r.collapsed;
    j["summary"] = r.summary;
    j["status"] = r.status;
    j["diff_summary"] = r.diffSummary;
    j["diff_text"] = r.diffText;
    j["extra_full"] = r.extraFull;
    j["lines"] = r.lines;
    j["duration_text"] = r.durationText;
    if (!r.children.empty()) {
        nlohmann::json kids = nlohmann::json::array();
        for (const auto& c : r.children) kids.push_back(chatRowJson(c));
        j["children"] = kids;
    }
    return j;
}

nlohmann::json segmentsJson(const std::vector<Segment>& segs) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& s : segs) out.push_back({{"text", s.text}, {"color", s.color}});
    return out;
}

nlohmann::json rowsJson(const std::vector<std::string>& rows) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& r : rows) out.push_back(r);
    return out;
}

nlohmann::json approvalJson(const ApprovalPanel& p) {
    return {{"visible", p.visible},
            {"title", p.title},
            {"intro", p.intro},
            {"command", p.command},
            {"targets", rowsJson(p.targets)},
            {"more_targets", p.moreTargets},
            {"explain", p.explain},
            {"undo", p.undo},
            {"show_always", p.showAlways},
            {"buttons", rowsJson(p.buttons)}};
}

nlohmann::json configJson(const ConfigPanel& p) {
    nlohmann::json fields = nlohmann::json::array();
    for (const auto& f : p.fields) {
        fields.push_back({{"label", f.label},
                          {"value", f.value},
                          {"kind", f.kind},
                          {"options", rowsJson(f.options)},
                          {"index", f.index},
                          {"checked", f.checked}});
    }
    return {{"visible", p.visible},
            {"title", p.title},
            {"hint", p.hint},
            {"fields", fields},
            {"buttons", rowsJson(p.buttons)}};
}

nlohmann::json mcpJson(const McpPanel& p) {
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& r : p.rows) {
        rows.push_back({{"enabled", r.enabled},
                        {"trusted", r.trusted},
                        {"name", r.name},
                        {"protocol", r.protocol},
                        {"state", r.state},
                        {"tools", r.tools}});
    }
    return {{"visible", p.visible},
            {"rows", rows},
            {"selected", p.selected},
            {"purpose", p.purpose},
            {"error", p.error},
            {"hint", p.hint},
            {"buttons", rowsJson(p.buttons)}};
}

nlohmann::json listJson(const ListPanel& p) {
    return {{"visible", p.visible},
            {"title", p.title},
            {"hint", p.hint},
            {"rows", rowsJson(p.rows)},
            {"selected", p.selected}};
}

} // namespace

std::string toJson(const Screen& s) {
    nlohmann::json chat = nlohmann::json::array();
    for (const auto& r : s.chat) chat.push_back(chatRowJson(r));

    nlohmann::json agents = nlohmann::json::array();
    for (const auto& a : s.agents) {
        agents.push_back({{"tag", a.tag},
                          {"name", a.name},
                          {"activity", a.activity},
                          {"detail", a.detail},
                          {"slot", a.slot},
                          {"idle", a.idle},
                          {"unit_cols", a.unitCols}});
    }
    nlohmann::json tasks = nlohmann::json::array();
    for (const auto& t : s.tasks) {
        tasks.push_back({{"tag", t.tag},
                         {"activity", t.activity},
                         {"detail", t.detail},
                         {"unit_cols", t.unitCols}});
    }
    nlohmann::json todo = nlohmann::json::array();
    for (const auto& r : s.todo) todo.push_back({{"text", r.text}, {"kind", r.kind}});

    nlohmann::json slash = nlohmann::json::array();
    for (const auto& r : s.slashRows) {
        slash.push_back({{"name", r.name}, {"args", r.args}, {"desc", r.desc}});
    }

    nlohmann::json themeFields = nlohmann::json::array();
    for (const auto& f : s.themeEditor.fields) themeFields.push_back(f);

    nlohmann::json helpRows = nlohmann::json::array();
    for (const auto& h : s.help.rows) helpRows.push_back({{"key", h.key}, {"desc", h.desc}});

    nlohmann::json j;
    j["version"] = s.version;
    j["modal"] = s.modal;
    j["dock"] = s.dock;

    j["engine"] = {{"phase", s.phaseName},
                   {"phase_color", s.phaseColor},
                   {"busy", s.busy},
                   {"spinner", s.spinnerFrame},
                   {"model", s.model},
                   {"workspace", s.workspace},
                   {"role", s.roleName},
                   {"role_index", s.roleIndex},
                   {"prompt_files", s.promptFiles},
                   {"queued", s.queued}};

    j["chat"] = {{"rows", chat},
                 {"streaming",
                  {{"visible", s.streaming.visible},
                   {"has_reasoning", s.streaming.hasReasoning},
                   {"reasoning_head", s.streaming.reasoningHead},
                   {"reasoning_body", s.streaming.reasoningBody},
                   {"earlier_omitted", s.streaming.earlierOmitted},
                   {"has_content", s.streaming.hasContent},
                   {"content_head", s.streaming.contentHead},
                   {"content", s.streaming.content}}}};

    j["input"] = {{"text", s.input.text},
                  {"cursor", s.input.cursor},
                  {"placeholder", s.input.placeholder}};

    j["slash"] = {{"open", s.slashOpen},
                  {"selected", s.slashSelected},
                  {"first", s.slashFirst},
                  {"above", s.slashAbove},
                  {"below", s.slashBelow},
                  {"rows", slash}};

    j["agents"] = {{"empty", s.agentsEmpty}, {"rows", agents}};
    j["tasks"] = {{"empty", s.tasksEmpty}, {"rows", tasks}};
    j["todo"] = {{"title", s.todoTitle}, {"empty", s.todoEmpty}, {"rows", todo}};

    j["status_line"] = {{"text", s.statusText},
                        {"color", s.statusColor},
                        {"spinner", s.statusSpinner},
                        {"skill", s.skillStatus}};
    j["status_bar"] = segmentsJson(s.statusBar);
    j["keycaps"] = segmentsJson(s.keycaps);

    j["approval"] = approvalJson(s.approval);
    j["config"] = configJson(s.config);
    j["mcp"] = mcpJson(s.mcp);
    j["open"] = listJson(s.open);
    j["model"] = listJson(s.modelList);
    j["theme"] = listJson(s.themeList);
    j["theme_editor"] = {{"visible", s.themeEditor.visible},
                         {"title", s.themeEditor.title},
                         {"fields", themeFields},
                         {"selected", s.themeEditor.selected},
                         {"field", s.themeEditor.field},
                         {"hex", s.themeEditor.hex},
                         {"hint", s.themeEditor.hint},
                         {"buttons", rowsJson(s.themeEditor.buttons)}};
    j["help"] = {{"visible", s.help.visible},
                 {"title", s.help.title},
                 {"rows", helpRows},
                 {"hint", s.help.hint},
                 {"buttons", rowsJson(s.help.buttons)}};
    j["quit"] = {{"visible", s.quit.visible},
                 {"title", s.quit.title},
                 {"body", s.quit.body},
                 {"buttons", rowsJson(s.quit.buttons)}};
    j["welcome"] = {{"visible", s.welcome.visible},
                    {"title", s.welcome.title},
                    {"body", s.welcome.body},
                    {"key_label", s.welcome.keyLabel},
                    {"button", s.welcome.button},
                    {"hint", s.welcome.hint}};

    // 2-space indent, keys sorted, UTF-8 not escaped: a human reads a diff of
    // two snapshots and sees Chinese text as Chinese text.
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

} // namespace view_model
