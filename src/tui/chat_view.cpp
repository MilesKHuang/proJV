// proJV TUI -- chat view: render chat history as labeled, color-coded blocks.
//
// Each message is a bordered block: a colored "── Role ──" label, then the
// content (markdown for assistant replies, plain text otherwise). The block
// border uses the role color so different message kinds are visually distinct.
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
    Color user;
    Color assistant;
    Color system;
    Color tool;
    Color toolResult;
    Color reasoning;
    Color reasoningBody;
    Color compacted;
};
Palette g_palette;
const Palette& P = g_palette;

void refreshPalette() {
    const auto& T = ThemeManager::instance().current();
    g_palette.text = theme_map::hexToColor(T.text);
    g_palette.user = theme_map::hexToColor(T.mdH1);
    g_palette.assistant = theme_map::hexToColor(T.phaseStreaming);
    g_palette.system = theme_map::hexToColor(T.statusIdle);
    g_palette.tool = theme_map::hexToColor(T.toolTitleColor);
    g_palette.toolResult = theme_map::hexToColor(T.toolResultText);
    g_palette.reasoning = theme_map::hexToColor(T.reasoningTextColor);
    g_palette.reasoningBody = theme_map::hexToColor(T.reasoningBodyText);
    g_palette.compacted = theme_map::hexToColor(T.statusCtxHigh);
}

Element label(const std::string& text, Color c) {
    return ftxui::text(text) | ftxui::bold | ftxui::color(c);
}

// Wrap a message block with a colored border (role color for visual separation).
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
        Color roleColor = P.system;
        if (b.role == "user") {
            roleColor = P.user;
            block = ftxui::vbox({ label("── You ──", P.user), markdown_view::renderPlainText(b.content) | ftxui::color(P.text) });
        } else if (b.role == "assistant" && b.hasReasoning && b.content.empty()) {
            roleColor = P.reasoning;
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
        } else if (b.role == "assistant") {
            roleColor = P.assistant;
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
                els.push_back(label("── AI ──", P.assistant));
                els.push_back(markdown_view::renderMarkdown(b.content));
            }
            block = ftxui::vbox(std::move(els));
        } else if (b.role == "tool_call") {
            roleColor = P.tool;
            std::string merged = b.content;
            // Merge consecutive tool_result bubbles into a single tool block.
            while (i + 1 < total && bubbles[i + 1].role == "tool_result") {
                merged += "\n--------------------\n" + bubbles[i + 1].content;
                ++i;
            }
            std::string display = merged;
            int nl = 0;
            for (char c : merged) if (c == '\n') ++nl;
            if (nl >= 5) {
                int found = 0; size_t pos = 0;
                while (found < 5 && pos < merged.size()) {
                    pos = merged.find('\n', pos);
                    if (pos == std::string::npos) break;
                    ++pos; ++found;
                }
                display = merged.substr(0, pos) + "...";
            }
            block = ftxui::vbox({ label("── Tool ──", P.tool), markdown_view::renderPlainText(display) | ftxui::color(P.text) });
        } else if (b.role == "tool_result") {
            roleColor = P.toolResult;
            block = ftxui::vbox({ label("── Result ──", P.toolResult), markdown_view::renderPlainText(b.content) | ftxui::color(P.text) | ftxui::dim });
        } else if (b.role == "system") {
            bool compacted = b.content.find("[Context compacted:") != std::string::npos;
            roleColor = compacted ? P.compacted : P.system;
            block = ftxui::vbox({ label("── System ──", roleColor),
                markdown_view::renderPlainText(b.content) | ftxui::color(P.text) | ftxui::dim });
        } else {
            roleColor = P.system;
            block = markdown_view::renderPlainText(b.content) | ftxui::color(P.text);
        }

        lines.push_back(frameBlock(std::move(block), roleColor));
        if (i + 1 < total) {
            lines.push_back(ftxui::separatorHeavy() | ftxui::color(roleColor));
        }
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
        els.push_back(label("── AI ──", P.assistant));
        els.push_back(markdown_view::renderMarkdown(status.streamingText));
    }
    if (els.empty()) {
        return ftxui::text(" ");
    }
    Element block = ftxui::vbox(std::move(els));
    return std::move(block) | ftxui::borderStyled(P.assistant);
}

} // namespace chat_view
