#pragma once
#include "client/deepseek.h"
#include "core/session.h"
#include "tools/registry.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Test seam: when installed on a participant, replaces the real API call. It
// receives the forced tool name and the current session messages and returns
// the model's reply (valid=false simulates "no tool call"). Empty == real API.
using BigbangResponder =
    std::function<ToolCall(const std::string&, const std::vector<Message>&)>;

// Structured result from a bigbang_vote tool call.
struct VoteResult {
    bool agree = false;
    std::vector<std::string> agreedPoints;
    std::vector<std::string> concerns;      // each entry "[severity] issue"
    std::string suggestedTweak;
};

// One debate role: independent DeepSeekClient + Session, no Agent coupling.
// NOTE: holds DeepSeekClient (atomic+mutex) and Session (mutex), so it is
// non-copyable and non-movable. Always own it via std::unique_ptr / std::array.
class BigbangParticipant {
public:
    BigbangParticipant(std::string name, std::string systemPrompt, bool canWriteDoc);

    void configure(const AppConfig& cfg, ToolRegistry* tools, int maxFileRounds);

    // Inject a shared-context block (the main session transcript) as an extra
    // system message so the role can resolve references like "this project".
    void seedSharedContext(const std::string& text);

    // Test seam (see BigbangResponder). Empty keeps the real API path.
    void setResponder(BigbangResponder r) { responder_ = std::move(r); }

    // Phase 1/2: propose or integrate. Returns the final statement.
    std::string turn(const std::string& userMessage);

    // Phase 3: vote.
    VoteResult vote(const std::string& userMessage);

    // Phase 5 (Leonard only): produce the execution document markdown.
    std::string writeDoc(const std::string& userMessage);

    const std::string& name() const { return name_; }
    void cancel() { client_.cancel(); }

private:
    std::vector<ToolDefinition> toolDefs() const;

    // Run one streaming request. `force` requests a specific function
    // (tool_choice=function); false uses tool_choice=auto. Returns the merged
    // tool call (valid=false when the model produced no tool call).
    ToolCall doRequest(const std::string& toolName, bool force, std::string& outText);

    // Wrapper: try forcing `toolName`; if the model rejects a forced
    // tool_choice (thinking mode), retry once with auto.
    ToolCall requestTool(const std::string& toolName, std::string& outText);

    std::string executeFileRequests(const std::vector<std::string>& paths);

    std::string name_;
    std::string systemPrompt_;
    bool canWriteDoc_ = false;
    AppConfig cfg_;
    ToolRegistry* tools_ = nullptr;
    int maxFileRounds_ = 2;
    DeepSeekClient client_;
    Session session_;
    std::string lastReasoning_;   // reasoning_content of the last request
    std::string lastError_;       // last HTTP/stream error (empty = ok)
    BigbangResponder responder_;  // test seam; empty in production
};

// Orchestrates the /bigbang debate across Sheldon, Penny and Leonard.
class Roundtable {
public:
    struct Callbacks {
        // Projection: persist a role statement into the main session.
        std::function<void(const std::string& role, const std::string& text)> onStatement;
        // Projection: persist a vote summary.
        std::function<void(const std::string& role, const VoteResult& v)> onVote;
        // Projection: final execution document.
        std::function<void(const std::string& docMarkdown)> onDoc;
        // Live phase status for the UI.
        std::function<void(const std::string& status)> onProgress;
    };

    Roundtable(AppConfig cfg, ToolRegistry* tools, Callbacks cbs,
               std::string sharedContext = "");

    // Blocking: runs the whole debate on the calling thread.
    void run(const std::string& topic);

    void cancel();

    int lastRounds() const { return lastRounds_; }
    bool converged() const { return converged_; }
    const std::string& lastDoc() const { return lastDoc_; }

    // Test seams: scripted responder per role (0=Sheldon,1=Penny,2=Leonard)
    // and a round-cap override. Unused in production.
    void setResponder(int idx, BigbangResponder r) {
        if (idx >= 0 && idx < 3 && parts_[idx]) parts_[idx]->setResponder(std::move(r));
    }
    void setMaxRounds(int n) { maxRounds_ = n; }

private:
    void progress(const std::string& s);
    std::string buildVoteSummary() const;
    std::string buildResidualConcerns() const;

    AppConfig cfg_;
    ToolRegistry* tools_ = nullptr;
    Callbacks cbs_;
    int maxRounds_ = 8;
    int lastRounds_ = 0;
    bool converged_ = false;
    std::string lastDoc_;

    std::atomic<bool> cancelRequested_{false};

    // 0 = Sheldon, 1 = Penny, 2 = Leonard.
    std::array<std::unique_ptr<BigbangParticipant>, 3> parts_;

    // Cross-round state (only touched on the debate thread).
    std::string sheldonS_, pennyS_, leonardS_, prevLeonardS_;
    std::array<VoteResult, 3> votes_;
};
