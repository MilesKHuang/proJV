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

Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles, int focusIndex) {
    refreshPalette();

    int total = static_cast<int>(bubbles.size());
    Elements lines;
    for (int i = 0; i < total; ++i) {
        const auto& b = bubbles[i];

        Element block;
        if (b.role == "user") {
            block = ftxui::vbox({ label("── You ──", P.user), ftxui::text(b.content) });
        } else if (b.role == "assistant" && b.hasReasoning && b.content.empty()) {
            Elements els;
            els.push_back(label("── Thinking ──", P.reasoning));
            if (b.reasoningExpanded) {
                els.push_back(ftxui::text(b.reasoningText) | ftxui::color(P.reasoningBody));
            } else {
                els.push_back(ftxui::text("(collapsed, F9 to expand)") | ftxui::dim);
            }
            block = ftxui::vbox(std::move(els));
        } else if (b.role == "assistant") {
            Elements els;
            if (b.hasReasoning && b.reasoningExpanded) {
                els.push_back(label("── Thinking ──", P.reasoning));
                els.push_back(ftxui::text(b.reasoningText) | ftxui::color(P.reasoningBody));
            }
            if (!b.content.empty()) {
                els.push_back(label("── AI ──", P.assistant));
                els.push_back(markdown_view::renderMarkdown(b.content));
            }
            block = ftxui::vbox(std::move(els));
        } else if (b.role == "tool_call") {
            block = ftxui::vbox({ label("── Tool ──", P.tool), ftxui::text(b.content) });
        } else if (b.role == "tool_result") {
            block = ftxui::vbox({ label("── Result ──", P.toolResult), ftxui::text(b.content) | ftxui::dim });
        } else if (b.role == "system") {
            bool compacted = b.content.find("[Context compacted:") != std::string::npos;
            block = ftxui::vbox({ label("── System ──", compacted ? P.compacted : P.system),
                ftxui::text(b.content) | ftxui::dim });
        } else {
            block = ftxui::text(b.content);
        }

        if (i == focusIndex) {
            block = block | ftxui::focus;
        }
        lines.push_back(block);
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
