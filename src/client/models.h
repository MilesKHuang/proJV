#pragma once
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <optional>
#include <chrono>

// --- Tool call / tool result ----------------------------------------

struct ToolParameter {
    std::string name;
    std::string type; // "string", "number", "boolean", "array"
    std::string description;
    bool required = false;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<ToolParameter> parameters;
    // Non-empty -> toJsonSchema() returns this verbatim. MCP tools use it: the
    // server's own JSON Schema is forwarded unchanged, so nested objects, enums,
    // $ref and required arrays survive. ToolParameter can express none of those,
    // and the executor already receives raw JSON -- the schema only shapes what
    // the model sees, it never participates in transport.
    std::string inputSchemaJson;

    // Serialize to JSON for the API request
    std::string toJsonSchema() const;
};

struct ToolCall {
    std::string id; // e.g. "call_abc123"
    std::string name;
    std::string arguments; // JSON string of arguments
    int index = 0;         // position in tool_calls array (for parallel calls)

    bool valid = true;
};

struct ToolResult {
    std::string toolCallId;
    std::string name;
    std::string content; // result text
    bool isError = false;
    bool isCancelled = false;
    // Unified diff of the file this call wrote (write_file / edit_file /
    // md_file / format_file). Display only: it goes into the tool row of the
    // session database for the TUI bubble and is never part of `content`, so it
    // never reaches the API.
    std::string diff;
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
    int64_t id = 0;                  // database row id (0 = not persisted yet)
    std::string role;                // "system", "user", "assistant", "tool"
    std::string content;             // text content (empty if tool_calls present)
    std::string reasoningContent;    // DeepSeek reasoning_content (V4 thinking)
    std::vector<ToolCall> toolCalls; // for assistant messages
    std::string toolCallId;          // for tool result messages
    std::string name;                // for tool result messages
    // Vision attachments. Only user messages may carry images; to_json()
    // drops them for every other role (the API returns 400 otherwise).
    std::vector<ImageAttachment> images;

    // True for messages the framework injected on the model's own request
    // (view_image). Persisted so a replay can tell them apart from what the user
    // actually typed; never sent to the API (to_json ignores it).
    bool synthetic = false;
    // Unified diff of the file a tool call wrote, carried by tool messages only.
    // Persisted with the row so a replayed session keeps the diff bubble; never
    // serialized by to_json() and never counted as input tokens -- it exists to
    // be looked at, not to be read by the model.
    std::string diff;
    // Constructor helpers
    static Message System(std::string content);
    static Message User(std::string content);
    static Message Assistant(std::string content = "");
    static Message Tool(std::string toolCallId, std::string name, std::string content,
                        bool isError = false);
};

// --- API types ------------------------------------------------------

// --- Reasoning effort (official Thinking Mode values) ----------------
//
// The API documents none/low/high/max for `reasoning_effort` and maps these
// aliases itself: minimal -> low, medium/xhigh -> high, ultra -> max. Nothing
// else is legal, so an unknown value is reported as empty here and the caller
// falls back to the official default instead of sending something the server
// would reject.
inline std::string normalizeReasoningEffort(const std::string& raw) {
    std::string v;
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (v == "minimal") return "low";
    if (v == "medium" || v == "xhigh") return "high";
    if (v == "ultra") return "max";
    if (v == "none" || v == "low" || v == "high" || v == "max") return v;
    return std::string();
}

// Official default: thinking is on and the effort is "high".
inline const std::string& kReasoningEffortOfficialDefault() {
    static const std::string v = "high";
    return v;
}

// What proJV ships with: the highest documented level, so a fresh install gets
// the best answer quality. Users may lower it (or turn thinking off with
// "none") in config.toml; an invalid value falls back to the official default.
inline const std::string& kReasoningEffortDefault() {
    static const std::string v = "max";
    return v;
}

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
    // OpenAI-compatible `reasoning_effort`: none / low / high / max. "none" is
    // the documented way to switch thinking off, so it is translated into the
    // thinking toggle above rather than sent as an effort level.
    std::string reasoningEffort = "max";
};

struct ChatResponse {
    std::string id;
    std::string model;
    std::vector<Message> messages;
    int promptTokens = 0;
    int completionTokens = 0;
};

// --- nlohmann::json conversions for ADL -----------------------------
#include "json.hpp"

void to_json(nlohmann::json& j, const ToolCall& tc);
void from_json(const nlohmann::json& j, ToolCall& tc);
void to_json(nlohmann::json& j, const Message& msg);
void from_json(const nlohmann::json& j, Message& msg);
void to_json(nlohmann::json& j, const ToolDefinition& td);

// --- Tool execution -------------------------------------------------

// --- Config ---------------------------------------------------------

// --- Model pricing (configurable via config.toml) -----------------
struct ModelPriceEntry {
    std::string prefix;       // model ID prefix to match (e.g. "deepseek-v4-pro")
    double inputPrice = 0.5;  // $ per 1M input tokens
    double outputPrice = 2.0; // $ per 1M output tokens
};

