#include "session.h"
#include "json.hpp"
#include "debug_log.h"
#include <algorithm>
#include <set>

// ============================================================================
// Truncation / compaction strings (moved from prompts.h)
// ============================================================================
static constexpr const char* CONTEXT_COMPACT_SUMMARY_PREFIX = " [Context compacted: ";
static constexpr const char* CONTEXT_COMPACT_SUMMARY_FULL_PREFIX =
    "\xe2\x9a\xa0\xef\xb8\x8f [Context compacted: ";

// -- Token estimation: ~4 ASCII chars = 1 token, ~2 CJK chars = 1 token --
size_t Session::estimateTokens(const std::string& text) {
    if (text.empty()) return 0;
    size_t t = 0;
    for (char c : text) {
        if ((unsigned char)c >= 0x80) t += 2;
        else t += 1;
    }
    return std::max<size_t>(1, t / 4);
}

// -- Prune session to fit within maxTokens context window --
void Session::pruneForContext(size_t maxTokens) {
    std::lock_guard<std::mutex> lock(mtx);

    // -- Helper: count current tokens --
    auto countTokens = [&]() -> size_t {
        size_t t = 0;
        for (const auto& msg : messages) {
            t += estimateTokens(msg.content);
            t += estimateTokens(msg.reasoningContent);
            for (const auto& tc : msg.toolCalls)
                t += estimateTokens(tc.name) + estimateTokens(tc.arguments);
        }
        return std::max<size_t>(t, 1);
    };

    size_t total = countTokens();

    if (total <= maxTokens) {
        debugLogf("[Session] Context OK: ~%zu tokens (limit %zu)", total, maxTokens);
        return;
    }

    size_t beforeMsgCount = messages.size();
    size_t beforeTokenCount = total;
    debugLogf("[Session] Context pruning: ~%zu tokens > %zu limit", total, maxTokens);

    // Track what gets pruned for the summary
    std::vector<std::string> truncatedToolNames;
    size_t truncatedUserTextCount = 0;
    size_t deletedMsgCount = 0;
    size_t deletedTokenCount = 0;

    // =============================================================
    // Step 1: Keep user & assistant intact - DO NOT TOUCH them
    // =============================================================

    // =============================================================
    // Step 2: Tool messages - keep last 10 full, older ones truncate
    // =============================================================
    {
        int totalToolMsgs = 0;
        for (const auto& msg : messages) {
            if (msg.role == "tool" || !msg.toolCalls.empty())
                ++totalToolMsgs;
        }

        int toTruncate = totalToolMsgs - 10;
        size_t cutoffIdx = 0;

        if (toTruncate > 0) {
            int seen = 0;
            for (size_t i = 0; i < messages.size() && seen < toTruncate; ++i) {
                if (messages[i].role == "tool" || !messages[i].toolCalls.empty()) {
                    ++seen;
                    if (seen == toTruncate)
                        cutoffIdx = i + 1;
                }
            }
        }

        for (size_t i = 0; i < cutoffIdx && i < messages.size(); ++i) {
            if (messages[i].role == "tool") {
                size_t before = estimateTokens(messages[i].content);
                if (!messages[i].name.empty() && truncatedToolNames.size() < 5) {
                    if (std::find(truncatedToolNames.begin(), truncatedToolNames.end(),
                                  messages[i].name) == truncatedToolNames.end())
                        truncatedToolNames.push_back(messages[i].name);
                }
                messages[i].content = "[truncated " + std::to_string(messages[i].content.size()) + " bytes]";
                size_t after = estimateTokens(messages[i].content);
                total = total > (before - after) ? total - (before - after) : 1;
            }
            if (messages[i].role == "assistant" && !messages[i].toolCalls.empty()) {
                for (auto& tc : messages[i].toolCalls) {
                    size_t before = estimateTokens(tc.arguments);
                    tc.arguments = "[truncated]";
                    size_t after = estimateTokens(tc.arguments);
                    total = total > (before - after) ? total - (before - after) : 1;
                }
            }
        }
    }

    // =============================================================
    // Step 3: If still over, truncate old user/assistant text (keep last 5)
    // =============================================================
    {
        size_t systemCount = 0;
        while (systemCount < messages.size() && messages[systemCount].role == "system")
            ++systemCount;

        int totalTextMsgs = 0;
        for (const auto& msg : messages) {
            if (msg.role != "tool" && msg.toolCalls.empty() && msg.role != "system")
                ++totalTextMsgs;
        }

        int toTruncate = totalTextMsgs - 5;
        if (toTruncate > 0) {
            size_t cutoffIdx = 0;
            int seen = 0;
            for (size_t i = systemCount; i < messages.size() && seen < toTruncate; ++i) {
                if (messages[i].role != "tool" && messages[i].toolCalls.empty() && messages[i].role != "system") {
                    ++seen;
                    if (seen == toTruncate)
                        cutoffIdx = i;
                }
            }
            for (size_t i = systemCount; i < cutoffIdx && i < messages.size(); ++i) {
                if (messages[i].role != "tool" && messages[i].toolCalls.empty() && messages[i].role != "system") {
                    size_t before = estimateTokens(messages[i].content);
                    if (before > 20) {
                        ++truncatedUserTextCount;
                        size_t byteSize = messages[i].content.size();
                        messages[i].content = "[Truncated: " + std::to_string(byteSize) + " bytes]";
                        size_t after = estimateTokens(messages[i].content);
                        total = total > (before - after) ? total - (before - after) : 1;
                    }
                }
            }
        }
    }

    // =============================================================
    // Step 4: If STILL over limit, delete oldest non-system messages
    // =============================================================
    if (total > maxTokens) {
        size_t systemCount = 0;
        while (systemCount < messages.size() && messages[systemCount].role == "system")
            ++systemCount;

        while (total > maxTokens && messages.size() > systemCount + 1) {
            size_t idx = systemCount;
            size_t removed = estimateTokens(messages[idx].content);
            int extraOrphans = 0;
            for (const auto& tc : messages[idx].toolCalls)
                removed += estimateTokens(tc.name) + estimateTokens(tc.arguments);
            // If deleting an assistant with tool_calls, also delete its orphaned
            // tool_result followers to prevent HTTP 400.
            if (!messages[idx].toolCalls.empty()) {
                while (idx + 1 < messages.size() && messages[idx + 1].role == "tool") {
                    removed += estimateTokens(messages[idx + 1].content);
                    messages.erase(messages.begin() + idx + 1);
                    ++extraOrphans;
                }
            }
            ++deletedMsgCount;
            deletedTokenCount += removed;
            messages.erase(messages.begin() + idx);
            total = total > removed ? total - removed : 1;
        }
    }

    // -- Build compact summary of what was pruned ----------------
    if (truncatedUserTextCount > 0 || deletedMsgCount > 0 || !truncatedToolNames.empty()) {
        std::string summary;
        summary += "⚠️ [Context compacted: ";
        if (deletedMsgCount > 0)
            summary += std::to_string(deletedMsgCount) + " old msgs deleted (" +
                       std::to_string(deletedTokenCount / 1000) + "K), ";
        if (truncatedUserTextCount > 0)
            summary += std::to_string(truncatedUserTextCount) + " text chunks shortened, ";
        if (!truncatedToolNames.empty()) {
            summary += "old tool results shortened: ";
            for (size_t i = 0; i < truncatedToolNames.size(); ++i) {
                if (i > 0) summary += ", ";
                summary += truncatedToolNames[i];
            }
            summary += " ";
        }
        if (summary.size() > 2 && summary.substr(summary.size() - 2) == ", ")
            summary.resize(summary.size() - 2);
        summary += "]";

        size_t sysCount = 0;
        while (sysCount < messages.size() && messages[sysCount].role == "system")
            ++sysCount;
        messages.insert(messages.begin() + sysCount, Message::System(summary));
        debugLog("[Session] Inserted context compaction summary");
    }

    debugLogf("[Session] Pruned: %zu -> %zu msgs (%zuK -> %zuK tokens)",
        beforeMsgCount, messages.size(),
        beforeTokenCount / 1000, total / 1000);
}

