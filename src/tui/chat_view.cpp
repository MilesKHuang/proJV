// proJV TUI -- chat view: render chat history as labeled, color-coded blocks.
//
// Each message is a bordered block: a "-- Role --" label, then the content
// (markdown for assistant replies, plain text otherwise). Borders stay neutral
// and the text color follows the theme's semantic grading, matching the
// legacy GUI text-color mapping (see theme_popup.cpp in the pre-TUI code).
#include "chat_view.h"

#include "markdown_view.h"
#include "theme_map.h"
#include "theme_manager.h"

#include <string>

namespace chat_view {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {
struct Palette {
    Color text;
    Color border;
    Color tool;
    Color toolResult;
    Color reasoning;
    Color reasoningBody;
    Color warning;
};
Palette g_palette;
const Palette& P = g_palette;

void refreshPalette() {
    const auto& T = ThemeManager::instance().current();
    g_palette.text = theme_map::hexToColor(T.text);
    g_palette.border = theme_map::hexToColor(T.border);
    g_palette.tool = theme_map::hexToColor(T.toolTitleColor);
    g_palette.toolResult = theme_map::hexToColor(T.toolResultText);
    g_palette.reasoning = theme_map::hexToColor(T.reasoningTextColor);
    g_palette.reasoningBody = theme_map::hexToColor(T.reasoningBodyText);
    g_palette.warning = theme_map::hexToColor(T.todoInProgress);
}

Element label(const std::string& text, Color c) {
    return ftxui::text(text) | ftxui::bold | ftxui::color(c);
}

// Wrap a message block with a neutral border so role identity comes from the
// label/text color, not from a rainbow of borders.
// Per-role accent for /bigbang speakers (fixed, theme-independent).
Color speakerColor(const std::string& s) {
    if (s == "Sheldon") return Color::RGB(96, 156, 255);   // blue
    if (s == "Penny")   return Color::RGB(255, 138, 176);  // pink
    if (s == "Leonard") return Color::RGB(120, 210, 140);  // green
    return P.text;
}

Element frameBlock(Element block, Color c) {
    return std::move(block) | ftxui::borderStyled(c);
}

// Long chains of thought are shown as a trailing window instead of in full:
// laying out 30k-50k chars would flood the chat and slow every streaming
// redraw. These helpers keep the display compact without touching the stored
// reasoning text.
constexpr int kReasoningTailLines = 18;

int countLines(const std::string& text) {
    int n = 1;
    for (char c : text) if (c == '\n') ++n;
    return n;
}

std::string tailLines(const std::string& text, int maxLines) {
    int total = countLines(text);
    if (total <= maxLines) return text;
    int skip = total - maxLines;
    size_t pos = 0;
    for (int i = 0; i < skip; ++i) {
        pos = text.find('\n', pos);
        if (pos == std::string::npos) break;
        ++pos;
    }
    return text.substr(pos);
}

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

std::string reasoningSummary(const std::string& text) {
    std::string s = "Thinking (" + std::to_string(text.size()) + " chars, "
        + std::to_string(countLines(text)) + " lines)";
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r' ||
                       text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    size_t start = (end == 0) ? 0 : text.rfind('\n', end - 1);
    start = (start == std::string::npos) ? 0 : start + 1;
    std::string lastLine = text.substr(start, end - start);
    if (lastLine.size() > 60) lastLine = lastLine.substr(0, 57) + "...";
    if (!lastLine.empty()) s += " · " + lastLine;
    s += " · F8 expand";
    return s;
}

} // namespace

Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles,
                     bool reasoningExpanded) {
    refreshPalette();

    int total = static_cast<int>(bubbles.size());
    Elements lines;
    for (int i = 0; i < total; ++i) {
        const auto& b = bubbles[i];

        Element block;
        if (b.role == "user") {
            block = ftxui::vbox({
                label("── You ──", P.text),
                markdown_view::renderPlainText(b.content) | ftxui::color(P.text),
            });
        } else if (b.role == "assistant" && b.hasReasoning && b.content.empty()) {
            Elements els;
            els.push_back(label("── Thinking ──", P.reasoning));
            if (reasoningExpanded) {
                if (countLines(b.reasoningText) > kReasoningTailLines) {
                    els.push_back(ftxui::text("… (earlier thinking omitted)") | ftxui::dim);
                }
                els.push_back(markdown_view::renderPlainText(collapseBlankLines(tailLines(b.reasoningText, kReasoningTailLines)))
                              | ftxui::color(P.reasoningBody));
            } else {
                els.push_back(ftxui::text(reasoningSummary(b.reasoningText)) | ftxui::dim);
            }
            block = ftxui::vbox(std::move(els));
        } else if (b.role == "assistant" && !b.speaker.empty()) {
            Color rc = speakerColor(b.speaker);
            Elements els;
            els.push_back(label("── " + b.speaker + (b.isVote ? " · vote" : "") + " ──", rc));
            if (b.isVote) {
                bool dissent = b.content.rfind("DISSENT", 0) == 0;
                Color vc = dissent ? Color::RGB(255, 96, 96) : Color::RGB(96, 220, 128);
                els.push_back(ftxui::text(b.content) | ftxui::color(vc) | ftxui::bold);
            } else {
                els.push_back(markdown_view::renderMarkdown(b.content));
            }
            block = ftxui::vbox(std::move(els));
        } else if (b.role == "assistant") {
            Elements els;
            if (b.hasReasoning) {
                if (reasoningExpanded) {
                    els.push_back(label("── Thinking ──", P.reasoning));
                    if (countLines(b.reasoningText) > kReasoningTailLines) {
                        els.push_back(ftxui::text("… (earlier thinking omitted)") | ftxui::dim);
                    }
                    els.push_back(markdown_view::renderPlainText(collapseBlankLines(tailLines(b.reasoningText, kReasoningTailLines)))
                                  | ftxui::color(P.reasoningBody));
                } else {
                    els.push_back(ftxui::text(reasoningSummary(b.reasoningText)) | ftxui::dim);
                }
            }
            if (!b.content.empty()) {
                els.push_back(label("── proJV ──", P.text));
                els.push_back(markdown_view::renderMarkdown(b.content));
            }
            block = ftxui::vbox(std::move(els));
        } else if (b.role == "tool_call") {
            std::string title = b.content;
            std::string results;
            // Merge consecutive tool_result bubbles into the same tool block,
            // but keep the invocation title and result text separately colored.
            while (i + 1 < total && bubbles[i + 1].role == "tool_result") {
                if (!results.empty()) results += "\n--------------------\n";
                results += bubbles[i + 1].content;
                ++i;
            }
            std::string displayResults = results;
            int nl = 0;
            for (char c : results) if (c == '\n') ++nl;
            if (nl >= 5) {
                int found = 0; size_t pos = 0;
                while (found < 5 && pos < results.size()) {
                    pos = results.find('\n', pos);
                    if (pos == std::string::npos) break;
                    ++pos; ++found;
                }
                displayResults = results.substr(0, pos) + "...";
            }
            Elements toolEls;
            toolEls.push_back(label("── Tool ──", P.tool));
            toolEls.push_back(markdown_view::renderPlainText(title) | ftxui::color(P.tool));
            if (!results.empty()) {
                toolEls.push_back(markdown_view::renderPlainText(displayResults) | ftxui::color(P.toolResult));
            }
            block = ftxui::vbox(std::move(toolEls));
        } else if (b.role == "tool_result") {
            block = ftxui::vbox({
                label("── Result ──", P.toolResult),
                markdown_view::renderPlainText(b.content) | ftxui::color(P.toolResult),
            });
        } else if (b.role == "system") {
            bool compacted = b.content.find("[Context compacted:") != std::string::npos;
            Color labelColor = compacted ? P.warning : P.text;
            block = ftxui::vbox({
                label("── System ──", labelColor),
                markdown_view::renderPlainText(b.content) | ftxui::color(P.text),
            });
        } else {
            block = markdown_view::renderPlainText(b.content) | ftxui::color(P.text);
        }

        Color borderColor = b.speaker.empty() ? P.border : speakerColor(b.speaker);
        lines.push_back(frameBlock(std::move(block), borderColor));
    }

    return ftxui::vbox(std::move(lines));
}

Element renderStreamingBubble(const AgentStatus& status) {
    refreshPalette();
    Elements els;
    if (!status.reasoningText.empty()) {
        els.push_back(label("── Thinking ──", P.reasoning));
        if (countLines(status.reasoningText) > kReasoningTailLines) {
            els.push_back(ftxui::text("… (earlier thinking omitted)") | ftxui::dim);
        }
        els.push_back(markdown_view::renderPlainText(collapseBlankLines(tailLines(status.reasoningText, kReasoningTailLines)))
                      | ftxui::color(P.reasoningBody));
    }
    if (!status.streamingText.empty()) {
        els.push_back(label("── proJV ──", P.text));
        els.push_back(markdown_view::renderMarkdown(status.streamingText));
    }
    if (els.empty()) {
        return ftxui::text(" ");
    }
    Element block = ftxui::vbox(std::move(els));
    return std::move(block) | ftxui::borderStyled(P.border);
}

} // namespace chat_view
