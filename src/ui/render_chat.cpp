#define IMGUI_DEFINE_MATH_OPERATORS
#include "app.h"
#include "ui/theme.h"
#include "render_chat.h"
#include "core/prompts.h"
#include "core/config.h"
#include "tools/registry.h"
#include "json.hpp"
#include "debug_log.h"
#include <windows.h>
#include <shellapi.h>
#include <imgui.h>
#include "markdown_render.h"
#include <algorithm>
#include <format>
#include <sstream>
#include <ctime>
#include <exception>
#include <stdexcept>

// --- Spinner helper (animated processing indicator)  -- Item 10.1 --------------
// Draws a rotating arc at current cursor position. Advances ImGui cursor.
static void renderSpinner(float radius, float thickness, const ImVec4& color) {
    auto drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 center(pos.x + radius, pos.y + radius);

    float time = (float)ImGui::GetTime();
    float startAngle = time * 3.0f;
    float endAngle = startAngle + 3.14159265f * 1.5f;

    drawList->PathArcTo(center, radius, startAngle, endAngle, 12);
    drawList->PathStroke(ImGui::ColorConvertFloat4ToU32(color), ImDrawFlags_None, thickness);

    // Advance cursor so layout works correctly
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    ImGui::SameLine();
}

// --- Timestamp helper ------------------------------------------------------
static std::string timestamp() {
    auto t = std::time(nullptr);
    char buf[32];
    tm local;
    localtime_s(&local, &t);
    strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return buf;
}


// --- Render text with optional markdown support ---------------------------
void renderFormattedText(const std::string& text, float bubbleWidth,
                         bool enableMd) {
    if (!enableMd) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bubbleWidth - 16.0f);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    try {
        renderMarkdown(text, bubbleWidth, [](const std::string& url) {
            ShellExecuteA(nullptr, "open", url.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        });
    }
    catch (const std::exception& e) {
        debugLog(std::string("Markdown exception: ") + e.what());
        debugLog(text.substr(0, 500));
        ImGui::TextWrapped("%s", text.c_str());
    }
    catch (...) {
        debugLog("Markdown exception: (unknown)");
        debugLog(text.substr(0, 500));
        ImGui::TextWrapped("%s", text.c_str());
    }
}

static const ThemeColors& T() { return ThemeManager::instance().current(); }

void RenderCopyButton(const char* label, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Button, ThemeColors::toVec4(T().buttonBg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeColors::toVec4(T().buttonHovered));
    if (ImGui::SmallButton(label)) {
        ImGui::SetClipboardText(text);
    }
    ImGui::PopStyleColor(2);
}

// --- Helper: format a tool message for display ----------------------------
// Converts Message (role=="tool" or with toolCalls)  -> (ChatBubble role, content)
static std::pair<std::string, std::string> formatToolMsg(const Message& msg) {
    if (!msg.toolCalls.empty()) {
        // Compact per-tool display: one line per tool with key arg only
        std::string d;
        for (size_t i = 0; i < msg.toolCalls.size(); ++i) {
            if (i > 0) d += "\n";
            const auto& tc = msg.toolCalls[i];
            d += tc.name;
            // Try to extract the key argument (path/file/command/pattern)
            try {
                auto args = nlohmann::json::parse(tc.arguments);
                if (tc.name == "read_file" || tc.name == "write_file")
                    d += ": " + args.value("path", args.value("file", "?"));
                else if (tc.name == "exec_shell" || tc.name == "shell")
                    d += ": " + args.value("command", "?");
                else if (tc.name == "edit_file")
                    d += ": " + args.value("path", "?");
                else if (tc.name == "grep_files")
                    d += ": " + args.value("pattern", "?");
                else if (tc.name == "file_search")
                    d += ": " + args.value("query", "?");
            } catch (...) {
                // Fallback: raw args truncated
                std::string a = tc.arguments;
                if (a.size() > 60) a = a.substr(0, 57) + "...";
                d += ": " + a;
            }
        }
        return {"tool_call", d};
    }
    // role == "tool"  -- compact summary with optional detail toggle
    {
        std::string d;
        // Compute a brief summary line
        size_t byteCount = msg.content.size();
        if (msg.name == "read_file" || msg.name == "file_search" || msg.name == "grep_files") {
            int lineCount = 0;
            for (char c : msg.content) if (c == '\n') ++lineCount;
            d = std::format("{} ({} lines, {} bytes)", msg.name, lineCount, byteCount);
        } else if (msg.name == "exec_shell" || msg.name == "shell" || msg.name == "git_log" 
                   || msg.name == "git_status" || msg.name == "git_diff" || msg.name == "web_search") {
            d = std::format("{} ({} bytes)", msg.name, byteCount);
        } else {
            d = std::format("{} ({} bytes)", msg.name, byteCount);
        }
        // Append first line of content for "done X" feel
        std::string firstLine = msg.content.substr(0, msg.content.find('\n'));
        if (firstLine.size() > 80) firstLine = firstLine.substr(0, 77) + "...";
        if (!firstLine.empty()) d += "  -> " + firstLine;
        return {"tool_result", d};
    }
}

