#include "models.h"
#include "json.hpp"
#include "debug_log.h"
#include "tools/base64.h"

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

// Resolve an attachment to a data: URL. Prefers the cached form; falls back to
// re-reading the file by path (the DB persists paths, not payloads).
static std::string imageUrlFor(const ImageAttachment& img) {
    if (!img.dataUrl.empty()) return img.dataUrl;
    if (img.path.empty()) return "";
    std::string mime;
    std::string url = imgutil::makeImageDataUrl(img.path, mime);
    return url;
}

void to_json(nlohmann::json& j, const Message& msg) {
    j["role"] = sanitizeUTF8(msg.role);
    // Always emit "content" field. Assistant messages with tool_calls
    // may have empty content; set it to null for API compatibility.
    // Missing "content" key causes HTTP 400 from DeepSeek API.
    if (msg.role == "user" && !msg.images.empty()) {
        // Vision: content becomes an array of text + image_url blocks.
        // Images are ONLY legal in user messages, so every other role keeps
        // the plain-string form and silently drops the attachments.
        nlohmann::json parts = nlohmann::json::array();
        if (!msg.content.empty())
            parts.push_back({{"type", "text"}, {"text", sanitizeUTF8(msg.content)}});
        for (const auto& img : msg.images) {
            std::string url = imageUrlFor(img);
            if (url.empty()) {
                // File vanished or is unreadable: keep the turn alive and tell
                // the model instead of failing the whole request.
                parts.push_back({{"type", "text"},
                                 {"text", "[image unavailable: " + sanitizeUTF8(img.path) + "]"}});
                continue;
            }
            const std::string detail = img.detail.empty() ? "auto" : img.detail;
            parts.push_back({{"type", "image_url"},
                             {"image_url", {{"url", url}, {"detail", detail}}}});
        }
        j["content"] = parts;
    } else if (msg.role == "assistant" && !msg.toolCalls.empty() && msg.content.empty()) {
        j["content"] = nullptr;
    } else {
        j["content"] = sanitizeUTF8(msg.content);
    }
    if (!msg.toolCallId.empty())
        j["tool_call_id"] = sanitizeUTF8(msg.toolCallId);
    if (!msg.name.empty())
        j["name"] = sanitizeUTF8(msg.name);
    // V4 thinking: when the request carries `tools` (proJV always does), the API
    // rejects any assistant message that omits this field with
    //   "The `reasoning_content` in the thinking mode must be passed back to the API."
    // So it is emitted for EVERY assistant message, empty when we have no chain
    // of thought for it (skill narration, /help output, error notices). An empty
    // string is accepted; with no tools the field is ignored by the server.
    if (msg.role == "assistant")
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
    if (j.contains("content") && j["content"].is_string()) {
        msg.content = j["content"].get<std::string>();
    } else if (j.contains("content") && j["content"].is_array()) {
        // Vision round-trip: flatten text parts, keep image parts.
        for (const auto& part : j["content"]) {
            if (!part.is_object()) continue;
            const std::string type = part.value("type", std::string());
            if (type == "text") {
                msg.content += part.value("text", std::string());
            } else if (type == "image_url") {
                ImageAttachment img;
                std::string url;
                if (part.contains("image_url") && part["image_url"].is_object()) {
                    const auto& iu = part["image_url"];
                    url = iu.value("url", std::string());
                    img.detail = iu.value("detail", std::string("auto"));
                } else if (part.contains("image_url") && part["image_url"].is_string()) {
                    url = part["image_url"].get<std::string>();
                }
                if (!url.empty()) {
                    img.dataUrl = url;
                    msg.images.push_back(std::move(img));
                }
            }
        }
    }
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
