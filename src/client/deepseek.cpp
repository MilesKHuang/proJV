#include <curl/curl.h>
#include "deepseek.h"
#include "json.hpp"
#include "debug_log.h"

// -- Tool-call filter markers (moved from prompts.h) --------------------
static constexpr const char* TOOL_CALL_START_MARKERS[] = {
    "[TOOL_CALL]", "<tool_call",
    "<invoke ", "<function_calls>",
    nullptr
};
static constexpr const char* TOOL_CALL_END_MARKERS[] = {
    "[/TOOL_CALL]", "</tool_call>",
    "</invoke>", "</function_calls>",
    nullptr
};
static constexpr int TOOL_CALL_MARKER_COUNT = 4;
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstring>

// -- Timeouts ----------------------------------------------------------------
static constexpr int  kStreamTotalTimeoutSec  = 900;   // 15 min hard cap
static constexpr int  kStreamLowSpeedLimit    = 1;     // bytes/sec (very low â€?tolerate API pauses)
static constexpr int  kStreamLowSpeedTime     = 300;   // 5 min below limit â†?timeout (was 120s; give reasoner more time)
static constexpr int  kConnectTimeoutSec      = 30;
static constexpr int  kSendTimeoutSec         = 60;    // sending large POST body

// -- SSE write-callback context -----------------------------------------------
struct StreamContext {
    DeepSeekClient*              client;
    StreamCallbacks*             callbacks;
    std::atomic<bool>*           cancelFlag;
    std::string                  sseBuffer;
    size_t                       totalBytes    = 0;
    long                         httpStatus    = 0;
    bool                         finished      = false;
    bool                         headerDone    = false;
    bool                         anyContent    = false;
    std::string                  errorBody;           // captured on HTTP error
};

// -- libcurl write callback (receives SSE chunks) ----------------------------
static size_t streamWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* ctx = static_cast<StreamContext*>(userdata);
    size_t bytes = size * nmemb;

    // If we haven't checked HTTP status yet, skip data until headers are done
    if (!ctx->headerDone) {
        // Defer â€?data before status check means we need to read status
        ctx->headerDone = true;
    }

    if (ctx->cancelFlag && ctx->cancelFlag->load(std::memory_order_acquire))
        return 0; // abort transfer

    if (ctx->httpStatus != 200) {
        // Capture error body instead of discarding it â€?
        // so Agent can distinguish context-overflow vs other 4xx causes.
        ctx->errorBody.append(ptr, bytes);
        return bytes;
    }

    ctx->sseBuffer.append(ptr, bytes);
    ctx->totalBytes += bytes;
    ctx->anyContent = true;

    // Process complete SSE events inline
    size_t offset = 0;
    while (offset < ctx->sseBuffer.size()) {
        size_t newOff = ctx->client->parseSSEChunk(ctx->sseBuffer, offset, *ctx->callbacks);
        if (newOff == SIZE_MAX) {
            ctx->finished = true;
            if (offset > 0) ctx->sseBuffer = ctx->sseBuffer.substr(offset);
            return 0;
        }
        if (newOff == offset) break;
        offset = newOff;
    }
    if (offset > 0)
        ctx->sseBuffer = ctx->sseBuffer.substr(offset);

    return bytes;
}

// -- libcurl header callback (capture HTTP status) ---------------------------
static size_t headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* ctx = static_cast<StreamContext*>(userdata);
    size_t total = size * nmemb;
    std::string line(ptr, total);

    if (line.rfind("HTTP/", 0) == 0) {
        auto sp = line.find(' ');
        if (sp != std::string::npos) {
            auto sp2 = line.find(' ', sp + 1);
            std::string codeStr = line.substr(sp + 1, sp2 - sp - 1);
            ctx->httpStatus = std::strtol(codeStr.c_str(), nullptr, 10);
        }
    }
    return total;
}

// -- libcurl progress callback (frequent â€?used for cancel detection) --------
static int progressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto* ctx = static_cast<StreamContext*>(userdata);
    if (ctx->cancelFlag && ctx->cancelFlag->load(std::memory_order_acquire))
        return 1; // non-zero â†?abort transfer
    return 0;
}