// --- App::deriveBubblesFromMessage ----------------------------------------
// Converts one Message into one or more ChatBubbles, appends to chatHistory.
void App::deriveBubblesFromMessage(const Message& msg) {
    if (msg.role != "system")
        debugLogf("[Bubble] derive: id=%lld role=%s content=%zu chars toolCalls=%zu",
            (long long)msg.id, msg.role.c_str(), msg.content.size(), msg.toolCalls.size());
    // Skip system messages (they are visual noise; /workspace is special-cased)
    if (msg.role == "system") {
        // 10.5: Show context compaction markers
        if (msg.content.find("[Context compacted:") != std::string::npos) {
            ChatBubble cb;
            cb.role = "system";
            cb.content = msg.content;
            chatHistory.push_back(cb);
        }
        // Only show system messages that look like workspace output
        else if (msg.content.find("[Workspace]") != std::string::npos) {
            ChatBubble cb;
            cb.role = "system";
            cb.content = msg.content;
            chatHistory.push_back(cb);
        }
        return;
    }

    // Tool messages: assistant with tool_calls  -> tool_call, role=="tool"  -> tool_result
    if (!msg.toolCalls.empty() || msg.role == "tool") {
        auto [role, display] = formatToolMsg(msg);

        // V4 thinking: prepend a collapsible thinking bubble.
        // For tool-call assistants: before the tool bubbles.
        // For text-only assistants: before the reply text.
        if (msg.role == "assistant" && !msg.reasoningContent.empty()) {
            ChatBubble thinkBubble;
            thinkBubble.role = "assistant";
            thinkBubble.hasReasoning = true;
            thinkBubble.reasoningText = msg.reasoningContent;
            thinkBubble.content.clear();  // no body text -- tools come next
            chatHistory.push_back(thinkBubble);
        }

        // If this is an assistant with tool_calls AND has text content,
        // push the text as a separate assistant bubble BEFORE the tool_call bubble
        // so the user sees both the generated response and the tool invocation.
        if (!msg.toolCalls.empty() && msg.role == "assistant" && !msg.content.empty()) {
            ChatBubble textBubble;
            textBubble.role = "assistant";
            textBubble.content = msg.content;
            chatHistory.push_back(textBubble);
        }
        // Tool call/result bubble
        {
            ChatBubble cb;
            cb.role = role;
            cb.content = display;
            chatHistory.push_back(cb);
        }
        return;
    }

    // Regular messages: user / assistant
    // V4 thinking: prepend a thinking bubble for text-only assistant replies
    if (msg.role == "assistant" && !msg.reasoningContent.empty()) {
        ChatBubble thinkBubble;
        thinkBubble.role = "assistant";
        thinkBubble.hasReasoning = true;
        thinkBubble.reasoningText = msg.reasoningContent;
        thinkBubble.content.clear();
        chatHistory.push_back(thinkBubble);
    }
    ChatBubble cb;
    cb.role = msg.role;
    cb.content = msg.content;
    chatHistory.push_back(cb);
}

