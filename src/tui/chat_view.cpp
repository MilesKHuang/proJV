// proJV TUI -- chat view: render chat history as plain colored text lines.
//
// Simplification: no bubble boxes/backgrounds (they render poorly in a terminal
// and overlap). Each message is plain text with a role prefix and a role color.
#include "chat_view.h"

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
    g_palette.assistant = theme_map::hexToColor(T.text);
    g_palette.system = theme_map::hexToColor(T.statusIdle);
    g_palette.tool = theme_map::hexToColor(T.toolTitleColor);
    g_palette.toolResult = theme_map::hexToColor(T.toolResultText);
    g_palette.reasoning = theme_map::hexToColor(T.reasoningTextColor);
    g_palette.reasoningBody = theme_map::hexToColor(T.reasoningBodyText);
    g_palette.compacted = theme_map::hexToColor(T.statusCtxHigh);
}

Element line(const std::string& prefix, const std::string& text, Color c) {
    return ftxui::text(prefix + text) | ftxui::color(c);
}

} // namespace

Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles, int scroll) {
    refreshPalette();

    int total = static_cast<int>(bubbles.size());
    int visibleEnd = total - scroll;  // hide the last `scroll` bubbles (scroll up)
    if (visibleEnd < 0) visibleEnd = 0;

    Elements lines;
    if (scroll > 0) {
        lines.push_back(ftxui::text("[ Scrolled up " + std::to_string(scroll) + " - PgDn to return ]")
            | ftxui::color(P.system));
    }
    for (int i = 0; i < visibleEnd; ++i) {
        const auto& b = bubbles[i];
        if (b.role == "user") {
            lines.push_back(line("You: ", b.content, P.user));
        } else if (b.role == "assistant" && b.hasReasoning && b.content.empty()) {
            if (b.reasoningExpanded) {
                lines.push_back(line("Thinking: ", b.reasoningText, P.reasoning));
            } else {
                lines.push_back(line("Thinking: ", "(collapsed, F9 to expand)", P.reasoning));
            }
        } else if (b.role == "assistant") {
            if (b.hasReasoning && b.reasoningExpanded) {
                lines.push_back(line("Thinking: ", b.reasoningText, P.reasoning));
            }
            if (!b.content.empty()) {
                lines.push_back(line("AI: ", b.content, P.assistant));
            }
        } else if (b.role == "tool_call") {
            lines.push_back(line("Tool: ", b.content, P.tool));
        } else if (b.role == "tool_result") {
            lines.push_back(line("Result: ", b.content, P.toolResult));
        } else if (b.role == "system") {
            bool compacted = b.content.find("[Context compacted:") != std::string::npos;
            lines.push_back(line("System: ", b.content,
                compacted ? P.compacted : P.system));
        }
    }

    return ftxui::vbox(std::move(lines));
}

Element renderStreamingBubble(const AgentStatus& status) {
    refreshPalette();
    Elements lines;
    if (!status.reasoningText.empty()) {
        lines.push_back(line("Thinking: ", status.reasoningText, P.reasoning));
    }
    if (!status.streamingText.empty()) {
        lines.push_back(line("AI: ", status.streamingText, P.assistant));
    }
    return ftxui::vbox(std::move(lines));
}

} // namespace chat_view
