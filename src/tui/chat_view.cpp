// proJV TUI -- chat view implementation (1:1 port of renderChatArea rendering).
#include "chat_view.h"
#include "markdown_view.h"
#include "theme_map.h"
#include "theme_manager.h"

#include <string>

namespace chat_view {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

// Approximate legacy theme colors. Phase 9 wires these to ThemeColors.
// (ImGui hex -> 8-bit-ish RGB; kept in one place for later replacement.)
namespace {
struct Palette {
    Color bubbleUser;
    Color bubbleAssistant;
    Color bubbleSystem;
    Color bubbleDefault;
    Color bubbleCompacted;
    Color toolBg;
    Color toolTitle;
    Color toolResult;
    Color reasoningBorder;
    Color reasoningText;
    Color reasoningBody;
    Color compactedLabel;
    Color statusIdle;
};

Palette g_palette;
const Palette& P = g_palette;

void refreshPalette() {
    const auto& T = ThemeManager::instance().current();
    g_palette.bubbleUser = theme_map::hexToColor(T.bubbleUserBg);
    g_palette.bubbleAssistant = theme_map::hexToColor(T.bubbleAssistantBg);
    g_palette.bubbleSystem = theme_map::hexToColor(T.bubbleSystemBg);
    g_palette.bubbleDefault = theme_map::hexToColor(T.bubbleDefaultBg);
    g_palette.bubbleCompacted = theme_map::hexToColor(T.bubbleCompactedBg);
    g_palette.toolBg = theme_map::hexToColor(T.toolBg);
    g_palette.toolTitle = theme_map::hexToColor(T.toolTitleColor);
    g_palette.toolResult = theme_map::hexToColor(T.toolResultText);
    g_palette.reasoningBorder = theme_map::hexToColor(T.reasoningBorder);
    g_palette.reasoningText = theme_map::hexToColor(T.reasoningTextColor);
    g_palette.reasoningBody = theme_map::hexToColor(T.reasoningBodyText);
    g_palette.compactedLabel = theme_map::hexToColor(T.todoInProgress);
    g_palette.statusIdle = theme_map::hexToColor(T.statusIdle);
}

Element renderNormalBubble(const bubble_model::Bubble& b) {
    bool isCompacted = (b.role == "system" &&
        b.content.find("[Context compacted:") != std::string::npos);

    Color bg = P.bubbleDefault;
    if (b.role == "user") bg = P.bubbleUser;
    else if (b.role == "assistant") bg = P.bubbleAssistant;
    else if (b.role == "system") bg = P.bubbleSystem;
    if (isCompacted) bg = P.bubbleCompacted;

    Elements els;
    if (isCompacted) {
        els.push_back(ftxui::text("[Context compacted]") | ftxui::color(P.compactedLabel));
        els.push_back(ftxui::separator());
    }
    if (b.role == "assistant") {
        els.push_back(markdown_view::renderMarkdown(b.content) | ftxui::bgcolor(bg));
    } else {
        els.push_back(ftxui::text(b.content) | ftxui::bgcolor(bg));
    }

    Element bubble = ftxui::vbox(std::move(els)) | ftxui::bgcolor(bg);

    if (b.role == "user") {
        return ftxui::hbox({ ftxui::filler(), bubble | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 64) });
    }
    return bubble | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 64);
}

Element renderReasoningCard(const bubble_model::Bubble& b) {
    std::string arrow = b.reasoningExpanded ? "[-]" : "[+]";
    std::string label = arrow + " Reasoning (" + std::to_string(b.reasoningText.size()) + " chars)";

    Elements els;
    els.push_back(ftxui::text(label) | ftxui::color(P.reasoningText));
    if (b.reasoningExpanded) {
        els.push_back(ftxui::separator());
        els.push_back(ftxui::text(b.reasoningText) | ftxui::color(P.reasoningBody)
            | ftxui::frame | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 5));
    }
    return ftxui::vbox(std::move(els)) | ftxui::border | ftxui::color(P.reasoningBorder);
}

