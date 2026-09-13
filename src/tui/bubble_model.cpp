// proJV TUI -- bubble model implementation (1:1 port of legacy derivation logic)
#include "bubble_model.h"

#include "json.hpp"

#include <cstdio>
#include <string>

namespace bubble_model {

std::pair<std::string, std::string> formatToolMsg(const Message& msg) {
    if (!msg.toolCalls.empty()) {
        // Compact per-tool display: one line per tool with key arg only.
        std::string d;
        for (size_t i = 0; i < msg.toolCalls.size(); ++i) {
            if (i > 0) d += "\n";
            const auto& tc = msg.toolCalls[i];
            d += tc.name;
            // Try to extract the key argument (path/file/command/pattern).
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
                // Fallback: raw args truncated.
                std::string a = tc.arguments;
                if (a.size() > 60) a = a.substr(0, 57) + "...";
                d += ": " + a;
            }
        }
        return {"tool_call", d};
    }

    // role == "tool" -- compact summary with optional detail toggle.
    {
        std::string d;
        size_t byteCount = msg.content.size();
        if (msg.name == "read_file" || msg.name == "file_search" || msg.name == "grep_files") {
            int lineCount = 0;
            for (char c : msg.content) if (c == '\n') ++lineCount;
            char buf[128];
            snprintf(buf, sizeof(buf), "%s (%d lines, %zu bytes)",
                msg.name.c_str(), lineCount, byteCount);
            d = buf;
        } else {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s (%zu bytes)", msg.name.c_str(), byteCount);
            d = buf;
        }
        // Append first line of content for "done X" feel.
        std::string firstLine = msg.content.substr(0, msg.content.find('\n'));
        if (firstLine.size() > 80) firstLine = firstLine.substr(0, 77) + "...";
        if (!firstLine.empty()) d += "  -> " + firstLine;
        return {"tool_result", d};
    }
}

std::vector<Bubble> deriveBubbles(const Message& msg) {
    std::vector<Bubble> out;

    // Skip system messages (visual noise; /workspace special-cased).
    if (msg.role == "system") {
        // Show context compaction markers.
        if (msg.content.find("[Context compacted:") != std::string::npos) {
            Bubble cb;
            cb.role = "system";
            cb.content = msg.content;
            out.push_back(cb);
        }
        // Only show system messages that look like workspace output.
        else if (msg.content.find("[Workspace]") != std::string::npos) {
            Bubble cb;
            cb.role = "system";
            cb.content = msg.content;
            out.push_back(cb);
        }
        return out;
    }

    // Tool messages: assistant with tool_calls -> tool_call, role=="tool" -> tool_result.
    if (!msg.toolCalls.empty() || msg.role == "tool") {
        auto [role, display] = formatToolMsg(msg);

        // V4 thinking: prepend a collapsible thinking bubble.
        if (msg.role == "assistant" && !msg.reasoningContent.empty()) {
            Bubble thinkBubble;
            thinkBubble.role = "assistant";
            thinkBubble.hasReasoning = true;
            thinkBubble.reasoningText = msg.reasoningContent;
            thinkBubble.content.clear();
            out.push_back(thinkBubble);
        }

        // If this is an assistant with tool_calls AND text content, push the
        // text as a separate assistant bubble BEFORE the tool_call bubble.
        if (!msg.toolCalls.empty() && msg.role == "assistant" && !msg.content.empty()) {
            Bubble textBubble;
            textBubble.role = "assistant";
            textBubble.content = msg.content;
            out.push_back(textBubble);
        }

        // Tool call/result bubble.
        Bubble cb;
        cb.role = role;
        cb.content = display;
        out.push_back(cb);
        return out;
    }

    // Regular messages: user / assistant.
    if (msg.role == "assistant" && !msg.reasoningContent.empty()) {
        Bubble thinkBubble;
        thinkBubble.role = "assistant";
        thinkBubble.hasReasoning = true;
        thinkBubble.reasoningText = msg.reasoningContent;
        thinkBubble.content.clear();
        out.push_back(thinkBubble);
    }
    Bubble cb;
    cb.role = msg.role;
    cb.content = msg.content;
    out.push_back(cb);
    return out;
}

} // namespace bubble_model