// Look up price by matching modelId against registered prefixes.
// Returns fallback prices if no match found.
inline double lookupModelPrice(const std::vector<ModelPriceEntry>& prices,
                               const std::string& modelId, bool input) {
    for (const auto& p : prices) {
        if (modelId.find(p.prefix) == 0) return input ? p.inputPrice : p.outputPrice;
    }
    return input ? 0.5 : 2.0; // fallback: cheapest
}

// Whether a model supports tool calls. Every current DeepSeek model does; the
// helper stays so call sites keep one place to ask, and so a future text-only
// model has a single switch to turn off.
inline bool modelSupportsTools(const std::string& modelId) {
    (void)modelId;
    return true;
}

// Whether a model accepts image content blocks. DeepSeek serves vision on the
// Flash line (deepseek-v4-flash); the "vision" tag is accepted too so
// experimental names keep working.
inline bool modelSupportsVision(const std::string& modelId) {
    if (modelId.find("flash") != std::string::npos) return true;
    if (modelId.find("vision") != std::string::npos) return true;
    return false;
}

// Output budget bounds. The ceiling is one source of truth for the config clamp
// and the reasoning-length retry: below the ceiling a retry doubles the budget,
// at the ceiling it stays put. It used to be a literal 65536 inside the retry,
// which is why "double and retry" never doubled the default and halved anything
// larger.
constexpr int kMinOutputTokens = 1024;
constexpr int kMaxOutputTokens = 65536;

// How much the agent asks before running a shell command. See AppConfig.
enum class ApprovalMode { Strict, Balanced, Loose };

// config.toml spelling <-> enum. Unknown text falls back to Balanced.
inline std::string approvalModeName(ApprovalMode m) {
    switch (m) {
    case ApprovalMode::Strict:
        return "strict";
    case ApprovalMode::Loose:
        return "loose";
    case ApprovalMode::Balanced:
    default:
        return "balanced";
    }
}

inline ApprovalMode parseApprovalMode(const std::string& s) {
    if (s == "strict") return ApprovalMode::Strict;
    if (s == "loose") return ApprovalMode::Loose;
    return ApprovalMode::Balanced;
}

struct AppConfig {
    std::string apiKey;
    std::string model = "deepseek-v4-flash";
    std::string baseUrl = "https://api.deepseek.com";
    int maxTokens = kMaxOutputTokens;
    double temperature = 0.0;
    // Thinking effort (config.toml "reasoning_effort"): none / low / high / max.
    // "none" turns thinking off. Invalid values fall back to "high".
    std::string reasoningEffort = "max";
    std::string configPath;
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

    // Report working/blocked/done state to the hosting terminal over OSC 7501
    // ("Program Status Protocol"). Hosts that do not know the sequence drop it
    // silently, so this can be on by default; PROJV_NO_OSC=1 forces it off.
    bool oscStatus = true;

    // Snapshots (/undo). snapshotsMaxWorkspaceGb <= 0 disables the capacity
    // gate -- the gate exists because a 5 GB tree would be copied on every
    // turn; keepDays follows the log retention rule.
    int snapshotsMaxWorkspaceGb = 2;
    int snapshotsKeepDays = 7;

    // Command approval (config.toml "approval_mode", F2). One dial, three stops:
    //   Strict   -- every Ask asks (deletes, overwrites, redirects, commit/push)
    //               and there is NO "always": nothing is remembered.
    //   Balanced -- (default) destructive commands ask; functional overwrites such
    //               as `build > log.txt` run. "Always" is offered and remembered.
    //   Loose    -- only the red line (format / partition / device) is refused;
    //               every other command runs without a dialog.
    // Deny is outside the dial: formatting is never executed and has no override.
    ApprovalMode approvalMode = ApprovalMode::Balanced;

    // Commands the user allowed forever with the "ALWAYS" button. Exact text,
    // persisted in config.toml as approval_grants so a restart does not forget.
    // Never written by the model -- only by approveTool(2).
    std::vector<std::string> approvalGrants;

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
    std::string id;    // e.g. "deepseek-v4-flash"
    std::string label; // display name (same as id when simplified)
};

// Session is defined in core/session.h

// --- Agent status ---------------------------------------------------

enum class AgentState {
    Idle,
    Thinking,         // waiting for API response (streaming)
    AwaitingApproval, // waiting for user to approve a tool
    ExecutingTool,
    Error
};

struct AgentStatus {
    AgentState state = AgentState::Idle;
    std::string streamingText;
    std::string reasoningText; // reasoning_content (thinking mode)
    std::string errorMessage;
    std::string statusMessage; // "Working...", "Thinking...", etc.

    // Tool execution progress (updated per-tool during ExecutingTool)
    int toolProgressCurrent = 0; // index of the tool currently executing (1-based)
    int toolProgressTotal = 0;   // total tools in this round
    std::string currentToolName; // name of the tool being executed
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
