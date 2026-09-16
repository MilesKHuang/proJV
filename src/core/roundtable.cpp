#include "roundtable.h"
#include "core/prompts.h"
#include "debug_log.h"
#include "json.hpp"

#include <set>
#include <thread>
#include <utility>

namespace {

// Pull a string-array field out of a JSON arguments string.
std::vector<std::string> stringArrayField(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array()) {
        for (const auto& v : j[key]) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// BigbangParticipant
// ---------------------------------------------------------------------------

BigbangParticipant::BigbangParticipant(std::string name, std::string systemPrompt, bool canWriteDoc)
    : name_(std::move(name)), systemPrompt_(std::move(systemPrompt)), canWriteDoc_(canWriteDoc) {
    session_.addMessage(Message::System(systemPrompt_));
}

void BigbangParticipant::configure(const AppConfig& cfg, ToolRegistry* tools, int maxFileRounds) {
    cfg_ = cfg;
    tools_ = tools;
    maxFileRounds_ = maxFileRounds;
    client_.setConfig(cfg);
}

std::vector<ToolDefinition> BigbangParticipant::toolDefs() const {
    std::vector<ToolDefinition> defs;
    {
        ToolDefinition d;
        d.name = "bigbang_turn";
        d.description = "本轮发言。给出你的叙述/方案；如需先看代码，在 file_requests 列出文件路径。";
        d.parameters = {
            {"statement", "string", "本轮叙述或方案", true},
            {"file_requests", "array", "最多 3 个文件路径，仅在需要先看代码时填写", false}
        };
        defs.push_back(d);
    }
    {
        ToolDefinition d;
        d.name = "bigbang_vote";
        d.description = "对当前方案投票。仅在投票阶段调用。";
        d.parameters = {
            {"agree", "boolean", "是否同意当前方案", true},
            {"agreed_points", "array", "认同的部分，字符串列表", false},
            {"concerns", "array", "担心点列表，每条格式 '[high|medium|low] 具体问题描述'", false},
            {"suggested_tweak", "string", "要改的话建议怎么改；同意填 'none'", true}
        };
        defs.push_back(d);
    }
    if (canWriteDoc_) {
        ToolDefinition d;
        d.name = "write_bigbang_doc";
        d.description = "投票收敛后产出最终的结构化执行文档。仅供 Leonard 在 Phase 5 调用。";
        d.parameters = {
            {"doc_markdown", "string", "完整的执行文档 Markdown 内容", true}
        };
        defs.push_back(d);
    }
    return defs;
}

ToolCall BigbangParticipant::doRequest(const std::string& toolName, bool force,
                                       std::string& outText) {
    ChatRequest req;
    req.model = cfg_.model;
    req.maxTokens = cfg_.maxTokens;
    req.temperature = cfg_.temperature;
    req.stream = true;
    // Thinking-mode models (DeepSeek v4) reject a forced function tool_choice;
    // force=false falls back to "auto" so the model still picks the tool itself.
    req.toolChoice = force ? toolName : std::string("auto");
    req.tools = toolDefs();
    req.messages = session_.getContextMessages();

    std::vector<ToolCall> parts;
    bool got = false;
    outText.clear();
    lastReasoning_.clear();
    lastError_.clear();

    StreamCallbacks cb;
    cb.onText = [&outText](const std::string& t) { outText += t; };
    // Thinking-mode models require reasoning_content to be echoed back on the
    // next request; capture it so the assistant tool-call message can carry it.
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
        debugLogf("[Bigbang][%s] '%s' failed: %s",
                  name_.c_str(), toolName.c_str(), lastError_.c_str());
    }

    if (!got || parts.empty()) {
        ToolCall empty;
        empty.valid = false;
        return empty;
    }
    for (auto& p : parts) {
        if (p.name == toolName) return p;
    }
    return parts[0];
}

ToolCall BigbangParticipant::requestTool(const std::string& toolName, std::string& outText) {
    outText.clear();
    ToolCall tc = doRequest(toolName, /*force=*/true, outText);
    if (!tc.valid && lastError_.find("tool_choice") != std::string::npos) {
        debugLogf("[Bigbang][%s] forced tool_choice rejected, retrying with auto",
                  name_.c_str());
        tc = doRequest(toolName, /*force=*/false, outText);
    }
    return tc;
}

std::string BigbangParticipant::executeFileRequests(const std::vector<std::string>& paths) {
    std::string out;
    std::set<std::string> seen;
    int n = 0;
    for (const auto& p : paths) {
        if (p.empty() || seen.count(p)) continue;
        if (n >= 3) { out += "\n[bigbang] file_requests truncated to 3 per turn.\n"; break; }
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

std::string BigbangParticipant::turn(const std::string& userMessage) {
    session_.addMessage(Message::User(userMessage));
    std::string statement;
    for (int round = 0; round <= maxFileRounds_; ++round) {
        std::string text;
        ToolCall tc = requestTool("bigbang_turn", text);
        if (!tc.valid) {
            if (!lastError_.empty()) {
                std::string err = "[bigbang error] " + lastError_;
                session_.addMessage(Message::Assistant(err));
                return err;
            }
            if (!text.empty()) {
                session_.addMessage(Message::Assistant(text));
                return text;
            }
            return "(no statement)";
        }

        std::vector<std::string> files;
        std::string stmt;
        try {
            auto args = nlohmann::json::parse(tc.arguments);
            if (args.contains("statement") && args["statement"].is_string())
                stmt = args["statement"].get<std::string>();
            files = stringArrayField(args, "file_requests");
        } catch (...) {}
        if (!stmt.empty()) statement = stmt;

        Message am = Message::Assistant();
        am.toolCalls.push_back(tc);
        am.reasoningContent = lastReasoning_;
        session_.addMessage(am);

        bool finalize = (round >= maxFileRounds_) || files.empty();
        if (finalize) {
            session_.addMessage(Message::Tool(tc.id, "bigbang_turn", "OK"));
            if (statement.empty()) statement = text.empty() ? "(no statement)" : text;
            return statement;
        }
        session_.addMessage(Message::Tool(tc.id, "bigbang_turn", executeFileRequests(files)));
    }
    return statement;
}

VoteResult BigbangParticipant::vote(const std::string& userMessage) {
    session_.addMessage(Message::User(userMessage));
    std::string text;
    ToolCall tc = requestTool("bigbang_vote", text);

    VoteResult vr;
    if (!tc.valid) {
        vr.agree = false;
        vr.suggestedTweak = "none";
        if (!lastError_.empty())
            vr.concerns.push_back("[high] bigbang error: " + lastError_);
        else
            vr.concerns.push_back("[low] (no structured vote) " + text);
        return vr;
    }
    try {
        auto args = nlohmann::json::parse(tc.arguments);
        if (args.contains("agree") && args["agree"].is_boolean())
            vr.agree = args["agree"].get<bool>();
        vr.agreedPoints = stringArrayField(args, "agreed_points");
        vr.concerns = stringArrayField(args, "concerns");
        if (args.contains("suggested_tweak") && args["suggested_tweak"].is_string())
            vr.suggestedTweak = args["suggested_tweak"].get<std::string>();
    } catch (...) {}

    Message am = Message::Assistant();
    am.toolCalls.push_back(tc);
    am.reasoningContent = lastReasoning_;
    session_.addMessage(am);
    session_.addMessage(Message::Tool(tc.id, "bigbang_vote", "OK"));
    return vr;
}

std::string BigbangParticipant::writeDoc(const std::string& userMessage) {
    session_.addMessage(Message::User(userMessage));
    std::string text;
    ToolCall tc = requestTool("write_bigbang_doc", text);
    if (!tc.valid) {
        if (!lastError_.empty()) return "[bigbang error] " + lastError_;
        return text;
    }

    std::string doc;
    try {
        auto args = nlohmann::json::parse(tc.arguments);
        if (args.contains("doc_markdown") && args["doc_markdown"].is_string())
            doc = args["doc_markdown"].get<std::string>();
    } catch (...) {}

    Message am = Message::Assistant();
    am.toolCalls.push_back(tc);
    am.reasoningContent = lastReasoning_;
    session_.addMessage(am);
    session_.addMessage(Message::Tool(tc.id, "write_bigbang_doc", "OK"));
    return doc;
}

// ---------------------------------------------------------------------------
// Roundtable
// ---------------------------------------------------------------------------

Roundtable::Roundtable(AppConfig cfg, ToolRegistry* tools, Callbacks cbs)
    : cfg_(std::move(cfg)), tools_(tools), cbs_(std::move(cbs)) {
    ensureDefaultBigbangPrompts();
    auto mk = [this](const char* role, const char* display, bool canDoc) {
        auto p = std::make_unique<BigbangParticipant>(display, loadBigbangPrompt(role), canDoc);
        p->configure(cfg_, tools_, 2);
        return p;
    };
    parts_[0] = mk("sheldon", "Sheldon", false);
    parts_[1] = mk("penny", "Penny", false);
    parts_[2] = mk("leonard", "Leonard", true);
}

void Roundtable::progress(const std::string& s) {
    if (cbs_.onProgress) cbs_.onProgress(s);
}

void Roundtable::cancel() {
    cancelRequested_.store(true);
    for (auto& p : parts_) {
        if (p) p->cancel();
    }
}

std::string Roundtable::buildVoteSummary() const {
    const char* names[3] = {"Sheldon", "Penny", "Leonard"};
    std::string out;
    for (int i = 0; i < 3; ++i) {
        out += names[i];
        out += votes_[i].agree ? ": agree" : ": dissent";
        out += " (concerns: ";
        if (votes_[i].concerns.empty()) {
            out += "none";
        } else {
            for (size_t k = 0; k < votes_[i].concerns.size(); ++k) {
                if (k) out += "; ";
                out += votes_[i].concerns[k];
            }
        }
        out += "); tweak: " + votes_[i].suggestedTweak + "\n";
    }
    return out;
}

std::string Roundtable::buildResidualConcerns() const {
    const char* names[3] = {"Sheldon", "Penny", "Leonard"};
    std::string out;
    for (int i = 0; i < 3; ++i) {
        if (votes_[i].agree) continue;
        for (const auto& c : votes_[i].concerns) {
            out += "- " + std::string(names[i]) + ": " + c + "\n";
        }
    }
    if (out.empty()) out = "(none)\n";
    return out;
}

void Roundtable::run(const std::string& topic) {
    if (!modelSupportsTools(cfg_.model)) {
        std::string msg = "[bigbang] model '" + cfg_.model +
            "' does not support tool calls, so the debate cannot run. "
            "Switch to a tool-capable model in config.toml.";
        if (cbs_.onStatement) cbs_.onStatement("system", msg);
        progress(msg);
        return;
    }

    std::string voteSummary;

    for (int round = 1; round <= maxRounds_ && !cancelRequested_.load(); ++round) {
        lastRounds_ = round;
        progress("Round " + std::to_string(round) + "/" + std::to_string(maxRounds_)
                 + " - proposals (Sheldon + Penny)");

        std::string sheldonMsg, pennyMsg;
        if (round == 1) {
            sheldonMsg = "Topic: " + topic + "\n\nCall bigbang_turn with your proposal.";
            pennyMsg = sheldonMsg;
        } else {
            std::string base = "Topic: " + topic
                + "\n\nPrevious Leonard compromise:\n" + prevLeonardS_
                + "\n\nPrevious votes:\n" + voteSummary
                + "\n\nRevise your proposal based on the above, then call bigbang_turn.";
            sheldonMsg = base;
            pennyMsg = base;
        }

        std::string s1, s2;
        {
            std::thread t1([&] { s1 = parts_[0]->turn(sheldonMsg); });
            std::thread t2([&] { s2 = parts_[1]->turn(pennyMsg); });
            t1.join();
            t2.join();
        }
        if (cancelRequested_.load()) break;
        sheldonS_ = s1;
        pennyS_ = s2;
        if (cbs_.onStatement) {
            cbs_.onStatement("Sheldon", s1);
            cbs_.onStatement("Penny", s2);
        }

        progress("Round " + std::to_string(round) + "/" + std::to_string(maxRounds_)
                 + " - integration (Leonard)");
        std::string combined = "Topic: " + topic
            + "\n\nSheldon proposal:\n" + s1
            + "\n\nPenny proposal:\n" + s2;
        if (round > 1) combined += "\n\nPrevious votes:\n" + voteSummary;
        combined += "\n\nCall bigbang_turn with your compromise "
                    "(common ground / disagreements / compromise / steps).";
        leonardS_ = parts_[2]->turn(combined);
        if (cancelRequested_.load()) break;
        if (cbs_.onStatement) cbs_.onStatement("Leonard", leonardS_);

        progress("Round " + std::to_string(round) + "/" + std::to_string(maxRounds_) + " - vote");
        std::string voteMsg = "Topic: " + topic
            + "\n\nProposal under vote (Leonard compromise):\n" + leonardS_
            + "\n\nCall bigbang_vote now.";
        {
            std::thread v0([&] { votes_[0] = parts_[0]->vote(voteMsg); });
            std::thread v1([&] { votes_[1] = parts_[1]->vote(voteMsg); });
            std::thread v2([&] { votes_[2] = parts_[2]->vote(voteMsg); });
            v0.join();
            v1.join();
            v2.join();
        }
        if (cancelRequested_.load()) break;
        if (cbs_.onVote) {
            cbs_.onVote("Sheldon", votes_[0]);
            cbs_.onVote("Penny", votes_[1]);
            cbs_.onVote("Leonard", votes_[2]);
        }

        bool allAgree = votes_[0].agree && votes_[1].agree && votes_[2].agree;
        bool loopDetect = (round > 1 && !leonardS_.empty() && leonardS_ == prevLeonardS_);
        prevLeonardS_ = leonardS_;

        voteSummary = buildVoteSummary();

        if (allAgree || loopDetect) {
            converged_ = true;
            break;
        }
    }

    if (cancelRequested_.load()) {
        progress("Cancelled");
        return;
    }

    progress("Writing execution document (Leonard)");
    std::string docMsg = "Topic: " + topic
        + "\n\nFinal proposal (Leonard compromise, voted):\n" + leonardS_
        + "\n\nResidual disagreements (write any [high] items into the "
          "unresolved-disagreements section):\n" + buildResidualConcerns()
        + "\n\nCall write_bigbang_doc with the structured execution document.";
    lastDoc_ = parts_[2]->writeDoc(docMsg);

    if (!lastDoc_.empty() && cbs_.onDoc) cbs_.onDoc(lastDoc_);
    progress(converged_ ? "Done (converged)" : "Done (round cap reached)");
}
