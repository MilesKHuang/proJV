// SKILL_ENGINE -- generic role runtime (see SKILL_ENGINE.md v1.1, section 4.1/4.2).
// One LLM loop: register verb + capability tool defs, call the model, execute
// side tools (file_requests / registry / request_agent), return raw args JSON.
// Semantically blind: it does NOT know any skill's field names.
#pragma once
#include "client/deepseek.h"
#include "core/session.h"
#include "tools/registry.h"

#include <functional>
#include <string>
#include <vector>

// Test seam: replaces the real API call. Empty == real API path.
using SubAgentResponder =
    std::function<ToolCall(const std::string& verb, const std::vector<Message>&)>;

class SubAgent {
public:
    SubAgent(std::string name, std::string systemPrompt);

    void configure(const AppConfig& cfg, ToolRegistry* tools,
                   const std::vector<std::string>& whitelist, int maxToolIters);
    void registerToolDef(const ToolDefinition& def);
    void seedSharedContext(const std::string& text);
    void setToolInterceptor(std::function<std::string(const std::string&, const std::string&)> cb) {
        toolInterceptor_ = std::move(cb);
    }
    void setResponder(SubAgentResponder r) { responder_ = std::move(r); }

    // Run one interaction: returns the verb tool's raw args JSON, a plain-text
    // fallback, an "[error] ..." string, or "" (no tool call and no text).
    std::string turn(const std::string& message, const std::string& verb, bool force);

    // First registered tool name (used as the default verb for request_agent
    // targets that have no explicit action in the program).
    std::string firstToolName() const {
        return toolDefs_.empty() ? std::string() : toolDefs_.front().name;
    }

    void cancel() { client_.cancel(); }

private:
    ToolCall doRequest(const std::string& verb, bool force, std::string& outText);
    ToolCall requestTool(const std::string& verb, bool force, std::string& outText);
    std::string executeFileRequests(const std::vector<std::string>& paths);
    std::string dispatchSide(const ToolCall& tc);

    std::string name_;
    std::string systemPrompt_;
    AppConfig cfg_;
    ToolRegistry* tools_ = nullptr;
    std::vector<std::string> toolWhitelist_;
    int maxToolIters_ = 3;
    DeepSeekClient client_;
    Session session_;
    std::vector<ToolDefinition> toolDefs_;
    std::string lastReasoning_;
    std::string lastError_;
    std::function<std::string(const std::string&, const std::string&)> toolInterceptor_;
    SubAgentResponder responder_;
};
