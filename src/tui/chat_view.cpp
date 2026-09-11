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

// Wrap a message block with a colored border (role color for visual separation).
Element frameBlock(Element block, Color c, bool focused) {
    if (focused) block = std::move(block) | ftxui::focus;
    return std::move(block) | ftxui::borderStyled(c);
}

} // namespace

Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles, int focusIndex) {
    refreshPalette();

    int total = static_cast<int>(bubbles.size());
    Elements lines;
    for (int i = 0; i < total; ++i) {
        const auto& b = bubbles[i];

        Element block;
        Color roleColor = P.system;
        if (b.role == "user") {
            roleColor = P.user;
            block = ftxui::vbox({ label("── You ──", P.user), ftxui::text(b.content) });
        } else if (b.role == "assistant" && b.hasReasoning && b.content.empty()) {
            roleColor = P.reasoning;
            Elements els;
            els.push_back(label("── Thinking ──", P.reasoning));
            if (b.reasoningExpanded) {
                els.push_back(ftxui::text(b.reasoningText) | ftxui::color(P.reasoningBody));
            } else {
                els.push_back(ftxui::text("(collapsed, F9 to expand)") | ftxui::dim);
            }
            block = ftxui::vbox(std::move(els));
        } else if (b.role == "assistant") {
            roleColor = P.assistant;
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
            roleColor = P.tool;
            block = ftxui::vbox({ label("── Tool ──", P.tool), ftxui::text(b.content) });
        } else if (b.role == "tool_result") {
            roleColor = P.toolResult;
            block = ftxui::vbox({ label("── Result ──", P.toolResult), ftxui::text(b.content) | ftxui::dim });
        } else if (b.role == "system") {
            bool compacted = b.content.find("[Context compacted:") != std::string::npos;
            roleColor = compacted ? P.compacted : P.system;
            block = ftxui::vbox({ label("── System ──", roleColor),
                ftxui::text(b.content) | ftxui::dim });
        } else {
            roleColor = P.system;
            block = ftxui::text(b.content);
        }

        lines.push_back(frameBlock(std::move(block), roleColor, i == focusIndex));
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
        els.push_back(ftxui::text(status.reasoningText) | ftxui::color(P.reasoningBody));
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