Element renderToolBubble(const std::string& toolTitle, const std::string& mergedContent) {
    Elements els;
    els.push_back(ftxui::text(toolTitle) | ftxui::color(P.toolTitle));
    els.push_back(ftxui::separator());

    if (!mergedContent.empty()) {
        std::string displayContent = mergedContent;
        int nlCount = 0;
        for (char c : mergedContent) if (c == '\n') ++nlCount;
        if (nlCount >= 5) {
            int found = 0;
            size_t pos = 0;
            while (found < 5 && pos < mergedContent.size()) {
                pos = mergedContent.find('\n', pos);
                if (pos == std::string::npos) break;
                ++pos;
                ++found;
            }
            displayContent = mergedContent.substr(0, pos) + "\n...";
        }
        els.push_back(ftxui::text(displayContent) | ftxui::color(P.toolResult));
    }

    return ftxui::vbox(std::move(els)) | ftxui::bgcolor(P.toolBg);
}

} // namespace

Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles, int scroll) {
    refreshPalette();
    constexpr int kMaxVisibleBubbles = 100;
    int totalBubbles = static_cast<int>(bubbles.size());

    // Keyboard scroll-up: hide the last `scroll` bubbles.
    int visibleEnd = totalBubbles - scroll;
    if (visibleEnd < 0) visibleEnd = 0;

    int startIdx = 0;
    if (visibleEnd > kMaxVisibleBubbles) {
        startIdx = visibleEnd - kMaxVisibleBubbles;
    }

    Elements els;
    if (startIdx > 0) {
        std::string hint = "[ Showing last " + std::to_string(kMaxVisibleBubbles) +
            " of " + std::to_string(visibleEnd) + " messages - older history hidden ]";
        els.push_back(ftxui::text(hint) | ftxui::color(P.statusIdle));
        els.push_back(ftxui::separator());
    }
    if (scroll > 0) {
        els.push_back(ftxui::text("[ Scrolled up " + std::to_string(scroll) + " bubble(s) - Down to return ]") | ftxui::color(P.statusIdle));
        els.push_back(ftxui::separator());
    }

    for (int i = startIdx; i < visibleEnd; ++i) {
        const auto& bubble = bubbles[i];

        // Merged tool bubble: tool_call title + following tool_result content.
        if (bubble.role == "tool_call") {
            std::string mergedContent;
            while (i + 1 < visibleEnd && bubbles[i + 1].role == "tool_result") {
                if (!mergedContent.empty()) mergedContent += "\n--------------------\n";
                mergedContent += bubbles[i + 1].content;
                ++i;
            }

            std::string aiText;
            std::string toolTitle = bubble.content;
            auto toolMarker = toolTitle.find("\n[tool:");
            if (toolMarker != std::string::npos) {
                aiText = toolTitle.substr(0, toolMarker);
                toolTitle = toolTitle.substr(toolMarker + 1);
            }

            if (!aiText.empty()) {
                bubble_model::Bubble textBubble;
                textBubble.role = "assistant";
                textBubble.content = aiText;
                els.push_back(renderNormalBubble(textBubble));
            }
            els.push_back(renderToolBubble(toolTitle, mergedContent));
            continue;
        }

        // Reasoning-only bubble (chain-of-thought, no body text).
        if (bubble.role == "assistant" && bubble.hasReasoning && bubble.content.empty()) {
            els.push_back(renderReasoningCard(bubble));
            continue;
        }

        // Normal bubble (user, assistant, system, standalone tool_result).
        els.push_back(renderNormalBubble(bubble));
    }

    // Focus anchor at the bottom so the FTXUI frame auto-scrolls to the latest
    // message (Tab focus stays on the input component; this is an Element focus).
    els.push_back(ftxui::text("") | ftxui::focus);

    return ftxui::vbox(std::move(els));
}

Element renderStreamingBubble(const AgentStatus& status) {
    refreshPalette();
    Elements els;

    // Live reasoning card (expanded while streaming).
    if (!status.reasoningText.empty()) {
        bubble_model::Bubble think;
        think.role = "assistant";
        think.hasReasoning = true;
        think.reasoningText = status.reasoningText;
        think.reasoningExpanded = true;
        els.push_back(renderReasoningCard(think));
    }

    // Live content body.
    if (!status.streamingText.empty()) {
        bubble_model::Bubble textBubble;
        textBubble.role = "assistant";
        textBubble.content = status.streamingText;
        els.push_back(renderNormalBubble(textBubble));
    }

    return ftxui::vbox(std::move(els));
}

} // namespace chat_view