// -- Blocking (non-streaming) write callback context -------------------------
struct BlockContext {
    std::string body;
    std::atomic<bool>* cancelFlag;
};

static size_t blockWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* ctx = static_cast<BlockContext*>(userdata);
    if (ctx->cancelFlag && ctx->cancelFlag->load())
        return 0;
    ctx->body.append(ptr, size * nmemb);
    return size * nmemb;
}

// -- libcurl init (called once) -----------------------------------------------
static bool g_curlInitialized = false;
void DeepSeekClient::initLibcurl() {
    if (!g_curlInitialized) {
        curl_global_init(CURL_GLOBAL_ALL);
        g_curlInitialized = true;
    }
}

DeepSeekClient::DeepSeekClient() {
    initLibcurl();
}

DeepSeekClient::~DeepSeekClient() {
    cancel();
}

// -- Create a curl easy handle with per-call defaults ------------------------
void* DeepSeekClient::createEasyHandle() {
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)kConnectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)kSendTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "proJV/1.0");
    return curl;
}

std::string DeepSeekClient::buildRequestBody(const ChatRequest& request) {
    nlohmann::json body;
    body["model"] = request.model;
    body["stream"] = request.stream;
    body["max_tokens"] = request.maxTokens;
    body["temperature"] = request.temperature;

    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& msg : request.messages) {
        nlohmann::json m;
        to_json(m, msg);  // delegates to models.cpp â€?handles content=null for tool_calls, etc.
        msgs.push_back(m);
    }
    body["messages"] = msgs;

    if (!request.tools.empty()) {
        nlohmann::json tools = nlohmann::json::array();
        for (const auto& td : request.tools) {
            nlohmann::json t;
            to_json(t, td);
            tools.push_back(t);
        }
        body["tools"] = tools;
    }

    return body.dump(2);
}

int DeepSeekClient::postWithRetry(void* curl, const std::string& url, const std::string& body, long* outHttpStatus) {
    int retries = 0;
    while (true) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());

        CURLcode res = curl_easy_perform(curl);

        if (outHttpStatus) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, outHttpStatus);
        }

        if (res == CURLE_OK) {
            if (*outHttpStatus == 200 || *outHttpStatus == 0)
                return (int)res;
            if (isRetryableHttpStatus(*outHttpStatus) && retries < retryConfig_.maxRetries) {
                ++retries;
                debugLogf("[DeepSeek] HTTP %ld retryable, attempt %d/%d",
                    *outHttpStatus, retries, retryConfig_.maxRetries);
                std::this_thread::sleep_for(std::chrono::milliseconds(retryConfig_.delayForAttempt(retries - 1)));
                continue;
            }
            return (int)res;
        }

        // Network-level error â€?retry if we haven't exceeded limit
        if (retries < retryConfig_.maxRetries) {
            ++retries;
            debugLogf("[DeepSeek] curl_easy_perform error %d, retry %d/%d",
                (int)res, retries, retryConfig_.maxRetries);
            std::this_thread::sleep_for(std::chrono::milliseconds(retryConfig_.delayForAttempt(retries - 1)));
            continue;
        }
        return (int)res;
    }
}

// -- Cancel: set flag â†?write/progress callbacks will abort ------------------
void DeepSeekClient::cancel() {
    auto tid = std::this_thread::get_id();
    debugLogf("[DeepSeek] cancel called (thread=%08X)", *(unsigned int*)&tid);
    cancelFlag.store(true, std::memory_order_release);
    streaming.store(false, std::memory_order_release);
}