// --- App::buildBubblesFromMessages ----------------------------------------
// Full rebuild of chatHistory from session messages.
// Called on session load (initialize, switchToDialog, newChat).
void App::buildBubblesFromMessages() {
    if (!agent) return;
    chatHistory.clear();
    lastMessageId_ = 0;

    // Read from DB (via getNewMessagesSince), not session.
    // This keeps the data boundary: UI -> DB, session is Agent-internal.
    auto msgs = agent->getNewMessagesSince(0);
    for (const auto& msg : msgs) {
        deriveBubblesFromMessage(msg);
        if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
    }

    debugLogf("[BubbleSync] buildBubblesFromMessages: %zu bubbles from %zu messages",
        chatHistory.size(), msgs.size());
    scrollToBottom = true;
}

// --- App::syncChatFromAgent -----------------------------------------------
// Pure DB-driven incremental sync.  Reads new messages from SQLite and
// derives ChatBubbles.  Single source of truth  -- no streaming overlay,
// no dedup.  Status bar (agent->getStatus()) provides real-time feedback.
void App::syncChatFromAgent() {
    if (!agent) return;
    auto status = agent->getStatus();
    bool wroteNew = false;

    // Sync new DB messages  -> chatHistory.
    // Single source of truth: agent->getNewMessagesSince() reads from DB.
    // No live streaming bubble -- the status bar provides real-time feedback.
    auto newMsgs = agent->getNewMessagesSince(lastMessageId_);
    if (!newMsgs.empty()) debugLogf("[Bubble] syncChat: %zu new msgs, lastMsgId=%lld",
        newMsgs.size(), (long long)lastMessageId_);
    for (const auto& msg : newMsgs) {
        deriveBubblesFromMessage(msg);
        if (msg.id > lastMessageId_) lastMessageId_ = msg.id;
        wroteNew = true;
    }

    // Error state
    if (status.state == AgentState::Error && lastStatus.state != AgentState::Error) {
        ChatBubble cb;
        cb.role = "system";
        cb.content = "[Error] " + status.errorMessage;
        chatHistory.push_back(cb);
        wroteNew = true;
    }

    // Cap visible bubbles at 200 to bound rendering cost.
    // Full history is still in DB  -- start a new chat if you need fresh context.
    constexpr size_t kMaxBubbles = 200;
    if (chatHistory.size() > kMaxBubbles) {
        size_t excess = chatHistory.size() - kMaxBubbles;
        chatHistory.erase(chatHistory.begin(), chatHistory.begin() + excess);
    }

    // Log state transitions
    if (status.state != lastStatus.state) {
    }

    if (wroteNew) scrollToBottom = true;
    lastStatus = status;
}