void Session::stripLastToolCallsAndResults() {
    std::lock_guard<std::mutex> lock(mtx);

    int lastToolCallIdx = -1;
    for (int i = (int)messages.size() - 1; i >= 0; --i) {
        if (!messages[i].toolCalls.empty()) {
            lastToolCallIdx = i;
            break;
        }
    }
    if (lastToolCallIdx < 0) return;

    int removeEnd = lastToolCallIdx;
    for (size_t i = (size_t)lastToolCallIdx + 1; i < messages.size(); ++i) {
        if (messages[i].role == "tool") {
            removeEnd = (int)i;
        } else {
            break;
        }
    }

    size_t count = (size_t)(removeEnd - lastToolCallIdx + 1);
    messages.erase(messages.begin() + lastToolCallIdx,
                   messages.begin() + removeEnd + 1);
    debugLogf("[Session] stripLastToolCallsAndResults: removed %zu msgs", count);
}

void Session::softStripLastToolCalls() {
    std::lock_guard<std::mutex> lock(mtx);

    int lastIdx = -1;
    for (int i = (int)messages.size() - 1; i >= 0; --i) {
        if (!messages[i].toolCalls.empty()) {
            lastIdx = i;
            break;
        }
    }
    if (lastIdx < 0) return;

    std::string combined = messages[lastIdx].content;
    std::vector<size_t> resultIndices;

    for (size_t j = (size_t)lastIdx + 1; j < messages.size(); ++j) {
        if (messages[j].role == "tool") {
            resultIndices.push_back(j);
            if (!combined.empty()) combined += "\n\n";
            combined += "--- " + messages[j].name + " result ---\n";
            combined += messages[j].content;
        } else {
            break;
        }
    }

    messages[lastIdx].content = combined;
    messages[lastIdx].toolCalls.clear();

    for (auto it = resultIndices.rbegin(); it != resultIndices.rend(); ++it) {
        messages.erase(messages.begin() + *it);
    }

    size_t removedCount = resultIndices.size();
    debugLogf("[Session] softStripLastToolCalls: merged %zu tool results into assistant msg (index %d)",
        removedCount, lastIdx);
}