// -- streamBlocking (synchronous, runs on calling thread) --------------------
bool DeepSeekClient::streamBlocking(const ChatRequest& request, StreamCallbacks callbacks) {
    debugLog("[DeepSeek] streamBlocking (synchronous)");
    cancelFlag.store(false, std::memory_order_release);
    streaming.store(true, std::memory_order_release);
    inToolCall_ = false;
    dsmlDetected_ = false;

    StreamContext ctx;
    ctx.client      = this;
    ctx.callbacks   = &callbacks;
    ctx.cancelFlag  = &cancelFlag;

    int transparentRetries = 0;
    int transparentRetryAfterContent = 0;
    static constexpr int MAX_RETRIES = 3;
    static constexpr int MAX_RETRIES_AFTER_CONTENT = 2;

    bool success = false;
    while (true) {
        CURL* curl = (CURL*)createEasyHandle();
        if (!curl) {
            int maxR = ctx.anyContent ? MAX_RETRIES_AFTER_CONTENT : MAX_RETRIES;
            int& r = ctx.anyContent ? transparentRetryAfterContent : transparentRetries;
            if (r < maxR && !cancelFlag.load(std::memory_order_acquire)) { ++r; std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
            if (callbacks.onError) callbacks.onError("Failed to init curl");
            break;
        }
        struct CurlGuard { CURL* h; ~CurlGuard() { if (h) curl_easy_cleanup(h); } };
        CurlGuard guard{curl};

        std::string body = buildRequestBody(request);
        std::string url = config.baseUrl;
        if (!url.empty() && url.back() != '/') url += '/';
        url += "v1/chat/completions";

        std::string auth = "Authorization: Bearer " + config.apiKey;
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        headers = curl_slist_append(headers, auth.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        ctx.sseBuffer.clear(); ctx.totalBytes = 0;
        ctx.httpStatus = 0; ctx.finished = false; ctx.headerDone = false;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)kStreamTotalTimeoutSec);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, (long)kStreamLowSpeedLimit);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)kStreamLowSpeedTime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        long httpStatus = ctx.httpStatus;
        if (res != CURLE_OK && !ctx.finished) {
            const char* es = curl_easy_strerror(res);
            int maxR = ctx.anyContent ? MAX_RETRIES_AFTER_CONTENT : MAX_RETRIES;
            int& r = ctx.anyContent ? transparentRetryAfterContent : transparentRetries;
            if (r < maxR && !cancelFlag.load(std::memory_order_acquire)) { ++r; std::this_thread::sleep_for(std::chrono::milliseconds(retryConfig_.delayForAttempt(r-1))); continue; }
            if (callbacks.onError) callbacks.onError(std::string("Stream error: ") + es);
            break;
        }
        if (httpStatus != 200) {
            int maxR = ctx.anyContent ? MAX_RETRIES_AFTER_CONTENT : MAX_RETRIES;
            int& r = ctx.anyContent ? transparentRetryAfterContent : transparentRetries;
            if (isRetryableHttpStatus(httpStatus) && r < maxR && !cancelFlag.load(std::memory_order_acquire)) {
                ++r; int d = (httpStatus==429)?2000:retryConfig_.delayForAttempt(r-1);
                std::this_thread::sleep_for(std::chrono::milliseconds(d)); continue;
            }
            std::string errMsg = "HTTP "+std::to_string(httpStatus);
            if (!ctx.errorBody.empty()) errMsg += ": " + ctx.errorBody;
            if (callbacks.onError) callbacks.onError(errMsg);
            break;
        }
        if (!ctx.sseBuffer.empty()) parseSSEChunk(ctx.sseBuffer, 0, callbacks);
        if (ctx.anyContent && !ctx.finished && ctx.totalBytes < 2048) {
            int& r = transparentRetryAfterContent;
            if (r < MAX_RETRIES_AFTER_CONTENT && !cancelFlag.load(std::memory_order_acquire)) { ++r; std::this_thread::sleep_for(std::chrono::milliseconds(retryConfig_.delayForAttempt(r-1))); continue; }
        }
        if (ctx.anyContent && !ctx.finished && callbacks.onFinish) callbacks.onFinish();
        success = true;
        break;
    }
    streaming.store(false, std::memory_order_release);
    return success;
}

// -- isRetryableHttpStatus ----------------------------------------------------
bool DeepSeekClient::isRetryableHttpStatus(unsigned long statusCode) {
    if (statusCode == 429) return true;
    if (statusCode >= 500 && statusCode < 600) return true;
    return false;
}

