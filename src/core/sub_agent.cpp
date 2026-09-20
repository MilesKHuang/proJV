// SKILL_ENGINE -- SubAgent implementation. Logic mirrors the old
// BigbangParticipant (roundtable.cpp): same tool_choice force/auto fallback,
// same reasoning_content echo, same file-request round cap. Made generic.
#include "sub_agent.h"
#include "json.hpp"
#include "debug_log.h"

#include <set>
#include <utility>

SubAgent::SubAgent(std::string name, std::string systemPrompt)
    : name_(std::move(name)), systemPrompt_(std::move(systemPrompt)) {
    session_.addMessage(Message::System(systemPrompt_));
}

void SubAgent::configure(const AppConfig& cfg, ToolRegistry* tools,
                         const std::vector<std::string>& whitelist, int maxToolIters) {
    cfg_ = cfg;
    tools_ = tools;
    toolWhitelist_ = whitelist;
    maxToolIters_ = maxToolIters;
    client_.setConfig(cfg);
}

void SubAgent::registerToolDef(const ToolDefinition& def) {
    for (auto& d : toolDefs_) if (d.name == def.name) return;   // dedup
    toolDefs_.push_back(def);
}

void SubAgent::seedSharedContext(const std::string& text) {
    if (text.empty()) return;
    session_.addMessage(Message::System(text));
}

std::vector<ToolCall> SubAgent::doRequest(const std::string& verb, bool force, std::string& outText) {
    ChatRequest req;
    req.model = cfg_.model;
    req.maxTokens = cfg_.maxTokens;
    req.temperature = cfg_.temperature;
    req.stream = true;
    // Thinking-mode models reject a forced function tool_choice; force=false
    // falls back to "auto" so the model still picks the tool itself.
    req.toolChoice = force ? verb : std::string("auto");
    req.tools = toolDefs_;
    req.messages = session_.getContextMessages();

    if (multiResponder_) {
        outText.clear();
        lastReasoning_.clear();
        lastError_.clear();
        return multiResponder_(verb, req.messages);
    }
    if (responder_) {
        outText.clear();
        lastReasoning_.clear();
        lastError_.clear();
        ToolCall c = responder_(verb, req.messages);
        if (!c.valid) return {};
        return { c };
    }

    std::vector<ToolCall> parts;
    bool got = false;
    outText.clear();
    lastReasoning_.clear();
    lastError_.clear();

    StreamCallbacks cb;
    cb.onText = [&outText](const std::string& t) { outText += t; };
    // Thinking-mode models require reasoning_content echoed on the next request.
    cb.onThinking = [this](const std::string& t) { lastReasoning_ += t; };
    cb.onToolCall = [&parts, &got](const ToolCall& tc) {
        int idx = tc.index < 0 ? 0 : tc.index;
        if (static_cast<int>(parts.size()) <= idx) parts.resize(idx + 1);
        if (!tc.id.empty()) parts[idx].id = tc.id;
        if (!tc.name.empty()) parts[idx].name = tc.name;
        parts[idx].arguments += tc.arguments;
        parts[idx].index = idx;
        got = true;
    };
    cb.onError = [this](const std::string& e) { lastError_ = e; };

    client_.streamBlocking(req, cb);
    if (!lastError_.empty()) {
        debugLogf("[Skill][%s] '%s' failed: %s",
                  name_.c_str(), verb.c_str(), lastError_.c_str());
    }

    // Return ALL calls in this response (parallel tool calling), not just one.
    std::vector<ToolCall> result;
    if (got) for (auto& p : parts) if (!p.name.empty()) result.push_back(p);
    return result;
}

std::vector<ToolCall> SubAgent::requestTool(const std::string& verb, bool force, std::string& outText) {
    outText.clear();
    std::vector<ToolCall> calls = doRequest(verb, force, outText);   // honor force
    if (calls.empty() && force && lastError_.find("tool_choice") != std::string::npos) {
        debugLogf("[Skill][%s] forced tool_choice rejected, retrying with auto",
                  name_.c_str());
        calls = doRequest(verb, false, outText);
    }
    return calls;
}

std::string SubAgent::executeFileRequests(const std::vector<std::string>& paths) {
    std::string out;
    std::set<std::string> seen;
    int n = 0;
    for (const auto& p : paths) {
        if (p.empty() || seen.count(p)) continue;
        if (n >= 3) { out += "\n[skill] file_requests truncated to 3 per turn.\n"; break; }
        seen.insert(p);
        ++n;
        std::string args = nlohmann::json{{"path", p}}.dump();
        std::string res = tools_ ? tools_->execute("read_file", args)
                                 : std::string("Error: no tool registry available");
        out += "===== " + p + " =====\n" + res + "\n";
    }
    if (out.empty()) out = "(no file content)";
    return out;
}

std::string SubAgent::dispatchSide(const ToolCall& tc) {
    if (tc.name == "request_agent" && toolInterceptor_)
        return toolInterceptor_(tc.name, tc.arguments);
    for (const auto& w : toolWhitelist_)
        if (w == tc.name)
            return tools_ ? tools_->execute(tc.name, tc.arguments)
                          : std::string("Error: no tool registry available");
    return "Error: tool '" + tc.name + "' not allowed";
}

std::string SubAgent::turn(const std::string& message, const std::string& verb, bool force) {
    session_.addMessage(Message::User(message));
    std::string narr;   // last assistant narration text (used if the verb tool never fires)
    for (int r = 0; r < maxToolIters_; ++r) {
        std::string text;
        std::vector<ToolCall> calls = requestTool(verb, force, text);
        if (!text.empty()) narr = text;
        if (calls.empty()) {
            std::string out;
            if (!lastError_.empty()) out = "[error] " + lastError_;
            else if (!text.empty()) out = text;
            if (!out.empty()) session_.addMessage(Message::Assistant(out));
            return out;
        }

        Message am = Message::Assistant();
        am.toolCalls = calls;
        am.reasoningContent = lastReasoning_;
        session_.addMessage(am);

        int verbIdx = -1;
        for (int i = 0; i < static_cast<int>(calls.size()); ++i)
            if (calls[i].name == verb) { verbIdx = i; break; }

        bool finalize = false;
        if (verbIdx >= 0) {
            std::vector<std::string> files;
            try {
                auto args = nlohmann::json::parse(calls[verbIdx].arguments);
                if (args.contains("file_requests") && args["file_requests"].is_array())
                    for (const auto& v : args["file_requests"])
                        if (v.is_string()) files.push_back(v.get<std::string>());
            } catch (...) {}
            if (!files.empty() && r < maxToolIters_ - 1) {
                session_.addMessage(Message::Tool(calls[verbIdx].id, verb, executeFileRequests(files)));
            } else {
                session_.addMessage(Message::Tool(calls[verbIdx].id, verb, "OK"));
                finalize = true;
            }
        }
        // Every call in the response gets a tool result (no orphan tool_calls).
        for (int i = 0; i < static_cast<int>(calls.size()); ++i) {
            if (i == verbIdx) continue;
            session_.addMessage(Message::Tool(calls[i].id, calls[i].name, dispatchSide(calls[i])));
        }
        if (finalize) return calls[verbIdx].arguments;
    }
    return narr;   // loop exhausted without the verb: surface narration so the turn is visible
}