// -- Basic accessors ----------------------------------------------------

void Session::addMessage(const Message& msg) {
    std::lock_guard<std::mutex> lock(mtx);
    messages.push_back(msg);
}

std::vector<Message> Session::getContextMessages() const {
    std::lock_guard<std::mutex> lock(mtx);
    return messages;
}

size_t Session::messageCount() const {
    std::lock_guard<std::mutex> lock(mtx);
    return messages.size();
}

void Session::loadMessages(std::vector<Message>&& msgs) {
    std::lock_guard<std::mutex> lock(mtx);
    messages = std::move(msgs);
    // Auto-repair: fix orphaned tool pairs after loading (prevents HTTP 400
    // from stale stored sessions where tool_call/tool_result pairs got
    // misaligned during a crash or interrupted write).
    repairOrphanedToolCalls();
    debugLogf("[Session] loadMessages: %zu messages (after repair)", messages.size());
}

void Session::clear() {
    std::lock_guard<std::mutex> lock(mtx);
    messages.clear();
}

void Session::popLastAssistant() {
    std::lock_guard<std::mutex> lock(mtx);
    if (!messages.empty() &&
        messages.back().role == "assistant" &&
        messages.back().toolCalls.empty()) {
        messages.pop_back();
        debugLog("[Session] popLastAssistant: removed orphaned assistant message");
    }
}

void Session::clearLastToolCalls(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mtx);
    if (messages.empty()) return;
    auto& last = messages.back();
    if (last.toolCalls.empty()) return;

    std::string rejection = prefix;
    for (size_t i = 0; i < last.toolCalls.size(); ++i) {
        if (i > 0) rejection += ", ";
        rejection += last.toolCalls[i].name;
    }
    rejection += "]";
    if (!last.content.empty()) last.content += "\n" + rejection;
    else last.content = rejection;
    last.toolCalls.clear();
    debugLogf("[Session] clearLastToolCalls: cleared %zu tool calls", last.toolCalls.size());
}

void Session::stripOrphanedToolCalls() {
    std::lock_guard<std::mutex> lock(mtx);
    size_t total = messages.size();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "assistant" && !it->toolCalls.empty()) {
            size_t msgIdx = total - 1 - std::distance(messages.rbegin(), it);
            std::string tcNames;
            for (const auto& tc : it->toolCalls) {
                if (!tcNames.empty()) tcNames += ", ";
                tcNames += tc.name + "(" + tc.id + ")";
            }
            debugLogf("[Session] stripOrphanedToolCalls: msg[%zu/%zu] role=assistant content=%zu bytes, stripping %zu tool_calls: [%s]",
                msgIdx, total, it->content.size(), it->toolCalls.size(), tcNames.c_str());

            if (it->content.empty()) {
                it->content = "[Tool calls stripped due to consecutive errors]";
            }
            it->toolCalls.clear();

            // Count trailing tool messages that will become orphans
            size_t orphanCount = 0;
            for (size_t j = msgIdx + 1; j < messages.size(); ++j) {
                if (messages[j].role == "tool") ++orphanCount;
                else if (messages[j].role != "system") break;
            }
            debugLogf("[Session] stripOrphanedToolCalls: %zu trailing tool msg(s) now orphaned (msg[%zu]..)",
                orphanCount, msgIdx + 1);
            break;
        }
    }
}

void Session::compressMessages(size_t& userTrimmed, size_t& assistantTrimmed,
                                size_t& toolArgsTrimmed, size_t& toolResultsTrimmed) {
    std::lock_guard<std::mutex> lock(mtx);
    userTrimmed = assistantTrimmed = toolArgsTrimmed = toolResultsTrimmed = 0;

    for (auto& m : messages) {
        if (m.role == "user" && m.content.size() > 1000) {
            std::string suffix = "\n... [truncated " + std::to_string(m.content.size() - 500) + " chars]";
            m.content = m.content.substr(0, 500) + suffix;
            ++userTrimmed;
        } else if (m.role == "assistant" && m.toolCalls.empty() && m.content.size() > 1000) {
            std::string suffix = "\n... [truncated " + std::to_string(m.content.size() - 500) + " chars]";
            m.content = m.content.substr(0, 500) + suffix;
            ++assistantTrimmed;
        } else if (m.role == "assistant" && !m.toolCalls.empty()) {
            for (auto& tc : m.toolCalls) {
                if (tc.arguments.size() > 300) {
                    tc.arguments = tc.arguments.substr(0, 300) + "... [truncated]";
                    ++toolArgsTrimmed;
                }
            }
        } else if (m.role == "tool" && m.content.size() > 500) {
            std::string suffix = "\n... [truncated " + std::to_string(m.content.size() - 500) + " chars]";
            m.content = m.content.substr(0, 500) + suffix;
            ++toolResultsTrimmed;
        }
    }
}