// -- sendMessage (non-streaming) ----------------------------------------------
ChatResponse DeepSeekClient::sendMessage(const ChatRequest& request, std::string* errorOut) {
    ChatResponse resp;
    CURL* curl = (CURL*)createEasyHandle();
    if (!curl) {
        if (errorOut) *errorOut = "Failed to init curl";
        return resp;
    }
    struct CurlGuard { CURL* h; ~CurlGuard() { if (h) curl_easy_cleanup(h); } };
    CurlGuard guard{curl};

    BlockContext blockCtx;
    blockCtx.cancelFlag = &cancelFlag;

    std::string body = buildRequestBody(request);
    std::string url = config.baseUrl;
    if (url.back() != '/') url += '/';
    url += "v1/chat/completions";

    std::string auth = "Authorization: Bearer " + config.apiKey;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, blockWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &blockCtx);

    long httpStatus = 0;
    int rc = postWithRetry(curl, url, body, &httpStatus);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        if (errorOut) *errorOut = std::string("curl error: ") + curl_easy_strerror((CURLcode)rc);
        return resp;
    }
    if (httpStatus != 200) {
        if (errorOut) *errorOut = "HTTP " + std::to_string(httpStatus) + ": " + blockCtx.body;
        return resp;
    }

    // Parse JSON response
    try {
        auto j = nlohmann::json::parse(blockCtx.body);
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            const auto& choice = j["choices"][0];
            Message msg;
            msg.role = "assistant";
            msg.content = choice["message"].value("content", "");
            resp.messages.push_back(msg);
        }
        if (j.contains("usage")) {
            auto& u = j["usage"];
            resp.promptTokens     = u.value("prompt_tokens", 0);
            resp.completionTokens = u.value("completion_tokens", 0);
        }
    } catch (...) {}
    return resp;
}

// -- healthCheck --------------------------------------------------------------
bool DeepSeekClient::healthCheck(std::string* errorOut) {
    CURL* curl = (CURL*)createEasyHandle();
    if (!curl) { if (errorOut) *errorOut = "curl init failed"; return false; }
    struct CurlGuard { CURL* h; ~CurlGuard() { if (h) curl_easy_cleanup(h); } };
    CurlGuard guard{curl};

    BlockContext blockCtx;
    blockCtx.cancelFlag = nullptr;

    std::string url = config.baseUrl;
    if (url.back() != '/') url += '/';
    url += "v1/models";

    std::string auth = "Authorization: Bearer " + config.apiKey;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, blockWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &blockCtx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    if (res != CURLE_OK) {
        if (errorOut) *errorOut = curl_easy_strerror(res);
        return false;
    }
    return (httpStatus == 200);
}

// -- fetchModels --------------------------------------------------------------
std::vector<ModelInfo> DeepSeekClient::fetchModels(std::string* errorOut) {
    std::vector<ModelInfo> models;
    debugLog("[DeepSeek] fetchModels called");

    CURL* curl = (CURL*)createEasyHandle();
    if (!curl) { if (errorOut) *errorOut = "curl init failed"; return models; }
    struct CurlGuard { CURL* h; ~CurlGuard() { if (h) curl_easy_cleanup(h); } };
    CurlGuard guard{curl};

    BlockContext blockCtx;
    blockCtx.cancelFlag = nullptr;

    std::string url = config.baseUrl;
    if (url.back() != '/') url += '/';
    url += "v1/models";

    std::string auth = "Authorization: Bearer " + config.apiKey;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, blockWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &blockCtx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        if (errorOut) *errorOut = curl_easy_strerror(res);
        return models;
    }

    try {
        auto j = nlohmann::json::parse(blockCtx.body);
        if (j.contains("data") && j["data"].is_array()) {
            for (const auto& m : j["data"]) {
                ModelInfo mi;
                mi.id = m.value("id", "");
                mi.label = m.value("id", "");
                models.push_back(mi);
            }
        }
    } catch (...) {
        if (errorOut) *errorOut = "JSON parse error in models response";
    }
    debugLogf("[DeepSeek] fetchModels: got %zu models", models.size());
    return models;
}

// ======================================================================
//  Functions below are ported from the original WinHTTP implementation.
//  They are HTTP-layer-agnostic and work identically with libcurl.
// ======================================================================

