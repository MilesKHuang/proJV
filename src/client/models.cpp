#include "models.h"
#include "json.hpp"
#include "debug_log.h"

// --- ToolDefinition ------------------------------------------------
std::string ToolDefinition::toJsonSchema() const {
    try {
        nlohmann::json schema;
        schema["type"] = "object";

        if (!parameters.empty()) {
            nlohmann::json props;
            nlohmann::json required = nlohmann::json::array();

            for (const auto& p : parameters) {
                nlohmann::json prop;
                prop["type"] = sanitizeUTF8(p.type);
                prop["description"] = sanitizeUTF8(p.description);
                props[sanitizeUTF8(p.name)] = prop;

                if (p.required) {
                    required.push_back(p.name);
                }
            }

            schema["properties"] = props;
            if (!required.empty()) {
                schema["required"] = required;
            }
        }

        return schema.dump(4);
    }
    catch (const std::exception& e) {
        debugLogf("[Models] toJsonSchema exception: %s", e.what());
        return "{\"type\": \"object\"}";
    }
}

// --- Message static constructors -----------------------------------
Message Message::System(std::string content) {
    Message m;
    m.role = "system";
    m.content = std::move(content);
    return m;
}

Message Message::User(std::string content) {
    Message m;
    m.role = "user";
    m.content = std::move(content);
    return m;
}

Message Message::Assistant(std::string content) {
    Message m;
    m.role = "assistant";
    m.content = std::move(content);
    return m;
}

Message Message::Tool(std::string toolCallId, std::string name, std::string content, bool isError) {
    Message m;
    m.role = "tool";
    m.toolCallId = std::move(toolCallId);
    m.name = std::move(name);
    m.content = std::move(content);
    if (isError) {
        m.content = "[Error] " + m.content;
    }
    return m;
}

// --- nlohmann::json conversions -------------------------------------

void to_json(nlohmann::json& j, const ToolCall& tc) {
    j = nlohmann::json{
        {"id", sanitizeUTF8(tc.id)},
        {"type", "function"},
        {"function", {
            {"name", sanitizeUTF8(tc.name)},
            {"arguments", sanitizeUTF8(tc.arguments)}
        }}
    };
}

void from_json(const nlohmann::json& j, ToolCall& tc) {
    if (j.contains("id") && j["id"].is_string())
        tc.id = j["id"].get<std::string>();
    if (j.contains("function") && j["function"].is_object()) {
        const auto& fn = j["function"];
        if (fn.contains("name") && fn["name"].is_string())
            tc.name = fn["name"].get<std::string>();
        if (fn.contains("arguments") && fn["arguments"].is_string())
            tc.arguments = fn["arguments"].get<std::string>();
    }
    // index is not serialized; it is positional in the array
    tc.valid = true;
    tc.parseError.clear();
}

void to_json(nlohmann::json& j, const Message& msg) {
    j["role"] = sanitizeUTF8(msg.role);
    if (!msg.content.empty())
        j["content"] = sanitizeUTF8(msg.content);
    if (!msg.toolCallId.empty())
        j["tool_call_id"] = sanitizeUTF8(msg.toolCallId);
    if (!msg.name.empty())
        j["name"] = sanitizeUTF8(msg.name);
    // V4 reasoning_content: only send for assistant messages with tool_calls.
    if (!msg.reasoningContent.empty() && !msg.toolCalls.empty())
        j["reasoning_content"] = sanitizeUTF8(msg.reasoningContent);
    if (!msg.toolCalls.empty()) {
        nlohmann::json tcs = nlohmann::json::array();
        for (const auto& tc : msg.toolCalls)
            tcs.push_back(tc);
        j["tool_calls"] = tcs;
    }
}

void from_json(const nlohmann::json& j, Message& msg) {
    if (j.contains("role") && j["role"].is_string())
        msg.role = j["role"].get<std::string>();
    if (j.contains("content") && j["content"].is_string())
        msg.content = j["content"].get<std::string>();
    if (j.contains("tool_call_id") && j["tool_call_id"].is_string())
        msg.toolCallId = j["tool_call_id"].get<std::string>();
    if (j.contains("name") && j["name"].is_string())
        msg.name = j["name"].get<std::string>();
    if (j.contains("reasoning_content") && j["reasoning_content"].is_string())
        msg.reasoningContent = j["reasoning_content"].get<std::string>();
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& tcItem : j["tool_calls"]) {
            ToolCall tc = tcItem.get<ToolCall>();
            if (!tc.name.empty() || !tc.id.empty())
                msg.toolCalls.push_back(tc);
        }
    }
}

void to_json(nlohmann::json& j, const ToolDefinition& td) {
    j["type"] = "function";
    auto& func = j["function"];
    func["name"] = sanitizeUTF8(td.name);
    func["description"] = sanitizeUTF8(td.description);
    try {
        std::string schemaStr = td.toJsonSchema();
        func["parameters"] = nlohmann::json::parse(schemaStr);
    }
    catch (const nlohmann::json::parse_error& e) {
        debugLogf("[Models] to_json(ToolDefinition) schema parse error for '%s': %s",
            td.name.c_str(), e.what());
        func["parameters"] = nlohmann::json{{"type", "object"}};
    }
}