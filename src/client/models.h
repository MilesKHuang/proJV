#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <optional>
#include <chrono>

// --- Tool call / tool result ----------------------------------------

struct ToolParameter {
    std::string name;
    std::string type;      // "string", "number", "boolean", "array"
    std::string description;
    bool required = false;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<ToolParameter> parameters;

    // Serialize to JSON for the API request
    std::string toJsonSchema() const;
};

struct ToolCall {
    std::string id;       // e.g. "call_abc123"
    std::string name;
    std::string arguments; // JSON string of arguments
    int index = 0;        // position in tool_calls array (for parallel calls)

    bool valid = true;
    std::string parseError;
};

struct ToolResult {
    std::string toolCallId;
    std::string name;
    std::string content;  // result text
    bool isError = false;
    bool isCancelled = false;
};

// --- Images (vision input) ------------------------------------------

// One image attached to a user message.
//   path   -- filesystem path as resolved by the "@path" parser (persisted)
//   detail -- OpenAI-compatible detail level; Phase 1 always "auto"
//   mime   -- container format detected from the magic number ("image/png" ...)
//   dataUrl-- "data:<mime>;base64,..." cache. Not persisted: DB rows keep only
//             the path, and to_json() re-reads/re-encodes when this is empty.
struct ImageAttachment {
    std::string path;
    std::string detail = "auto";
    std::string mime;
    std::string dataUrl;
};

// --- Message --------------------------------------------------------

struct Message {
    int64_t id = 0;         // database row id (0 = not persisted yet)
    std::string role;       // "system", "user", "assistant", "tool"
    std::string content;    // text content (empty if tool_calls present)
    std::string reasoningContent;    // DeepSeek reasoning_content (V4 thinking)
    std::vector<ToolCall> toolCalls;   // for assistant messages
    std::string toolCallId; // for tool result messages
    std::string name;       // for tool result messages
    // Vision attachments. Only user messages may carry images; to_json()
    // drops them for every other role (the API returns 400 otherwise).
    std::vector<ImageAttachment> images;

    // Constructor helpers
    static Message System(std::string content);
    static Message User(std::string content);
    static Message Assistant(std::string content = "");
    static Message Tool(std::string toolCallId, std::string name, std::string content, bool isError = false);
};

// --- API types ------------------------------------------------------

struct ChatRequest {
    std::string model = "deepseek-v4-flash";
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    // Force the model to call a specific function tool (empty = API default).
    // Serialized as {"type":"function","function":{"name":<toolChoice>}}.
    std::string toolChoice;
    bool stream = true;
    int maxTokens = 65536;
    double temperature = 0.0;
    // Thinking mode. true (default) == do not send the field, which is what the
    // API defaults to; false sends {"thinking":{"type":"disabled"}}. Thinking
    // mode rejects a forced function tool_choice, so SubAgent disables it
    // whenever a verb tool is forced.
    bool thinking = true;
};

struct ChatResponse {
    std::string id;
    std::string model;
    std::vector<Message> messages;
    int promptTokens = 0;
    int completionTokens = 0;
};

struct StreamChunk {
    std::string deltaContent;
    std::vector<ToolCall> deltaToolCalls;
    bool finishReason = false;
    std::string finishReasonType;
    int index = 0;
};

// --- nlohmann::json conversions for ADL -----------------------------
#include "json.hpp"

void to_json(nlohmann::json& j, const ToolCall& tc);
void from_json(const nlohmann::json& j, ToolCall& tc);
void to_json(nlohmann::json& j, const Message& msg);
void from_json(const nlohmann::json& j, Message& msg);
void to_json(nlohmann::json& j, const ToolDefinition& td);

// --- Tool execution -------------------------------------------------

enum class ToolApproval { Pending, Approved, Rejected };

struct PendingToolCall {
    ToolCall call;
    ToolApproval approval = ToolApproval::Pending;
};

// --- Config ---------------------------------------------------------

// --- Model pricing (configurable via config.toml) -----------------
struct ModelPriceEntry {
    std::string prefix;         // model ID prefix to match (e.g. "deepseek-v4-pro")
    double inputPrice = 0.5;    // $ per 1M input tokens
    double outputPrice = 2.0;   // $ per 1M output tokens
};