// -- Tool-call filter: find earliest marker -------------------------------
static std::pair<size_t, size_t> findFirstMarker(
    const std::string& text, const char* const* markers, int count) {
    size_t bestIdx = std::string::npos;
    size_t bestLen = 0;
    for (int i = 0; i < count; ++i) {
        size_t pos = text.find(markers[i]);
        if (pos != std::string::npos && (bestIdx == std::string::npos || pos < bestIdx)) {
            bestIdx = pos;
            bestLen = std::strlen(markers[i]);
        }
    }
    return {bestIdx, bestLen};
}

// -- Tool-call filter: strip wrapper markers -------------------------------
static std::string filterToolCallDelta(const std::string& delta, bool& inToolCall) {
    if (delta.empty()) return {};
    std::string output;
    size_t pos = 0;
    while (pos < delta.size()) {
        std::string tail(delta.c_str() + pos, delta.size() - pos);
        if (inToolCall) {
            auto [idx, len] = findFirstMarker(tail, TOOL_CALL_END_MARKERS, TOOL_CALL_MARKER_COUNT);
            if (idx == std::string::npos) break;
            pos += idx + len;
            inToolCall = false;
        } else {
            auto [idx, len] = findFirstMarker(tail, TOOL_CALL_START_MARKERS, TOOL_CALL_MARKER_COUNT);
            if (idx == std::string::npos) {
                output.append(tail);
                break;
            }
            output.append(tail, 0, idx);
            pos += idx + len;
            inToolCall = true;
        }
    }
    return output;
}

// -- setConfig --------------------------------------------------------------
void DeepSeekClient::setConfig(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex);
    config = cfg;
}

// -- buildRequestBody (fallback for malformed JSON) -------------------------
// Already defined above; this comment documents the split point.

// -- SSE Parser (unchanged) ------------------------------------------------

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