void Session::repairOrphanedToolCalls() {
    std::lock_guard<std::mutex> lock(mtx);

    size_t beforeCount = messages.size();
    debugLogf("[Session] repairOrphanedToolCalls: START with %zu messages", beforeCount);

    // Pass 1: fix orphaned tool_calls (assistant with tool_calls but missing results)
    size_t pass1Stripped = 0;
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role == "assistant" && !messages[i].toolCalls.empty()) {
            std::set<std::string> neededIds;
            for (const auto& tc : messages[i].toolCalls)
                if (!tc.id.empty()) neededIds.insert(tc.id);
            if (neededIds.empty()) continue;

            for (size_t j = i + 1; j < messages.size(); ++j) {
                if (messages[j].role == "tool" && !messages[j].toolCallId.empty()) {
                    neededIds.erase(messages[j].toolCallId);
                } else if (messages[j].role == "assistant") {
                    break;
                }
            }

            if (!neededIds.empty()) {
                debugLogf("[Session] repair Pass1: stripping %zu orphaned tool_calls from msg[%zu] (missing %zu tool results)",
                    messages[i].toolCalls.size(), i, neededIds.size());
                if (messages[i].content.empty()) {
                    messages[i].content = "[Tool calls repaired - results were missing]";
                }
                messages[i].toolCalls.clear();
                ++pass1Stripped;
            }
        }
    }
    debugLogf("[Session] repair Pass1: stripped %zu assistant tool_calls sets", pass1Stripped);

    // Pass 2: remove orphan tool results (tool messages with no preceding tool_calls)
    size_t pass2Removed = 0;
    for (auto it = messages.begin(); it != messages.end(); ) {
        if (it->role == "tool") {
            bool hasPrecedingToolCall = false;
            for (auto prev = messages.begin(); prev != it; ++prev) {
                if (prev->role == "assistant") {
                    for (const auto& tc : prev->toolCalls) {
                        if (!tc.id.empty() && tc.id == it->toolCallId) {
                            hasPrecedingToolCall = true;
                            break;
                        }
                    }
                    // Name fallback ONLY when toolCallId is empty (legacy tool msgs without IDs).
                    // If a toolCallId exists, it MUST match by ID — never by name,
                    // because name-matching can wrongly link an orphan tool msg to
                    // an unrelated earlier assistant that happened to use the same tool.
                    if (!hasPrecedingToolCall && !it->name.empty() && it->toolCallId.empty()) {
                        for (const auto& tc : prev->toolCalls) {
                            if (tc.name == it->name) {
                                hasPrecedingToolCall = true;
                                debugLogf("[Session] repair Pass2: matched orphan tool by name='%s' to assistant with same tool",
                                    it->name.c_str());
                                break;
                            }
                        }
                    }
                }
                if (hasPrecedingToolCall) break;
            }
            if (!hasPrecedingToolCall) {
                std::string contentPreview = it->content.size() > 80
                    ? it->content.substr(0, 80) + "..." : it->content;
                debugLogf("[Session] repair Pass2: removing orphan tool msg toolCallId='%s' name='%s' content=%zu bytes: \"%s\"",
                    it->toolCallId.c_str(), it->name.c_str(), it->content.size(),
                    safeForLog(contentPreview).c_str());
                it = messages.erase(it);
                ++pass2Removed;
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }
    debugLogf("[Session] repair Pass2: removed %zu orphan tool messages", pass2Removed);

    // Final tally: count tool messages remaining
    size_t toolCount = 0;
    for (const auto& m : messages) if (m.role == "tool") ++toolCount;
    debugLogf("[Session] repairOrphanedToolCalls: DONE %zu→%zu msgs (%zu removed), %zu tool msgs remain",
        beforeCount, messages.size(), beforeCount - messages.size(), toolCount);
}

void Session::validateToolCallPairs() const {
    std::lock_guard<std::mutex> lock(mtx);

    debugLogf("[Session] validateToolCallPairs: scanning %zu messages for tool-call-pair violations",
        messages.size());

    size_t violations = 0;
    size_t toolCount = 0;

    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role != "tool") continue;
        ++toolCount;

        // Find the nearest preceding assistant, then check for matching tool_calls
        bool found = false;
        for (size_t j = i; j > 0; ) {
            --j;
            if (messages[j].role == "assistant") {
                if (!messages[j].toolCalls.empty()) {
                    for (const auto& tc : messages[j].toolCalls) {
                        if (!tc.id.empty() && !messages[i].toolCallId.empty()
                            && tc.id == messages[i].toolCallId) {
                            found = true;
                            break;
                        }
                    }
                    if (!found && !messages[i].name.empty() && messages[i].toolCallId.empty()) {
                        for (const auto& tc : messages[j].toolCalls) {
                            if (tc.name == messages[i].name) {
                                found = true;
                                break;
                            }
                        }
                    }
                }
                break; // nearest assistant is the only one that matters
            } else if (messages[j].role == "tool") {
                continue; // skip over consecutive tool messages
            } else {
                break; // system/user/etc breaks the chain
            }
        }

        if (!found) {
            ++violations;
            std::string contentPreview = messages[i].content.size() > 100
                ? messages[i].content.substr(0, 100) + "..." : messages[i].content;
            LOG_F(WARNING, "[Session] VIOLATION #%zu: msg[%zu] role=tool toolCallId='%s' name='%s' content=%zu bytes: \"%s\"",
                violations, i,
                messages[i].toolCallId.c_str(),
                messages[i].name.c_str(),
                messages[i].content.size(),
                safeForLog(contentPreview).c_str());
        }
    }

    if (violations > 0) {
        LOG_F(ERROR, "[Session] validateToolCallPairs: *** %zu VIOLATION(S) *** in %zu messages (%zu tool msgs) → HTTP 400 WILL OCCUR!",
            violations, messages.size(), toolCount);
    } else {
        debugLogf("[Session] validateToolCallPairs: ALL CLEAR — 0 violations in %zu messages (%zu tool msgs)",
            messages.size(), toolCount);
    }
}

