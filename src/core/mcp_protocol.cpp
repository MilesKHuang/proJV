#include "core/mcp_protocol.h"

#include <algorithm>

namespace mcp {

bool isKnownProtocolVersion(const std::string& v) {
    const auto& all = kKnownProtocolVersions();
    return std::find(all.begin(), all.end(), v) != all.end();
}

bool isBlankLine(const std::string& line) {
    for (char c : line)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    return true;
}

Message parseLine(const std::string& line) {
    Message m;
    m.raw = line;
    if (line.empty()) {
        m.why = "empty line";
        return m;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const std::exception& e) {
        m.why = std::string("not JSON: ") + e.what();
        return m;
    }
    if (!j.is_object()) {
        m.why = "not a JSON object";
        return m;
    }

    // id may be a number or a string per JSON-RPC; MCP uses numbers. Anything
    // unparseable is left at -1 and will simply never match a pending request.
    if (j.contains("id")) {
        const auto& id = j["id"];
        if (id.is_number_integer()) {
            m.id = id.get<long long>();
        } else if (id.is_string()) {
            try {
                m.id = std::stoll(id.get<std::string>());
            } catch (...) {
                m.id = -1;
            }
        }
    }

    if (j.contains("method") && j["method"].is_string()) {
        m.method = j["method"].get<std::string>();
        // A server that writes a *request* on stdout is a violation in modern
        // mode and simply unsupported in legacy mode (we declare capabilities
        // {}). We classify it so the caller can name it in the error text
        // instead of logging a useless "did not answer".
        if (j.contains("id"))
            m.isRequest = true;
        else
            m.isNotification = true;
        m.parseOk = true;
        return m;
    }

    if (j.contains("result") || j.contains("error")) {
        m.isResponse = true;
        if (j.contains("result")) m.result = j["result"];
        if (j.contains("error")) m.error = j["error"];
        m.parseOk = true;
        return m;
    }

    m.why = "neither a request, a notification nor a response";
    return m;
}

std::string makeRequest(long long id, const std::string& method, nlohmann::json params,
                        bool stateless, const std::string& clientVersion) {
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = method;

    if (stateless) {
        // The three keys are mandatory in modern mode: a server answers -32602
        // Invalid params when protocolVersion or clientCapabilities is missing.
        if (!params.is_object()) params = nlohmann::json::object();
        params["_meta"] = {
            {"io.modelcontextprotocol/protocolVersion", kModernProtocolVersion},
            {"io.modelcontextprotocol/clientCapabilities", nlohmann::json::object()},
            {"io.modelcontextprotocol/clientInfo",
             {{"name", "proJV"}, {"version", clientVersion}}}};
    }
    if (!params.is_null()) req["params"] = std::move(params);

    // dump() never emits a raw newline, so a request is always a single line.
    return req.dump();
}

std::string makeNotification(const std::string& method, nlohmann::json params) {
    nlohmann::json n;
    n["jsonrpc"] = "2.0";
    n["method"] = method;
    if (!params.is_null()) n["params"] = std::move(params);
    return n.dump();
}

bool isVersionRejection(const nlohmann::json& error) {
    if (!error.is_object()) return false;
    if (error.contains("code") && error["code"].is_number_integer() &&
        error["code"].get<int>() == -32602) {
        return true; // Invalid params: the usual symptom of a missing _meta block
    }
    if (error.contains("data") && error["data"].is_object()) {
        const auto& d = error["data"];
        if (d.contains("name") && d["name"].is_string() &&
            d["name"].get<std::string>().find("UnsupportedProtocolVersion") != std::string::npos)
            return true;
        if (d.contains("type") && d["type"].is_string() &&
            d["type"].get<std::string>().find("UnsupportedProtocolVersion") != std::string::npos)
            return true;
    }
    if (error.contains("message") && error["message"].is_string()) {
        const std::string msg = error["message"].get<std::string>();
        if (msg.find("UnsupportedProtocolVersion") != std::string::npos) return true;
        if (msg.find("protocolVersion") != std::string::npos) return true;
    }
    return false;
}

bool aggregateToolResult(const nlohmann::json& result, std::string& text, std::string& err) {
    text.clear();
    err.clear();
    if (!result.is_object()) {
        // A bare value is legal JSON-RPC but not a tools/call result; treat it
        // as text rather than failing the call.
        text = result.is_null() ? std::string() : result.dump();
        return true;
    }

    // resultType is mandatory in modern mode; a legacy server omits it and must
    // be read as "complete" (backward compatibility).
    if (result.contains("resultType") && result["resultType"].is_string()) {
        const std::string rt = result["resultType"].get<std::string>();
        if (rt == "input_required") {
            err = "server requires additional input (MRTR) -- not supported in this version";
            return false;
        }
    }

    std::vector<std::string> parts;
    if (result.contains("content") && result["content"].is_array()) {
        for (const auto& block : result["content"]) {
            if (!block.is_object()) continue;
            const std::string type = block.value("type", std::string());
            if (type == "text") {
                parts.push_back(block.value("text", std::string()));
            } else if (type == "image") {
                parts.push_back("[image: " + block.value("mimeType", std::string("unknown")) + "]");
            } else if (type == "audio") {
                parts.push_back("[audio: " + block.value("mimeType", std::string("unknown")) + "]");
            } else if (type == "resource") {
                parts.push_back("[resource]");
            } else if (type == "resource_link") {
                parts.push_back("[resource_link: " + block.value("uri", std::string()) + "]");
            } else if (!type.empty()) {
                parts.push_back("[" + type + "]");
            }
        }
    }

    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) text += '\n';
        text += parts[i];
    }

    // structuredContent is any JSON value (not necessarily an object). It is
    // appended only when there is no text block, so a server that sends both
    // does not duplicate its own answer.
    if (text.empty() && result.contains("structuredContent") &&
        !result["structuredContent"].is_null()) {
        text = result["structuredContent"].dump();
    }
    if (text.empty() && result.contains("isError") && result["isError"].is_boolean() &&
        result["isError"].get<bool>()) {
        text = "(server reported an error with no message)";
    }
    return true;
}

} // namespace mcp
