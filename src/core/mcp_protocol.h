// proJV -- MCP client protocol layer (JSON-RPC 2.0 over newline-framed stdio).
//
// Two eras are supported:
//   legacy  -- "initialize" handshake, then notifications/initialized. The
//              protocolVersion we offer is 2025-11-25; a server may answer with
//              an older version it knows, and that is fine as long as we know it
//              too (kKnownProtocolVersions).
//   modern  -- no handshake at all: every request carries the three _meta keys,
//              and every result carries resultType (complete / input_required).
//
// Framing is newline-delimited JSON, NOT Content-Length. nlohmann::json::dump()
// never emits a raw newline, so outbound framing is safe by construction;
// inbound lines are capped at kMaxLineBytes and a longer line is a violation.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace mcp {

// 1 MiB. A single stdout line beyond this is treated as a protocol violation
// rather than something to buffer forever.
constexpr size_t kMaxLineBytes = 1024 * 1024;

// Protocol versions this client understands. Offered value in legacy mode is
// kOfferedProtocolVersion; a server answering with any of the others is still
// accepted, because tools/list and tools/call are wire-compatible across them.
inline const char* kOfferedProtocolVersion = "2025-11-25";
inline const char* kModernProtocolVersion = "2026-07-28";

inline const std::vector<std::string>& kKnownProtocolVersions() {
    static const std::vector<std::string> v = {"2024-11-05", "2025-03-26", "2025-06-18",
                                              "2025-11-25", "2026-07-28"};
    return v;
}

bool isKnownProtocolVersion(const std::string& v);

// One parsed message from the server's stdout. The three shapes are mutually
// exclusive and are decided by which fields exist.
struct Message {
    bool parseOk = false;       // a JSON object we could classify
    bool isResponse = false;    // has "id" and ("result" or "error")
    bool isRequest = false;     // has "method" and "id": server asking us
    bool isNotification = false; // has "method" but no "id"
    long long id = -1;
    std::string method;
    nlohmann::json result;
    nlohmann::json error;
    std::string raw;
    std::string why; // when !parseOk: what was wrong (for the log line)
};

// Parse one line of stdout. Never throws.
// Not-JSON lines are exactly what a chatty server writes when it logs to stdout
// instead of stderr; the caller skips those and keeps going (hard condition C3).
Message parseLine(const std::string& line);

// True when the line is only whitespace (CR included: some servers emit \r\n).
bool isBlankLine(const std::string& line);

// Build one request line without the trailing newline.
//   stateless -- inject the three _meta keys into params (modern mode)
//   params    -- must be an object, or null
std::string makeRequest(long long id, const std::string& method, nlohmann::json params,
                        bool stateless, const std::string& clientVersion);

// Build one notification line without the trailing newline.
std::string makeNotification(const std::string& method, nlohmann::json params);

// Aggregate a tools/call result into the single string the Agent shows the model.
//
// Returns false when the server asked for more input (resultType ==
// "input_required", the modern MRTR path this version does not implement); in
// that case `err` explains it and the caller must surface an Error: result.
//
// Otherwise `text` holds every content[].text joined by '\n', plus a placeholder
// line for each non-text block (an image cannot be shown in a terminal). A
// business failure (isError == true) is NOT an error for us: the text comes back
// as-is so the model can read what went wrong and try something else.
bool aggregateToolResult(const nlohmann::json& result, std::string& text, std::string& err);

// True when the JSON-RPC "error" member is a protocol-version rejection from a
// modern server (UnsupportedProtocolVersionError) or a plain -32602 caused by a
// missing _meta block. Such a server must not be retried.
bool isVersionRejection(const nlohmann::json& error);

} // namespace mcp