size_t DeepSeekClient::parseSSEChunk(
    const std::string& buffer,
    size_t startOffset,
    StreamCallbacks& callbacks
) {
    auto eventEnd = buffer.find("\n\n", startOffset);
    if (eventEnd == std::string::npos) return startOffset;

    std::string event = buffer.substr(startOffset, eventEnd - startOffset);
    size_t consumed = eventEnd + 2;

    std::string data;
    std::string eventType;
    size_t pos = 0;
    while (pos < event.size()) {
        auto nl = event.find('\n', pos);
        if (nl == std::string::npos) nl = event.size();
        std::string line = event.substr(pos, nl - pos);

        if (line.substr(0, 6) == "event:") {
            eventType = trim(line.substr(6));
        } else if (line.substr(0, 6) == "data: ") {
            data += line.substr(6);
        } else if (line.substr(0, 5) == "data:") {
            std::string payload = line.substr(5);
            if (payload.find("[DONE]") != std::string::npos) {
                debugLog("[DeepSeek] SSE: received [DONE] -- stream ended");
                return SIZE_MAX;
            }
            data += payload;
        }
        pos = nl + 1;
    }

    if (data.empty()) return consumed;

    if (data.find("[DONE]") != std::string::npos) {
        debugLog("[DeepSeek] SSE: [DONE] in accumulated data -- stream ended");
        return SIZE_MAX;
    }

    try {
        auto j = nlohmann::json::parse(data);

        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
            return consumed;

        const auto& choice = j["choices"][0];

        if (!choice.contains("delta") || !choice["delta"].is_object()) {
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                std::string finishReason = choice["finish_reason"].is_string()
                    ? choice["finish_reason"].get<std::string>() : "";
                debugLogf("[DeepSeek] SSE: finish_reason=%s (no delta)", finishReason.c_str());
                if (callbacks.onFinishReason) callbacks.onFinishReason(finishReason);
                if (callbacks.onFinish) callbacks.onFinish();
            }
            return consumed;
        }

        const auto& delta = choice["delta"];

        if (delta.contains("content") && delta["content"].is_string()) {
            std::string content = delta["content"].get<std::string>();
            if (!content.empty() && callbacks.onText) {
                // â”€â”€ DSML filtering (copied from CodeWhale) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
                if (!dsmlDetected_) {
                    static const char* dsmlMarkers[] = {
                        "<|DSML|>", "<|tool_calls_begin|>",
                        "\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C" ">",
                        "\xEF\xBD\x9C" "tool" "\xE2\x96\x81" "calls" "\xE2\x96\x81" "begin" "\xEF\xBD\x9C" ">",
                        "\xEF\xBD\x9C" "tool" "\xEF\xBD\x9C" "calls" "\xEF\xBD\x9C" ">",
                        "</" "\xEF\xBD\x9C" "tool" "\xE2\x96\x81" "calls" "\xEF\xBD\x9C" ">",
                        "</" "\xEF\xBD\x9C" "tool" "\xEF\xBD\x9C" "calls" "\xEF\xBD\x9C" ">",
                        "</tool_calls>",
                        "</" "\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C" ">",
                        "<" "\xEF\xBD\x9C" "DSML" "\xEF\xBD\x9C" ">",
                    };
                    size_t early = std::string::npos;
                    for (const char* m : dsmlMarkers) {
                        size_t p = content.find(m);
                        if (p != std::string::npos && (early == std::string::npos || p < early))
                            early = p;
                    }
                    if (early == std::string::npos && !content.empty()) {
                        unsigned char c = (unsigned char)content[0];
                        if (c == '<' && content.size() > 1) {
                            unsigned char c2 = (unsigned char)content[1];
                            if (c2 == '|' || c2 == 0xEF || c2 == 0xE2) early = 0;
                        }
                    }
                    if (early != std::string::npos) {
                        content = content.substr(0, early);
                        while (!content.empty() && (content.back() == ' ' || content.back() == '\t'
                            || content.back() == '\r' || content.back() == '\n'))
                            content.pop_back();
                        dsmlDetected_ = true;
                        debugLog("[DeepSeek] DSML marker detected, suppressing further content");
                    }
                } else {
                    content.clear();
                }
                if (!content.empty()) {
                    std::string visible = filterToolCallDelta(content, inToolCall_);
                    if (visible != content) debugLogf("[DeepSeek] Tool-call wrapper stripped");
                    if (!visible.empty()) callbacks.onText(visible);
                }
            }
        }

        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
            std::string reasoning = delta["reasoning_content"].get<std::string>();
            if (!reasoning.empty() && callbacks.onThinking) {
                callbacks.onThinking(reasoning);
            }
        }

        if (delta.contains("tool_calls") && delta["tool_calls"].is_array() && callbacks.onToolCall) {
            for (const auto& tcItem : delta["tool_calls"]) {
                ToolCall tc;
                if (tcItem.contains("index") && tcItem["index"].is_number_integer())
                    tc.index = tcItem["index"].get<int>();
                if (tcItem.contains("id") && tcItem["id"].is_string())
                    tc.id = tcItem["id"].get<std::string>();
                if (tcItem.contains("function") && tcItem["function"].is_object()) {
                    const auto& fn = tcItem["function"];
                    if (fn.contains("name") && fn["name"].is_string())
                        tc.name = fn["name"].get<std::string>();
                    if (fn.contains("arguments") && fn["arguments"].is_string())
                        tc.arguments = fn["arguments"].get<std::string>();
                }
                callbacks.onToolCall(tc);
            }
        }

        if (j.contains("usage") && j["usage"].is_object() && callbacks.onUsage) {
            int prompt = 0, completion = 0;
            const auto& usage = j["usage"];
            if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number_integer())
                prompt = usage["prompt_tokens"].get<int>();
            if (usage.contains("completion_tokens") && usage["completion_tokens"].is_number_integer())
                completion = usage["completion_tokens"].get<int>();
            if (prompt > 0 || completion > 0) {
                callbacks.onUsage(prompt, completion);
            }
        }

        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            std::string finishReason = choice["finish_reason"].is_string()
                ? choice["finish_reason"].get<std::string>() : "";
            debugLogf("[DeepSeek] SSE: finish_reason=%s", finishReason.c_str());
            if (callbacks.onFinishReason) callbacks.onFinishReason(finishReason);
            if (callbacks.onFinish) callbacks.onFinish();
            return consumed;
        }
    } catch (const nlohmann::json::parse_error& e) {
        debugLogf("[DeepSeek] SSE: JSON parse error: %s", e.what());
    } catch (const std::exception& e) {
        debugLogf("[DeepSeek] SSE: unexpected exception: %s", e.what());
    }

    return consumed;
}