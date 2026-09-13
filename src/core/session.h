#pragma once
#include "models.h"
#include <mutex>
#include <vector>
#include <string>
#include <set>

struct Session {
    // --- Message access (messages is private, all access through methods) ---

    // Thread-safe add a message
    void addMessage(const Message& msg);

    // Thread-safe read: returns a copy of all messages
    std::vector<Message> getContextMessages() const;

    // Thread-safe message count
    size_t messageCount() const;

    // Replace all messages (used by loadFromStorage)
    void loadMessages(std::vector<Message>&& msgs);

    // Clear all messages
    void clear();

    // Pop the last message if it is an assistant message without tool_calls.
    // Used by HTTP retry cleanup.
    void popLastAssistant();

    // Clear tool_calls from the last assistant message and append a rejection notice.
    void clearLastToolCalls(const std::string& prefix);

    // Strip orphaned tool_calls: find the last assistant with tool_calls,
    // set placeholder content, clear tool_calls. For error recovery.
    void stripOrphanedToolCalls();

    // Validate every tool message has a preceding assistant+tool_calls pair.
    // Logs WARNING with full details on violations. Call before buildRequestBody.
    void validateToolCallPairs() const;

    // In-place message compression: trim long user/assistant messages,
    // truncate tool call args and tool results. Returns trim counts.
    void compressMessages(size_t& userTrimmed, size_t& assistantTrimmed,
                          size_t& toolArgsTrimmed, size_t& toolResultsTrimmed);

    // Repair orphaned tool_calls and tool results (for HTTP 400 recovery).
    void repairOrphanedToolCalls();

    // --- Context window management ---
    void pruneForContext(size_t maxTokens);
    static size_t estimateTokens(const std::string& text);

    // Strip the last assistant(tool_calls) message and its tool result followers.
    void stripLastToolCallsAndResults();

    // Soft strip: merges the following tool result content INTO the assistant
    // message content, then removes the tool_calls and tool result messages.
    void softStripLastToolCalls();

    // Quick detection: returns true if orphaned tool messages exist (O(n) scan).
    // Does NOT modify messages. Call before buildRequestBody as a cheap check.
    bool hasOrphanedTools() const;
    std::string serialize() const;
    bool deserialize(const std::string& json);

    // =================================================================
    // Context budget — dynamic window-aware pressure management
    // =================================================================

    enum class PressureLevel { Low, Medium, High, Critical };

    struct ContextBudget {
        size_t windowTokens = 1'048'576;   // model context window (default: 1M)
        size_t reservedOutput = 16384;      // space for model output
        size_t headroomTokens = 1024;       // safety margin
        size_t availableInput = 0;          // computed: window - reserved - headroom

        double compactionTrigger = 0.75;    // 75% → trigger LLM compaction
        double criticalTrigger = 0.90;      // 90% → emergency pruning

        PressureLevel getPressure(size_t estimatedTokens) const;
    };

    // Compute a ContextBudget from window size and max tokens.
    // Returns sensible defaults for reserve/headroom.
    static ContextBudget computeContextBudget(size_t windowTokens, size_t maxTokens);

    // =================================================================
    // Compaction planning (LLM-driven semantic compression)
    // =================================================================

    struct CompactionPlan {
        std::vector<size_t> pinnedIndices;     // keep untouched
        std::vector<size_t> summarizeIndices;  // send to LLM for summarization
    };

    // Working set: file paths extracted from recent messages to guide pinning.
    struct WorkingSet {
        std::set<std::string> filePaths;
        int maxPaths = 24;
    };

    // Plan which messages to compact: keep recent N + semantically important ones.
    CompactionPlan planCompaction(size_t keepRecent = 4) const;

    // Build the text input to send to the LLM for compaction summarization.
    std::string buildCompactionInput(const CompactionPlan& plan) const;

    // Replace summarized messages with a single system message containing
    // the LLM-generated summary.
    void applyCompaction(const CompactionPlan& plan, const std::string& summary);

private:
    // -- Working set helpers --
    WorkingSet deriveWorkingSet() const;
    bool shouldPinMessage(size_t msgIdx, const WorkingSet& ws) const;

    // -- Tool-call pair enforcement for compaction --
    void enforceToolCallPairs(CompactionPlan& plan) const;
    std::vector<Message> messages;
    mutable std::mutex mtx;  // protects messages
};