// --- App::renderChatArea ---------------------------------------------------
void App::renderChatArea() {
    ImGui::BeginChild("ChatArea", ImVec2(0, 0), false,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // --- Sliding window: only render the last N bubbles to bound per-frame cost.
    constexpr int kMaxVisibleBubbles = 100;
    int totalBubbles = (int)chatHistory.size();
    int skipCount = 0;
    int startIdx  = 0;
    if (totalBubbles > kMaxVisibleBubbles) {
        skipCount = totalBubbles - kMaxVisibleBubbles;
        startIdx  = skipCount;
    }

    if (skipCount > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(T().statusIdle));
        ImGui::TextWrapped(
            "[ Showing last %d of %d messages - older history hidden ]",
            kMaxVisibleBubbles, totalBubbles);
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // Throttled log: bubble count (every ~30 frames when >50 bubbles)
    static int chatFrameSkip = 0;
    if (chatHistory.size() > 50 && ++chatFrameSkip >= 30) {
        size_t toolBubbles = 0;
        for (const auto& b : chatHistory)
            if (b.role == "tool_call") ++toolBubbles;
        chatFrameSkip = 0;
    }

    int bubbleIdx = 0;
    for (int i = startIdx; i < totalBubbles; ++i) {
        auto& bubble = chatHistory[i];

        // -- Merged tool bubble: tool_call title + tool_result content --
        if (bubble.role == "tool_call") {
            std::string mergedContent;
            while (i + 1 < (int)chatHistory.size() && chatHistory[i+1].role == "tool_result") {
                if (!mergedContent.empty())
                    mergedContent += "\n--------------------\n";
                mergedContent += chatHistory[i+1].content;
                ++i;
            }

            float availWidth = ImGui::GetContentRegionAvail().x;
            const float padding = 10.0f;
            float bubbleWidth = std::max(100.0f, availWidth - padding * 2);

            std::string aiText;
            std::string toolTitle = bubble.content;
            auto toolMarker = toolTitle.find("\n[tool:");
            if (toolMarker != std::string::npos) {
                aiText = toolTitle.substr(0, toolMarker);
                toolTitle = toolTitle.substr(toolMarker + 1);
            }

            if (!aiText.empty()) {
                float aiWidth = std::max(100.0f, availWidth * 0.85f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::toVec4(T().bubbleAssistantBg));
                ImGui::PushID(bubbleIdx * 1000);

                ImGui::BeginChild("ai_text", ImVec2(aiWidth, 0),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

                renderFormattedText(aiText, aiWidth, true);

                ImGui::SameLine(std::max(ImGui::GetCursorPosX(), aiWidth - 38.0f));
                RenderCopyButton(("C##ai" + std::to_string(bubbleIdx)).c_str(), aiText.c_str());

                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar(2);
                ImGui::PopID();
                ImGui::Dummy(ImVec2(0, 4.0f));
            }

            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::toVec4(T().toolBg));
            ImGui::PushID(bubbleIdx);

            ImGui::BeginChild("bubble", ImVec2(bubbleWidth, 0),
                ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(T().toolTitleColor));
            ImGui::TextUnformatted(toolTitle.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();

            if (!mergedContent.empty()) {
                std::string displayContent = mergedContent;
                int nlCount = 0;
                for (char c : mergedContent)
                    if (c == '\n') ++nlCount;
                if (nlCount >= 5) {
                    int found = 0; size_t pos = 0;
                    while (found < 5 && pos < mergedContent.size()) {
                        pos = mergedContent.find('\n', pos);
                        if (pos == std::string::npos) break;
                        ++pos; ++found;
                    }
                    displayContent = mergedContent.substr(0, pos) + "\n...";
                }

                ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(T().toolResultText));
                renderFormattedText(displayContent, bubbleWidth, false);
                ImGui::PopStyleColor();
            }

            float textEndY = ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(
                std::max(padding, bubbleWidth - 32.0f), textEndY));
            RenderCopyButton(("C##" + std::to_string(bubbleIdx)).c_str(),
                (toolTitle + "\n" + mergedContent).c_str());

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            ImGui::PopID();
        }

        // -- Reasoning-only bubble (chain-of-thought, no body text) --
        else if (bubble.role == "assistant" && bubble.hasReasoning && bubble.content.empty()) {
            ImGui::Dummy(ImVec2(0, 6.0f));  // extra spacing above thinking card
            float availWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);  // left indent, not centered

            // Thin purple border card
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg,  ThemeColors::toVec4(T().reasoningCardBg));
            ImGui::PushStyleColor(ImGuiCol_Border,    ThemeColors::toVec4(T().reasoningBorder));
            ImGui::PushID(bubbleIdx);
            ImGui::BeginChild("think_card", ImVec2(availWidth - 24.0f, 0),
                ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

            std::string arrow = bubble.reasoningExpanded ? "[-]" : "[+]";
            std::string label = std::format("{} Reasoning ({} chars)",
                arrow, bubble.reasoningText.size());
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(T().reasoningTextColor));
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(0, 0)))
                bubble.reasoningExpanded = !bubble.reasoningExpanded;
            ImGui::PopStyleColor();

            if (bubble.reasoningExpanded) {
                ImGui::Dummy(ImVec2(0, 2.0f));
                float maxThinkH = ImGui::GetTextLineHeightWithSpacing() * 6.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::toVec4(T().reasoningBodyBg));
                ImGui::PushStyleColor(ImGuiCol_Text,    ThemeColors::toVec4(T().reasoningBodyText));
                ImGui::BeginChild("think_body", ImVec2(0, maxThinkH),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(bubble.reasoningText.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
            }

            ImGui::EndChild();
            ImGui::PopID();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
            ImGui::Dummy(ImVec2(0, 4.0f));
        }

        // -- Normal bubble (user, assistant, system, standalone tool_result) --
        else {
            ImVec4 bgColor;
            if (bubble.role == "user")         bgColor = ThemeColors::toVec4(T().bubbleUserBg);
            else if (bubble.role == "assistant") bgColor = ThemeColors::toVec4(T().bubbleAssistantBg);
            else if (bubble.role == "system")    bgColor = ThemeColors::toVec4(T().bubbleSystemBg);
            else                                 bgColor = ThemeColors::toVec4(T().bubbleDefaultBg);

            float availWidth = ImGui::GetContentRegionAvail().x;
            const float padding = 10.0f;
            float bubbleWidth = std::max(100.0f, availWidth - padding * 2);

            if (bubble.role == "user") {
                bubbleWidth = std::max(100.0f, availWidth * 0.85f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availWidth - bubbleWidth));
            }

            // 10.5: Compacted context marker  -- special yellow/warning styling
            bool isCompacted = (bubble.role == "system" &&
                bubble.content.find("[Context compacted:") != std::string::npos);
            if (isCompacted)
                bgColor = ThemeColors::toVec4(T().bubbleCompactedBg);

            std::string disp = bubble.content;

            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bgColor);
            ImGui::PushID(bubbleIdx);

            ImGui::BeginChild("bubble", ImVec2(bubbleWidth, 0),
                ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

            // Compacted context: prepend a warning icon label
            if (isCompacted) {
                ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::toVec4(T().todoInProgress));
                ImGui::TextUnformatted("[Context compacted]");
                ImGui::PopStyleColor();
                ImGui::Separator();
            }

            if (isCompacted) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(disp.c_str());
                ImGui::PopTextWrapPos();
            } else {
                bool isAssistant = (bubble.role == "assistant");
                renderFormattedText(disp, bubbleWidth, isAssistant);
            }

            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), bubbleWidth - 38.0f));
            RenderCopyButton(("C##" + std::to_string(bubbleIdx)).c_str(), disp.c_str());

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            ImGui::PopID();
        }

        ImGui::Dummy(ImVec2(0, 6.0f));
        ++bubbleIdx;
    }

    // -- Agent state machine indicator (non-persistent, not in chatHistory)
    // Shows the current AgentPhase and round count -- no char count redundancy.
    if (agent) {
        auto st = agent->getStatus();
        auto phase = agent->getPhase();
        const char* phaseName = "?";
        ImVec4 phaseColor = ThemeColors::toVec4(T().phaseIdle);
        switch (phase) {
            case AgentPhase::Idle:           phaseName = "Idle";           phaseColor = ThemeColors::toVec4(T().phaseIdle); break;
            case AgentPhase::Streaming:      phaseName = "Streaming";      phaseColor = ThemeColors::toVec4(T().phaseStreaming); break;
            case AgentPhase::ExecutingTools: phaseName = "ExecutingTools"; phaseColor = ThemeColors::toVec4(T().phaseExecutingTools); break;
            case AgentPhase::AwaitApproval:  phaseName = "AwaitApproval";  phaseColor = ThemeColors::toVec4(T().phaseAwaitApproval); break;
            case AgentPhase::Error:          phaseName = "Error";          phaseColor = ThemeColors::toVec4(T().phaseError); break;
        }
        ImGui::TextColored(phaseColor, " [Agent: %s]", phaseName);
        if (st.state == AgentState::ExecutingTool) {
            ImGui::SameLine(0, 4);
            ImGui::TextColored(phaseColor, "| %s (%d/%d)",
                st.currentToolName.c_str(), st.toolProgressCurrent, st.toolProgressTotal);
        } else if (phase == AgentPhase::Streaming && !st.streamingText.empty()) {
            ImGui::SameLine(0, 4);
            ImGui::TextColored(ThemeColors::toVec4(T().statusCtxPercent), "| receiving SSE data...");
        } else if (phase == AgentPhase::AwaitApproval) {
            ImGui::SameLine(0, 4);
            ImGui::TextColored(ThemeColors::toVec4(T().statusAwaiting), "| %s", st.statusMessage.c_str());
        }
    }

    if (scrollToBottom || ImGui::GetScrollY() + 30.0f >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    scrollToBottom = false;
    ImGui::EndChild();
}