// -- JSON persistence (DEPRECATED) ------------------------------------

std::string Session::serialize() const {
    std::lock_guard<std::mutex> lock(mtx);
    debugLogf("[Session] serialize: %zu messages", messages.size());

    try {
        nlohmann::json j;
        nlohmann::json msgs = nlohmann::json::array();

        for (const auto& msg : messages) {
            nlohmann::json m = msg;
            msgs.push_back(m);
        }

        j["messages"] = msgs;
        std::string result = j.dump();
        debugLogf("[Session] serialize done: %zu bytes", result.size());
        return result;
    }
    catch (const std::exception& e) {
        debugLogf("[Session] serialize exception: %s", e.what());
        return "{\"messages\":[]}";
    }
}

bool Session::deserialize(const std::string& json) {
    std::lock_guard<std::mutex> lock(mtx);
    messages.clear();
    debugLogf("[Session] deserialize: %zu bytes", json.size());

    auto parseJson = [](const std::string& input) -> nlohmann::json {
        try {
            return nlohmann::json::parse(input);
        } catch (const nlohmann::json::parse_error&) {
            std::string cleaned = input;
            while (!cleaned.empty()) {
                size_t last = cleaned.find_last_not_of(" \t\r\n");
                if (last == std::string::npos) break;
                size_t lineStart = cleaned.rfind('\n', last);
                size_t contentStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
                std::string lastLine = cleaned.substr(contentStart, last - contentStart + 1);
                size_t firstNonSpace = lastLine.find_first_not_of(" \t");
                if (firstNonSpace != std::string::npos && lastLine[firstNonSpace] == '/')
                    cleaned.resize(contentStart);
                else
                    break;
            }
            debugLog("[Session] deserialize: stripped trailing comment lines (old format)");
            return nlohmann::json::parse(cleaned);
        }
    };

    try {
        nlohmann::json j = parseJson(json);
        if (!j.contains("messages") || !j["messages"].is_array()) {
            debugLog("[Session] deserialize: no messages array found");
            return false;
        }

        for (const auto& item : j["messages"]) {
            Message msg = item.get<Message>();
            if (!msg.role.empty())
                messages.push_back(msg);
        }
    } catch (const nlohmann::json::parse_error& e) {
        debugLogf("[Session] deserialize: JSON parse error: %s | input (safe): %s",
            e.what(), safeForLog(truncateForLog(json, 500)).c_str());
        return false;
    } catch (const std::exception& e) {
        debugLogf("[Session] deserialize: unexpected exception: %s", e.what());
        return false;
    }

    debugLogf("[Session] deserialize done: %zu messages", messages.size());
    return !messages.empty();
}

// ============================================================================
// Compaction — WorkingSet + semantic pinning + LLM summary
// ============================================================================

#include <regex>
#include <algorithm>
#include <cctype>

