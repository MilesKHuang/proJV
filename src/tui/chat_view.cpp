// proJV TUI -- chat view implementation (1:1 port of renderChatArea rendering).
#include "chat_view.h"
#include "markdown_view.h"

#include <string>

namespace chat_view {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

// Approximate legacy theme colors. Phase 9 wires these to ThemeColors.
// (ImGui hex -> 8-bit-ish RGB; kept in one place for later replacement.)
namespace {
struct Palette {
    Color bubbleUser = Color::RGB(30, 40, 64);
    Color bubbleAssistant = Color::RGB(30, 40, 30);
    Color bubbleSystem = Color::RGB(24, 24, 32);
    Color bubbleDefault = Color::RGB(20, 20, 28);
    Color bubbleCompacted = Color::RGB(42, 36, 24);
    Color toolBg = Color::RGB(28, 28, 42);
    Color toolTitle = Color::RGB(224, 160, 64);
    Color toolResult = Color::RGB(160, 160, 184);
    Color reasoningBorder = Color::RGB(72, 72, 128);
    Color reasoningText = Color::RGB(160, 160, 224);
    Color reasoningBody = Color::RGB(144, 144, 208);
    Color compactedLabel = Color::RGB(224, 136, 48);
    Color statusIdle = Color::RGB(104, 104, 136);
};
const Palette P;

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
        els.push_back(ftxui::text(b.reasoningText) | ftxui::color(P.reasoningBody));
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

Element renderBubbles(const std::vector<bubble_model::Bubble>& bubbles) {
    constexpr int kMaxVisibleBubbles = 100;
    int totalBubbles = static_cast<int>(bubbles.size());
    int startIdx = 0;
    if (totalBubbles > kMaxVisibleBubbles) {
        startIdx = totalBubbles - kMaxVisibleBubbles;
    }

    Elements els;
    if (startIdx > 0) {
        std::string hint = "[ Showing last " + std::to_string(kMaxVisibleBubbles) +
            " of " + std::to_string(totalBubbles) + " messages - older history hidden ]";
        els.push_back(ftxui::text(hint) | ftxui::color(P.statusIdle));
        els.push_back(ftxui::separator());
    }

    for (int i = startIdx; i < totalBubbles; ++i) {
        const auto& bubble = bubbles[i];

        // Merged tool bubble: tool_call title + following tool_result content.
        if (bubble.role == "tool_call") {
            std::string mergedContent;
            while (i + 1 < totalBubbles && bubbles[i + 1].role == "tool_result") {
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

    return ftxui::vbox(std::move(els));
}

Element renderStreamingBubble(const AgentStatus& status) {
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
