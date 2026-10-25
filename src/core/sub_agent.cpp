// SKILL_ENGINE -- SubAgent implementation. Logic mirrors the old
// BigbangParticipant (roundtable.cpp): same tool_choice force/auto fallback,
// same reasoning_content echo, same file-request round cap. Made generic.
#include "sub_agent.h"
#include "tools/file_tool.h"
#include "json.hpp"
#include "debug_log.h"

#include <set>
#include <utility>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace {
// Clears the busy flag on every exit path of turn().
struct BusyFlag {
    std::atomic<bool>* flag;
    ~BusyFlag() { flag->store(false, std::memory_order_relaxed); }
};

// Bounded concurrency for request_agent groups: max_parallel == in flight.
class Semaphore {
public:
    explicit Semaphore(int n) : slots_(n < 1 ? 1 : n) {}
    void acquire() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return slots_ > 0; });
        --slots_;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(m_);
            ++slots_;
        }
        cv_.notify_one();
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    int slots_;
};
} // namespace

SubAgent::SubAgent(std::string name, std::string systemPrompt)
    : name_(std::move(name)), systemPrompt_(std::move(systemPrompt)) {
    session_.addMessage(Message::System(systemPrompt_));
}

void SubAgent::configure(const AppConfig& cfg, ToolRegistry* tools,
                         const std::vector<std::string>& whitelist, int maxToolIters,
                         bool thinking) {
    cfg_ = cfg;
    tools_ = tools;
    toolWhitelist_ = whitelist;
    maxToolIters_ = maxToolIters;
    thinking_ = thinking;
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

void SubAgent::setNarration(const std::string& s) {
    std::lock_guard<std::mutex> lk(narrMutex_);
    lastNarration_ = s;
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
    // Forced tools and thinking mode are mutually exclusive: disable thinking
    // whenever we pin the call, otherwise honour the role's setting.
    req.thinking = force ? false : thinking_;
    req.tools = toolDefs_;
    req.messages = session_.getContextMessages();

    if (multiResponder_) {
        outText = textResponder_ ? textResponder_(verb, req.messages) : std::string();
        lastReasoning_.clear();
        lastError_.clear();
        return multiResponder_(verb, req.messages);
    }
    if (responder_) {
        outText = textResponder_ ? textResponder_(verb, req.messages) : std::string();
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
        // "path#L120-L480" -> read_file(path, start_line=120, end_line=480).
        std::string fpath;
        int startLine = 0, endLine = 0;
        parseFileRequestSpec(p, fpath, startLine, endLine);
        nlohmann::json ja{{"path", fpath}};
        if (startLine > 0) ja["start_line"] = startLine;
        if (endLine > 0) ja["end_line"] = endLine;
        std::string args = ja.dump();
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
    // One turn per agent at a time: the session is not reentrant, and a driver
    // may have several concurrent request_agent groups pointing at agents.
    std::lock_guard<std::mutex> turnLock(turnMutex_);
    busy_.store(true, std::memory_order_relaxed);
    BusyFlag busyGuard{&busy_};
    session_.addMessage(Message::User(message));
    std::string narr;   // last assistant narration text (used if the verb tool never fires)
    bool nudged = false;   // "call the tool" hint is appended at most once per turn
    bool budgetNudged = false;
    for (int r = 0; r < maxToolIters_; ++r) {
        // Budget backpressure (opt-in, see submit_reserve): when the turn is
        // close to running out of tool calls, say so once. Soft on purpose --
        // forcing tool_choice here would kill the thinking chain.
        if (!budgetNudged && submitReserve_ > 0 && r >= maxToolIters_ - submitReserve_) {
            budgetNudged = true;
            session_.addMessage(Message::User(
                "Tool budget nearly exhausted (" + std::to_string(maxToolIters_ - r) +
                " call(s) left of " + std::to_string(maxToolIters_) + "). Stop exploring "
                "and call the '" + verb + "' tool now with what you have."));
        }
        std::string text;
        std::vector<ToolCall> calls = requestTool(verb, force, text);
        if (!text.empty()) { narr = text; setNarration(text); }
        if (calls.empty()) {
            // auto mode: the model may answer in prose without ever calling the
            // submission tool. Nudge once so the turn still produces a result.
            if (!nudged && lastError_.empty() && !text.empty() && r < maxToolIters_ - 1) {
                nudged = true;
                session_.addMessage(Message::Assistant(text));
                session_.addMessage(Message::User(
                    "Please call the '" + verb + "' tool to submit your result."));
                continue;
            }
            std::string out;
            if (!lastError_.empty()) out = "[error] " + lastError_;
            else if (!text.empty()) out = text;
            // Contract: a turn always returns something a caller can reason
            // about. Silence used to propagate as "", which every caller then
            // swallowed (empty slot -> no event -> "the batch never existed").
            else out = "[no report] '" + name_ +
                       "' returned no text and no tool call";
            lastIters_ = r + 1;
            lastMarker_ = out.rfind("[no report]", 0) == 0;
            session_.addMessage(Message::Assistant(out));
            setNarration(out);
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
        // request_agent calls are grouped by target: different targets run
        // concurrently, the same target runs in order, and every result is
        // written back in the original call order.
        std::vector<std::string> sideResults(calls.size());
        std::vector<int> serialIdx;
        std::map<std::string, std::vector<int>> groups;
        for (int i = 0; i < static_cast<int>(calls.size()); ++i) {
            if (i == verbIdx) continue;
            if (calls[i].name != "request_agent") { serialIdx.push_back(i); continue; }
            std::string target;
            try { target = nlohmann::json::parse(calls[i].arguments).value("agent_id", std::string("")); }
            catch (...) {}
            if (target.empty()) serialIdx.push_back(i);   // malformed -> answer in order
            else groups[target].push_back(i);
        }

        for (int i : serialIdx) {
            try {
                sideResults[i] = dispatchSide(calls[i]);
            } catch (const std::exception& e) {
                sideResults[i] = std::string("[error] ") + e.what();
            } catch (...) {
                sideResults[i] = "[error] unknown exception in tool '" + calls[i].name + "'";
            }
        }

        if (!groups.empty()) {
            Semaphore slots(maxParallel_);
            std::vector<std::thread> workers;
            workers.reserve(groups.size());
            for (auto& kv : groups) {
                slots.acquire();
                const std::vector<int> idxs = kv.second;
                workers.emplace_back([this, &calls, &sideResults, idxs, &slots]() {
                    try {
                        for (int idx : idxs) {
                            if (tools_ && tools_->isCancelled()) {
                                sideResults[idx] = "[cancelled]";
                                continue;
                            }
                            sideResults[idx] = dispatchSide(calls[idx]);
                        }
                    } catch (const std::exception& e) {
                        for (int idx : idxs)
                            if (sideResults[idx].empty())
                                sideResults[idx] = std::string("[error] ") + e.what();
                    } catch (...) {
                        for (int idx : idxs)
                            if (sideResults[idx].empty())
                                sideResults[idx] = "[error] unknown exception in dispatch";
                    }
                    slots.release();
                });
            }
            for (auto& t : workers) t.join();
        }

        for (int i = 0; i < static_cast<int>(calls.size()); ++i) {
            if (i == verbIdx) continue;
            const std::string& r = sideResults[i].empty() ? std::string("Error: no result") : sideResults[i];
            session_.addMessage(Message::Tool(calls[i].id, calls[i].name, r));
        }
        if (finalize) {
            lastIters_ = r + 1;
            lastMarker_ = false;
            return calls[verbIdx].arguments;
        }
    }
    lastIters_ = maxToolIters_;
    lastMarker_ = narr.empty();
    if (!narr.empty()) return narr;   // loop exhausted: surface the narration
    return "[no report] '" + name_ + "' ran out of tool budget (" +
           std::to_string(maxToolIters_) + " calls) without calling '" + verb + "'";
}
