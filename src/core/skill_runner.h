// SKILL_ENGINE -- blackboard interpreter (see SKILL_ENGINE.md v1.1, section 4.3/4.4).
// Owns a pool of SubAgents, runs a JSON-configured program round by round,
// merges results into named slots / votes, and emits unified events.
#pragma once
#include "models.h"
#include "core/skill_config.h"
#include "tools/registry.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SubAgent;

class SkillRunner {
public:
    struct Callbacks {
        // kind: "message" | "vote" | "document" | "system"
        std::function<void(const std::string& role, const std::string& kind,
                           const std::string& text)> onEvent;
        std::function<void(const std::string& status)> onProgress;
    };

    SkillRunner(AppConfig cfg, ToolRegistry* tools, Callbacks cbs,
                std::string sharedContext = "");
    ~SkillRunner();

    // Blocking: runs the whole skill on the calling thread.
    void run(const std::string& skillName, const std::string& topic);
    void cancel();

    // Test seam: scripted reply for one agent (empty == real API).
    using Responder = std::function<ToolCall(const std::string&, const std::vector<Message>&)>;
    void setResponder(const std::string& agentId, Responder r) { responders_[agentId] = std::move(r); }

    bool converged() const { return converged_; }
    int lastRounds() const { return lastRounds_; }
    const std::string& lastDoc() const { return lastDoc_; }

    // Directory helpers / seeding (mirror ensureDefaultPrompts).
    static std::vector<std::string> listSkills(const std::string& skillsDir);
    static void ensureDefaultSkills(const std::string& skillsDir);

private:
    void loadAgents();
    void executeSteps(const std::vector<SkillStep>& steps, int round);
    void mergeAction(const SkillAction& a, const std::string& result, int round);
    bool stopRule(int round) const;
    bool whenMatches(const std::string& when, int round) const;
    std::string fill(const std::string& tmpl, int round,
                     const std::map<std::string, std::string>& vars) const;
    std::string roundOutputs() const;
    std::string voteSummary() const;
    std::string residualConcerns() const;
    std::string voteLine(const VoteResult& vr) const;
    std::string display(const std::string& agentId) const;
    std::string handleRequestAgent(const std::string& id, const std::string& msg, int depth);

    AppConfig cfg_;
    ToolRegistry* tools_ = nullptr;
    Callbacks cbs_;
    std::string sharedContext_;
    std::string skillsDir_;
    std::string topic_;
    SkillConfig config_;

    std::map<std::string, std::unique_ptr<SubAgent>> agents_;
    std::map<std::string, Responder> responders_;
    std::map<std::string, std::string> slots_;       // text slots (this round)
    std::map<std::string, std::string> prevSlots_;   // snapshot at end of round
    std::map<std::string, VoteResult> votes_;        // keyed by voting agent id
    std::map<std::string, std::string> displayNames_;
    std::string roundContext_;                       // last round's vote summary

    bool converged_ = false;
    int lastRounds_ = 0;
    std::string lastDoc_;
    std::atomic<bool> cancelRequested_{false};
};
