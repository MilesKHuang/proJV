// proJV -- the headless event stream (design 08).
//
// One event per line, one JSON object per line, no delta merging: a consumer that
// wants fewer updates merges them itself. This stream is the ONLY interface a
// front-end, a CI job or the `run` subcommand uses to watch a turn -- it never
// reaches into Agent or SkillRunner internals.
//
// Rules this file enforces:
//   * field names are short ("t" is the event name): a long run emits a lot of
//     these lines, and the diff between two runs is read by humans;
//   * withTimestamp=false drops every timestamp and duration, so the same prompt
//     twice produces byte-identical output (the determinism acceptance test);
//   * tool_result carries `has_diff` and a short `text` head; the unified diff
//     travels in its own `diff` event, because sending the same text twice
//     doubles the traffic on a big edit;
//   * the schema version is written into `turn_start` only, not on every line.
//
// Header-only on purpose: nlohmann::json is already vendored as a single header,
// and the runtime, the CLI and the tests all need the same serializer without a
// new translation unit.
#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace events {

// Bump when a field is renamed or removed. Additive changes keep the version.
inline constexpr const char* kSchema = "1";

enum class Kind : int {
    TurnStart = 0,
    ThinkingDelta,
    TextDelta,
    ToolCall,
    ToolResult,
    Diff,
    ApprovalRequest,
    ApprovalResult,
    Phase,
    Usage,
    AgentState,
    SkillEvent,
    Task,
    Todo,
    TurnEnd,
    Notice,
    Error,
    Count, // sentinel, never serialized
};

inline constexpr int kKindCount = static_cast<int>(Kind::Count);

// "turn_start", "text_delta", ... -- the value of "t".
inline const char* name(Kind k) {
    static const char* const kNames[] = {
        "turn_start",  "thinking_delta", "text_delta",       "tool_call",
        "tool_result", "diff",           "approval_request", "approval_result",
        "phase",       "usage",          "agent_state",      "skill_event",
        "task",        "todo",           "turn_end",         "notice",
        "error",
    };
    const int i = static_cast<int>(k);
    return (i >= 0 && i < kKindCount) ? kNames[i] : "unknown";
}

// One event. A field that does not apply to a kind stays empty / false / 0 and is
// omitted from the line, which keeps the stream compact and diffable.
struct Event {
    Kind kind = Kind::Notice;
    int seq = 0; // per-run counter, so ids are reproducible (never random)

    // turn_start
    std::string turn;    // "1", "2", ... one per user submission
    std::string model;   // model id the request used
    std::string session; // session db file name, or a per-process fallback

    // identity
    std::string id;   // tool call id / approval id / task id
    std::string name; // tool name / server name / agent id
    std::string role; // skill role display name
    std::string sub;  // skill_event kind: message | vote | document | system

    // payload
    std::string text;     // delta text / narration / notice / error message
    std::string path;     // diff target file
    std::string cmd;      // approval command / background task command
    std::string mode;     // approval mode (strict / balanced / loose)
    std::string phase;    // agent phase name
    std::string activity; // agent_state: idle | working | error
    std::string reason;   // turn_end: why the turn stopped

    // flags
    bool ok = false;
    bool hasDiff = false;
    bool active = false;

    // numbers
    int prompt = 0;       // usage: prompt tokens
    int completion = 0;   // usage: completion tokens
    int exitCode = 0;     // task: exit code
    bool hasExit = false; // task reports exit 0 as meaningful, so it needs its own flag
    int bytes = 0;        // task: output size
    int pending = 0;      // todo: number of open items

    std::vector<std::string> items; // todo briefs / overview, one entry per line

    int64_t ts = 0; // epoch milliseconds; written only when withTimestamp

    Event() = default;
    Event(Kind k, int s = 0) : kind(k), seq(s) {}
};

// Serialize one event as one line (trailing '\n' included). Keys are sorted by
// nlohmann::json, so the output is deterministic without any extra bookkeeping.
inline std::string toLine(const Event& e, bool withTimestamp) {
    nlohmann::json j;
    j["t"] = name(e.kind);
    if (e.seq > 0) j["seq"] = e.seq;
    if (!e.turn.empty()) j["turn"] = e.turn;
    if (!e.model.empty()) j["model"] = e.model;
    if (!e.session.empty()) j["session"] = e.session;
    if (e.kind == Kind::TurnStart) j["schema"] = kSchema;

    if (!e.id.empty()) j["id"] = e.id;
    if (!e.name.empty()) j["name"] = e.name;
    if (!e.role.empty()) j["role"] = e.role;
    if (!e.sub.empty()) j["sub"] = e.sub;

    if (!e.text.empty()) j["text"] = e.text;
    if (!e.path.empty()) j["path"] = e.path;
    if (!e.cmd.empty()) j["cmd"] = e.cmd;
    if (!e.mode.empty()) j["mode"] = e.mode;
    if (!e.phase.empty()) j["phase"] = e.phase;
    if (!e.activity.empty()) j["activity"] = e.activity;
    if (!e.reason.empty()) j["reason"] = e.reason;

    if (e.ok) j["ok"] = true;
    if (e.hasDiff) j["has_diff"] = true;
    if (e.active) j["active"] = true;

    if (e.prompt > 0) j["prompt"] = e.prompt;
    if (e.completion > 0) j["completion"] = e.completion;
    if (e.hasExit) j["exit"] = e.exitCode;
    if (e.bytes > 0) j["bytes"] = e.bytes;
    if (e.pending > 0) j["pending"] = e.pending;
    if (!e.items.empty()) j["items"] = e.items;

    if (withTimestamp && e.ts > 0) j["ts"] = e.ts;

    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

// ---- schema description (`run --schema`) --------------------------------

struct KindDoc {
    const char* kind;
    const char* fields;
};

inline const std::vector<KindDoc>& schemaTable() {
    static const std::vector<KindDoc> table = {
        {"turn_start", "turn, model, session, schema"},
        {"thinking_delta", "seq, text"},
        {"text_delta", "seq, text"},
        {"tool_call", "seq, id, name, text(args)"},
        {"tool_result", "seq, id, name, ok, has_diff"},
        {"diff", "seq, id, path, text"},
        {"approval_request", "seq, id, name, cmd, mode"},
        {"approval_result", "seq, id, ok"},
        {"phase", "seq, phase, text"},
        {"usage", "seq, prompt, completion"},
        {"agent_state", "seq, name, role, activity, text, items"},
        {"skill_event", "seq, role, sub, text"},
        {"task", "seq, id, activity, exit, bytes, cmd"},
        {"todo", "seq, pending, items"},
        {"turn_end", "seq, reason"},
        {"notice", "seq, text"},
        {"error", "seq, text"},
    };
    return table;
}

inline std::string schemaText() {
    std::string s = "proJV event stream schema ";
    s += kSchema;
    s += "\none JSON object per line; \"t\" is the event name; empty fields are omitted.\n"
         "timestamps (\"ts\") appear only without --no-ts.\n\n";
    for (const auto& d : schemaTable()) {
        s += "  ";
        s += d.kind;
        const size_t pad = 24;
        const size_t len = std::string(d.kind).size();
        if (len < pad) s.append(pad - len, ' ');
        s += d.fields;
        s += "\n";
    }
    return s;
}

} // namespace events
