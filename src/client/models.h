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

// --- Message --------------------------------------------------------

struct Message {
    int64_t id = 0;         // database row id (0 = not persisted yet)
    std::string role;       // "system", "user", "assistant", "tool"
    std::string content;    // text content (empty if tool_calls present)
    std::string reasoningContent;    // DeepSeek reasoning_content (V4 thinking)
    std::vector<ToolCall> toolCalls;   // for assistant messages
    std::string toolCallId; // for tool result messages
    std::string name;       // for tool result messages

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
    bool stream = true;
    int maxTokens = 8192;
    double temperature = 0.0;
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

struct AppConfig {
    std::string apiKey;
    std::string model = "deepseek-v4-flash";
    std::string baseUrl = "https://api.deepseek.com";
    int maxTokens = 8192;
    double temperature = 0.0;
    std::string configPath;
    std::string workspacePath;   // file access whitelist; empty = exe dir
    std::string cppCompilerPath; // C++ compiler path (e.g. MSBuild.exe, cl.exe)
    std::string pythonPath;      // Python interpreter path

    // Model pricing: map of prefix → {input_price, output_price}
    // Parsed from [model_prices] section in config.toml
    std::vector<ModelPriceEntry> modelPrices;

    // Context window: 0 = auto-detect from model name, >0 = explicit override
    // Set via config.toml "context_window" key. Default 0 enables auto-detection
    // from the model→window mapping in contextWindowForModel().
    size_t contextWindow = 0;

    // Theme name: empty = auto-detect from projv_theme/ (first found).
    // Otherwise, switch to this theme on startup.  Saved to config.toml [theme].
    std::string themeName;

    bool loaded = false;
    std::string loadError;

    bool load(const std::string& path = "");
    bool save();
};

// -- Model → context window mapping (used when config.contextWindow == 0) --
// Returns the known context-window size in tokens for supported models.
// Returns 0 for unknown models — callers should fall back to a safe default (64K).
inline size_t contextWindowForModel(const std::string& model) {
    if (model == "deepseek-v4-pro" || model == "deepseek-v4-flash") return 1'048'576;
    if (model == "deepseek-chat" || model == "deepseek-reasoner" || model == "deepseek-r1") return 65'536;
    // Unknown model → assume conservative 64K
    return 65'536;
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
    int toolProgressCurrent = 0;   // 当前执行到第几个 tool（从 1 开始）
    int toolProgressTotal = 0;     // 本轮一共多少个 tool
    std::string currentToolName;   // 正在执行的 tool 名称
};

// --- Callbacks for streaming ----------------------------------------

struct StreamCallbacks {
    std::function<void(const std::string& text)> onText;
    std::function<void(const std::string& text)> onThinking;
    std::function<void(const ToolCall& tool)> onToolCall;
    std::function<void()> onFinish;
    std::function<void(const std::string& error)> onError;
    std::function<void(int promptTokens, int completionTokens)> onUsage;
};