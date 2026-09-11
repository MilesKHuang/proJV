// proJV TUI -- chat view: render chat history as labeled, color-coded blocks.
//
// Each message is a block: a colored "── Role ──" label, then the content
// (markdown for assistant replies, plain text otherwise), then a blank line.
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

} // namespace

Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles, int scroll) {
    refreshPalette();

    int total = static_cast<int>(bubbles.size());
    int visibleEnd = total - scroll;
    if (visibleEnd < 0) visibleEnd = 0;

    Elements lines;
    for (int i = 0; i < visibleEnd; ++i) {
        const auto& b = bubbles[i];

        if (b.role == "user") {
            lines.push_back(label("── You ──", P.user));
            lines.push_back(ftxui::text(b.content));
        } else if (b.role == "assistant" && b.hasReasoning && b.content.empty()) {
            lines.push_back(label("── Thinking ──", P.reasoning));
            if (b.reasoningExpanded) {
                lines.push_back(ftxui::text(b.reasoningText) | ftxui::color(P.reasoningBody));
            } else {
                lines.push_back(ftxui::text("(collapsed, F9 to expand)") | ftxui::dim);
            }
        } else if (b.role == "assistant") {
            if (b.hasReasoning && b.reasoningExpanded) {
                lines.push_back(label("── Thinking ──", P.reasoning));
                lines.push_back(ftxui::text(b.reasoningText) | ftxui::color(P.reasoningBody));
            }
            if (!b.content.empty()) {
                lines.push_back(label("── AI ──", P.assistant));
                lines.push_back(markdown_view::renderMarkdown(b.content));
            }
        } else if (b.role == "tool_call") {
            lines.push_back(label("── Tool ──", P.tool));
            lines.push_back(ftxui::text(b.content));
        } else if (b.role == "tool_result") {
            lines.push_back(label("── Result ──", P.toolResult));
            lines.push_back(ftxui::text(b.content) | ftxui::dim);
        } else if (b.role == "system") {
            bool compacted = b.content.find("[Context compacted:") != std::string::npos;
            lines.push_back(label("── System ──", compacted ? P.compacted : P.system));
            lines.push_back(ftxui::text(b.content) | ftxui::dim);
        }

        lines.push_back(ftxui::text(""));  // blank line between messages
    }

    return ftxui::vbox(std::move(lines));
}

Element renderStreamingBubble(const AgentStatus& status) {
    refreshPalette();
    Elements lines;
    if (!status.reasoningText.empty()) {
        lines.push_back(label("── Thinking ──", P.reasoning));
        lines.push_back(ftxui::text(status.reasoningText) | ftxui::color(P.reasoningBody));
    }
    if (!status.streamingText.empty()) {
        lines.push_back(label("── AI ──", P.assistant));
        lines.push_back(markdown_view::renderMarkdown(status.streamingText));
    }
    return ftxui::vbox(std::move(lines));
}

} // namespace chat_view