// Look up price by matching modelId against registered prefixes.
// Returns fallback prices if no match found.
inline double lookupModelPrice(const std::vector<ModelPriceEntry>& prices,
                                const std::string& modelId, bool input) {
    for (const auto& p : prices) {
        if (modelId.find(p.prefix) == 0)
            return input ? p.inputPrice : p.outputPrice;
    }
    return input ? 0.5 : 2.0;  // fallback: cheapest
}

// Whether a model supports tool calls (model-specific knowledge)
inline bool modelSupportsTools(const std::string& modelId) {
    return modelId.find("reasoner") == std::string::npos;
}

// Whether a model accepts image content blocks. DeepSeek serves vision on the
// Flash line ("deepseek-flash", legacy "deepseek-v4-flash-vision-exp"); the
// "vision" tag is accepted too so experimental names keep working.
inline bool modelSupportsVision(const std::string& modelId) {
    if (modelId.find("flash") != std::string::npos) return true;
    if (modelId.find("vision") != std::string::npos) return true;
    return false;
}

struct AppConfig {
    std::string apiKey;
    std::string model = "deepseek-v4-flash";
    std::string baseUrl = "https://api.deepseek.com";
    int maxTokens = 65536;
    double temperature = 0.0;
    std::string configPath;
    std::string workspacePath;   // file access whitelist; empty = exe dir
    std::string cppCompilerPath; // C++ compiler path (e.g. MSBuild.exe, cl.exe)
    std::string pythonPath;      // Python interpreter path
    std::string clangFormatPath; // clang-format binary for the format_file tool (empty = PATH)
    // Model pricing: map of prefix -> {input_price, output_price}
    // Parsed from [model_prices] section in config.toml
    std::vector<ModelPriceEntry> modelPrices;

    // Context window: 0 = auto-detect from model name, >0 = explicit override
    // Set via config.toml "context_window" key. Default 0 enables auto-detection
    // from the model->window mapping in contextWindowForModel().
    size_t contextWindow = 0;

    // Theme name: empty = auto-detect from projv_theme/ (first found).
    // Otherwise, switch to this theme on startup.  Saved to config.toml [theme].
    std::string themeName;

    bool loaded = false;
    std::string loadError;

    bool load(const std::string& path = "");
    bool save();
};

// -- Model -> context window mapping (used when config.contextWindow == 0) --
// Returns the known context-window size in tokens for supported models.
// Returns 0 for unknown models -- callers should fall back to a safe default (64K).
inline size_t contextWindowForModel(const std::string& model) {
    (void)model;
    // DeepSeek models are effectively 1M context. config.toml
    // "context_window" still allows an explicit override.
    return 1'048'576;
}

// --- Model info (fetched from API + pricing) ----------------------
struct ModelInfo {
    std::string id;         // e.g. "deepseek-chat"
    std::string label;      // display name (same as id when simplified)
    double inputPricePerM = 0.0;   // $ per 1M input tokens
    double outputPricePerM = 0.0;  // $ per 1M output tokens
    bool supportsTools = true;
};

// Session is defined in core/session.h

// --- Agent status ---------------------------------------------------

enum class AgentState {
    Idle,
    Thinking,    // waiting for API response (streaming)
    AwaitingApproval,  // waiting for user to approve a tool
    ExecutingTool,
    Error
};

struct AgentStatus {
    AgentState state = AgentState::Idle;
    std::string streamingText;
    std::string reasoningText;  // reasoning_content from deepseek-reasoner
    PendingToolCall pendingTool;
    std::string errorMessage;
    std::string statusMessage; // "Working...", "Thinking...", etc.

    // Tool execution progress (updated per-tool during ExecutingTool)
    int toolProgressCurrent = 0;   // index of the tool currently executing (1-based)
    int toolProgressTotal = 0;     // total tools in this round
    std::string currentToolName;   // name of the tool being executed
};

// --- Callbacks for streaming ----------------------------------------

struct StreamCallbacks {
    std::function<void(const std::string& text)> onText;
    std::function<void(const std::string& text)> onThinking;
    std::function<void(const ToolCall& tool)> onToolCall;
    std::function<void()> onFinish;
    std::function<void(const std::string& reason)> onFinishReason;
    std::function<void(const std::string& error)> onError;
    std::function<void(int promptTokens, int completionTokens)> onUsage;
};