// -- path-like pattern: matches relative file paths in tool args and content --
static bool looksLikeFilePath(const std::string& s) {
    if (s.empty() || s.size() > 256) return false;
    // Must contain a dot extension or be a known root file
    if (s.find('.') == std::string::npos && s != "Makefile" && s != "CMakeLists.txt")
        return false;
    // No spaces, no control chars
    for (char c : s) {
        if (c <= 32 || c == '"' || c == '\'') return false;
    }
    return true;
}

Session::WorkingSet Session::deriveWorkingSet() const {
    WorkingSet ws;
    // Scan from most recent to oldest, collecting up to maxPaths
    auto msgs = getContextMessages();
    // Known tool argument keys that hold file paths
    static const std::vector<std::string> pathKeys = {
        "path", "file", "old_path", "new_path", "target", "cwd", "paths", "files"
    };

    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (ws.filePaths.size() >= (size_t)ws.maxPaths) break;

        // 1) Extract from tool_call arguments (JSON parsing)
        for (const auto& tc : it->toolCalls) {
            if (tc.arguments.empty()) continue;
            try {
                auto args = nlohmann::json::parse(tc.arguments);
                for (const auto& key : pathKeys) {
                    if (args.contains(key)) {
                        if (args[key].is_string()) {
                            std::string p = args[key].get<std::string>();
                            if (looksLikeFilePath(p)) ws.filePaths.insert(p);
                        } else if (args[key].is_array()) {
                            for (const auto& item : args[key]) {
                                if (item.is_string()) {
                                    std::string p = item.get<std::string>();
                                    if (looksLikeFilePath(p)) ws.filePaths.insert(p);
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }

        // 2) Extract from content text (simple regex for path patterns)
        if (!it->content.empty()) {
            std::regex pathRe(R"(([A-Za-z0-9._\-]+/)+[A-Za-z0-9._\-]+\.\w{1,8})");
            auto begin = std::sregex_iterator(it->content.begin(), it->content.end(), pathRe);
            auto end = std::sregex_iterator();
            for (auto i = begin; i != end && ws.filePaths.size() < (size_t)ws.maxPaths; ++i) {
                ws.filePaths.insert(i->str());
            }
        }
    }
    debugLogf("[Session] deriveWorkingSet: found %zu file paths", ws.filePaths.size());
    return ws;
}

bool Session::shouldPinMessage(size_t msgIdx, const WorkingSet& ws) const {
    auto msgs = getContextMessages();
    if (msgIdx >= msgs.size()) return false;
    const auto& msg = msgs[msgIdx];

    std::string lower = msg.content;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    // 1) Mentions a working-set file path
    for (const auto& path : ws.filePaths) {
        if (msg.content.find(path) != std::string::npos) return true;
    }

    // 2) Contains error markers
    static const std::vector<std::string> errMarkers = {
        "error:", "error ", "failed", "panic", "traceback",
        "stack trace", "assertion failed", "test failed"
    };
    for (const auto& m : errMarkers) {
        if (lower.find(m) != std::string::npos) return true;
    }

    // 3) Contains patch/diff markers
    static const std::vector<std::string> patchMarkers = {
        "diff --git", "+++ b/", "--- a/", "```diff", "apply_patch"
    };
    for (const auto& m : patchMarkers) {
        if (lower.find(m) != std::string::npos) return true;
    }

    // 4) Mentions TODO/checklist
    if (lower.find("todo") != std::string::npos ||
        lower.find("checklist") != std::string::npos) return true;

    return false;
}

void Session::enforceToolCallPairs(CompactionPlan& plan) const {
    auto msgs = getContextMessages();
    // Build maps: tool_call_id → message index
    std::map<std::string, size_t> callIdToIdx;
    std::map<std::string, size_t> resultIdToIdx;

    for (size_t i = 0; i < msgs.size(); ++i) {
        for (const auto& tc : msgs[i].toolCalls) {
            if (!tc.id.empty()) callIdToIdx[tc.id] = i;
        }
        if (!msgs[i].toolCallId.empty()) resultIdToIdx[msgs[i].toolCallId] = i;
    }

    // If a tool result is pinned, its call must also be pinned.
    // If a call is pinned, its result must also be pinned.
    std::set<size_t> pinned(plan.pinnedIndices.begin(), plan.pinnedIndices.end());
    bool changed = true;
    int maxIter = 10;
    while (changed && maxIter-- > 0) {
        changed = false;
        for (size_t idx : pinned) {
            // Is this a tool result?
            if (!msgs[idx].toolCallId.empty()) {
                auto it = callIdToIdx.find(msgs[idx].toolCallId);
                if (it != callIdToIdx.end() && pinned.insert(it->second).second)
                    changed = true;
            }
            // Is this a call?
            for (const auto& tc : msgs[idx].toolCalls) {
                if (!tc.id.empty()) {
                    auto it = resultIdToIdx.find(tc.id);
                    if (it != resultIdToIdx.end() && pinned.insert(it->second).second)
                        changed = true;
                }
            }
        }
    }
    plan.pinnedIndices.assign(pinned.begin(), pinned.end());
}

Session::CompactionPlan Session::planCompaction(size_t keepRecent) const {
    auto msgs = getContextMessages();
    CompactionPlan plan;
    size_t n = msgs.size();
    if (n <= keepRecent) {
        // Not enough messages — pin everything
        for (size_t i = 0; i < n; ++i) plan.pinnedIndices.push_back(i);
        return plan;
    }

    std::set<size_t> pinned;

    // 1) Pin the tail (most recent messages)
    for (size_t i = n - keepRecent; i < n; ++i) pinned.insert(i);

    // 2) Derive working set from all messages, then pin semantically important ones
    WorkingSet ws = deriveWorkingSet();
    for (size_t i = 0; i < n - keepRecent; ++i) {
        if (shouldPinMessage(i, ws)) pinned.insert(i);
    }

    // 3) Enforce tool-call pairs
    plan.pinnedIndices.assign(pinned.begin(), pinned.end());
    enforceToolCallPairs(plan);

    // 4) Ensure at least one user message is in pinned set (API requirement)
    bool hasUser = false;
    for (size_t idx : pinned) {
        if (idx < n && msgs[idx].role == "user") { hasUser = true; break; }
    }
    if (!hasUser) {
        for (size_t i = n; i > 0; --i) {
            if (msgs[i-1].role == "user") { pinned.insert(i-1); break; }
        }
    }

    // 5) Build summarize list from non-pinned indices
    plan.pinnedIndices.assign(pinned.begin(), pinned.end());
    for (size_t i = 0; i < n; ++i) {
        if (pinned.find(i) == pinned.end())
            plan.summarizeIndices.push_back(i);
    }

    debugLogf("[Session] planCompaction: %zu msgs total → %zu pinned, %zu to summarize",
        n, plan.pinnedIndices.size(), plan.summarizeIndices.size());
    return plan;
}

std::string Session::buildCompactionInput(const CompactionPlan& plan) const {
    auto msgs = getContextMessages();
    std::ostringstream out;

    for (size_t idx : plan.summarizeIndices) {
        if (idx >= msgs.size()) continue;
        const auto& msg = msgs[idx];
        out << "[" << msg.role << "]";
        if (!msg.name.empty()) out << " (" << msg.name << ")";
        out << ":\n";

        // Truncate long content
        size_t maxLen = 2000;
        if (msg.role == "tool") maxLen = 500;
        std::string text = msg.content;
        if (text.size() > maxLen) {
            text = text.substr(0, maxLen) + "\n... [truncated " +
                   std::to_string(text.size() - maxLen) + " bytes]";
        }
        out << text << "\n\n";

        // Tool calls: show name + args
        for (const auto& tc : msg.toolCalls) {
            std::string args = tc.arguments;
            if (args.size() > 200) args = args.substr(0, 200) + "...";
            out << "  [tool_call: " << tc.name << " " << args << "]\n";
        }

        if (out.str().size() > 24000) {
            out << "... (truncated at 24K chars)\n";
            break;
        }
    }
    return out.str();
}

void Session::applyCompaction(const CompactionPlan& plan, const std::string& summary) {
    std::lock_guard<std::mutex> lock(mtx);

    if (plan.summarizeIndices.empty()) return;
    if (summary.empty()) return;

    // Collect target indices in descending order (erase from back to front)
    std::vector<size_t> targets = plan.summarizeIndices;
    std::sort(targets.rbegin(), targets.rend());

    // Remove all target messages
    for (size_t idx : targets) {
        if (idx < messages.size())
            messages.erase(messages.begin() + idx);
    }

    // Insert summary at a fixed position: right after all system messages.
    // This keeps the system prefix stable: the system block always occupies
    // indices [0, sysCount), and user/assistant/tool messages start at
    // sysCount.  Without this, the summary would be inserted at the old
    // position of the first compacted message, shifting everything after it
    // and breaking DeepSeek's KV prefix cache.
    size_t sysCount = 0;
    while (sysCount < messages.size() && messages[sysCount].role == "system")
        ++sysCount;
    size_t insertAt = sysCount;

    // Insert summary as system message
    std::string fullMsg = "⚠️ [Context compacted: " +
        std::to_string(targets.size()) + " messages summarized]\n\n" + summary;
    messages.insert(messages.begin() + insertAt, Message::System(fullMsg));

    // Repair orphaned tool calls inline (no lock — mtx is already held).
    // We cannot call repairOrphanedToolCalls() here because it tries to
    // lock mtx again, causing a double-lock (undefined behavior → crash).
    // Compaction already enforces pairs via enforceToolCallPairs, so
    // orphaned pairs after compaction are rare; do a quick inline scan.
    {
        // Pass 1: strip orphaned tool_calls (assistant with tool_calls but no results)
        for (auto& m : messages) {
            if (m.role == "assistant" && !m.toolCalls.empty()) {
                bool hasResult = false;
                for (size_t j = 0; j < messages.size(); ++j) {
                    if (messages[j].role == "tool") {
                        for (const auto& tc : m.toolCalls) {
                            if (!tc.id.empty() && messages[j].toolCallId == tc.id) {
                                hasResult = true; break;
                            }
                        }
                        if (hasResult) break;
                    }
                }
                if (!hasResult) {
                    if (m.content.empty()) m.content = "[Tool calls cleaned up]";
                    m.toolCalls.clear();
                }
            }
        }
        // Pass 2: remove orphan tool results (no preceding tool_calls)
        for (auto it = messages.begin(); it != messages.end(); ) {
            if (it->role == "tool" && !it->toolCallId.empty()) {
                bool found = false;
                for (auto prev = messages.begin(); prev != it; ++prev) {
                    if (prev->role == "assistant") {
                        for (const auto& tc : prev->toolCalls) {
                            if (!tc.id.empty() && tc.id == it->toolCallId) { found = true; break; }
                        }
                        if (found) break;
                    }
                }
                if (!found) it = messages.erase(it);
                else ++it;
            } else { ++it; }
        }
    }

    debugLogf("[Session] Compaction applied: %zu msgs → summary at index %zu",
        targets.size(), insertAt);
}

// ============================================================================
// hasOrphanedTools — O(n) quick detection, does NOT modify messages
// ============================================================================

bool Session::hasOrphanedTools() const {
    std::lock_guard<std::mutex> lock(mtx);

    // Scan: find any tool message without a matching preceding assistant
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role == "tool") {
            bool found = false;
            for (size_t j = i; j > 0; ) {
                --j;
                if (messages[j].role == "assistant") {
                    if (!messages[j].toolCalls.empty()) {
                        for (const auto& tc : messages[j].toolCalls) {
                            if (!tc.id.empty() && !messages[i].toolCallId.empty()
                                && tc.id == messages[i].toolCallId) {
                                found = true; break;
                            }
                        }
                        if (!found && !messages[i].name.empty() && messages[i].toolCallId.empty()) {
                            for (const auto& tc : messages[j].toolCalls) {
                                if (tc.name == messages[i].name) { found = true; break; }
                            }
                        }
                    }
                    break;
                } else if (messages[j].role == "tool") {
                    continue;
                } else {
                    break;
                }
            }
            if (!found) {
                debugLogf("[Session] hasOrphanedTools: found orphan tool msg[%zu] toolCallId='%s' name='%s'",
                    i, messages[i].toolCallId.c_str(), messages[i].name.c_str());
                return true;
            }
        }
        // Also check: assistant with tool_calls but missing tool results
        if (messages[i].role == "assistant" && !messages[i].toolCalls.empty()) {
            for (const auto& tc : messages[i].toolCalls) {
                if (tc.id.empty()) continue;
                bool hasResult = false;
                for (size_t k = i + 1; k < messages.size(); ++k) {
                    if (messages[k].role == "tool" && messages[k].toolCallId == tc.id) {
                        hasResult = true; break;
                    }
                    if (messages[k].role == "assistant") break;
                }
                if (!hasResult) {
                    debugLogf("[Session] hasOrphanedTools: msg[%zu] has tool_call '%s' with no result",
                        i, tc.name.c_str());
                    return true;
                }
            }
        }
    }
    return false;
}

// ============================================================================
// ContextBudget — pressure level computation
// ============================================================================

Session::PressureLevel Session::ContextBudget::getPressure(size_t estimatedTokens) const {
    if (availableInput == 0) return PressureLevel::Low;
    double pct = 100.0 * static_cast<double>(estimatedTokens) / static_cast<double>(availableInput);
    if (pct >= criticalTrigger * 100.0) return PressureLevel::Critical;
    if (pct >= compactionTrigger * 100.0) return PressureLevel::High;
    if (pct >= 40.0) return PressureLevel::Medium;
    return PressureLevel::Low;
}

Session::ContextBudget Session::computeContextBudget(size_t windowTokens, size_t maxTokens) {
    ContextBudget budget;
    budget.windowTokens = windowTokens;
    budget.reservedOutput = (maxTokens > 0) ? maxTokens : 8192;
    budget.headroomTokens = 1024;
    // availableInput = window - maxOutputTokens - headroom, guaranteed >= 1024
    if (windowTokens > budget.reservedOutput + budget.headroomTokens) {
        budget.availableInput = windowTokens - budget.reservedOutput - budget.headroomTokens;
    } else {
        // Window too small for meaningful reserve — set minimum
        budget.availableInput = windowTokens / 2;
    }
    return budget;
}