// --- App::renderInputArea --------------------------------------------------
void App::renderInputArea() {
    auto opStatus = agent ? agent->getStatus() : AgentStatus{};
    bool isIdle = (opStatus.state == AgentState::Idle || opStatus.state == AgentState::Error);
    bool isWaiting = !isIdle;

    // Log input-area availability transitions
    static bool lastWasWaiting = false;
    if (isWaiting != lastWasWaiting) {
        lastWasWaiting = isWaiting;
    }

    // --- Combo + status on same line ---
    {
        float comboWidth = 110.0f;
        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("##prompt", promptFiles_.empty() ? "..." : promptFiles_[activePromptIndex_].c_str())) {
            promptFiles_ = ensureDefaultPrompts();
            // Hide compactor.md from UI (still used by /compress internally)
            promptFiles_.erase(
                std::remove(promptFiles_.begin(), promptFiles_.end(), "compactor.md"),
                promptFiles_.end());
            if (activePromptIndex_ >= (int)promptFiles_.size())
                activePromptIndex_ = 0;

            for (int i = 0; i < (int)promptFiles_.size(); ++i) {
                bool isSel = (activePromptIndex_ == i);
                if (ImGui::Selectable(promptFiles_[i].c_str(), isSel)) {
                    if (i != activePromptIndex_ && isIdle) {
                        activePromptIndex_ = i;
                        if (agent) {
                            std::string content = loadPromptFile(promptFiles_[activePromptIndex_]);
                            agent->replaceSystemPrompt(content);
                        }
                    }
                }
                if (isSel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }

    // --- Status text ---
    if (isWaiting) {
        switch (opStatus.state) {
            case AgentState::Thinking: {
                bool hasReasoning = !opStatus.reasoningText.empty();
                bool hasText = !opStatus.streamingText.empty();
                renderSpinner(8.0f, 3.0f, ThemeColors::toVec4(T().phaseStreaming));
                if (hasReasoning && !hasText) {
                    ImGui::TextColored(ThemeColors::toVec4(T().reasoningTextColor),
                        "Reasoning... (%zu chars)", opStatus.reasoningText.size());
                } else {
                    ImGui::TextColored(ThemeColors::toVec4(T().phaseStreaming),
                        "Generating... (%zu chars)", opStatus.streamingText.size());
                    if (hasReasoning) {
                        ImGui::SameLine(0, 4);
                        ImGui::TextColored(ThemeColors::toVec4(T().reasoningTextColor),
                            "| reasoned %zu chars", opStatus.reasoningText.size());
                    }
                }
                break;
            }
            case AgentState::ExecutingTool: {
                renderSpinner(8.0f, 3.0f, ThemeColors::toVec4(T().statusRunning));
                ImGui::TextColored(ThemeColors::toVec4(T().statusRunning),
                    "Running: %s (%d/%d)",
                    opStatus.currentToolName.c_str(),
                    opStatus.toolProgressCurrent,
                    opStatus.toolProgressTotal);
                break;
            }
            case AgentState::AwaitingApproval:
                ImGui::TextColored(ThemeColors::toVec4(T().statusAwaiting),
                    " Awaiting approval...");
                break;
            default:
                ImGui::TextColored(ThemeColors::toVec4(T().statusIdle),
                    " %s", opStatus.statusMessage.c_str());
        }
    } else {
        ImGui::TextColored(ThemeColors::toVec4(T().statusIdle), " Idle");
    }

    // --- Separator between status line and input ---
    ImGui::Separator();

    // --- Input box auto-fills via child with -reserve for status bar ---
    float sbReserve = ImGui::GetFrameHeightWithSpacing() * 1.2f;
    ImGui::BeginChild("InputFill", ImVec2(0, -sbReserve), false);

    ImVec2 avail = ImGui::GetContentRegionAvail();

    float buttonWidth = 80.0f;
    float btnHeight = avail.y;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float inputAreaWidth = -1.0f;
    if (avail.x > buttonWidth + spacing * 2) {
        inputAreaWidth = avail.x - buttonWidth - spacing;
    }

    bool disableInput = isWaiting;
    if (disableInput) ImGui::BeginDisabled();

    // Log the first frame where input transitions from disabled->enabled
    static bool wasDisabled = false;
    if (!disableInput && wasDisabled) {
    }

    ImGui::PushItemWidth(inputAreaWidth);
    ImGui::InputTextMultiline("##input", inputBuf, sizeof(inputBuf),
        ImVec2(inputAreaWidth, btnHeight), 0);
    ImGui::PopItemWidth();

    if (disableInput) ImGui::EndDisabled();
    wasDisabled = disableInput;

    if (isIdle && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Enter)) {
        std::string text = inputBuf;
        text.erase(0, text.find_first_not_of(" \t\n\r"));
        text.erase(text.find_last_not_of(" \t\n\r") + 1);
        if (!text.empty() && agent) {
            if (text == "/workspace") {
                std::string ws = config.workspacePath.empty()
                    ? std::filesystem::absolute(
                          std::filesystem::path(getExeDir())
                      ).string()
                    : config.workspacePath;
                ChatBubble cb;
                cb.role = "system";
                cb.content = std::format("[Workspace] {}", ws);
                chatHistory.push_back(cb);
                scrollToBottom = true;
                inputBuf[0] = '\0';
            } else if (agent->handleQuickCommand(text)) {
                inputBuf[0] = '\0';
            } else {
                inputBuf[0] = '\0';
                agent->startTurn(text);
                launchAgentThread();
            }
        }
    }

    ImGui::SameLine();
    if (isWaiting) {
        ImGui::EndDisabled();  // pop outer app-level disable -> Cancel stays active
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, btnHeight))) {
            if (agent) agent->cancel();
        }
        ImGui::BeginDisabled();  // re-push app-level disable
    } else {
        bool sendDisabled = (inputBuf[0] == '\0');
        if (sendDisabled) ImGui::BeginDisabled();
        if (ImGui::Button("Send", ImVec2(buttonWidth, btnHeight))) {
            std::string text = inputBuf;
            text.erase(0, text.find_first_not_of(" \t\n\r"));
            text.erase(text.find_last_not_of(" \t\n\r") + 1);
            if (!text.empty() && agent) {
                if (text == "/workspace") {
                    std::string ws = config.workspacePath.empty()
                        ? std::filesystem::absolute(
                              std::filesystem::path(getExeDir())
                          ).string()
                        : config.workspacePath;
                    ChatBubble cb;
                    cb.role = "system";
                    cb.content = std::format("[Workspace] {}", ws);
                    chatHistory.push_back(cb);
                    scrollToBottom = true;
                    inputBuf[0] = '\0';
                } else if (agent->handleQuickCommand(text)) {
                    inputBuf[0] = '\0';
                } else {
                    inputBuf[0] = '\0';
                    agent->startTurn(text);
                    launchAgentThread();
                }
            }
        }
        if (sendDisabled) ImGui::EndDisabled();
    }

    ImGui::EndChild();  // InputFill
}