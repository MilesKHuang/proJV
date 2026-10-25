#pragma once
#include "models.h"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>

// -- Retry configuration for HTTP requests ------------------------------------
struct RetryConfig {
    int maxRetries = 3;
    int initialDelayMs = 1000;
    int backoffMultiplier = 2;
    int maxDelayMs = 30000;

    int delayForAttempt(int attempt) const {
        int delay = initialDelayMs;
        for (int i = 0; i < attempt; ++i) delay *= backoffMultiplier;
        return std::min(delay, maxDelayMs);
    }
};

class DeepSeekClient {
public:
    DeepSeekClient();
    ~DeepSeekClient();

    void setConfig(const AppConfig& cfg);
    bool isReady() const { return !config.apiKey.empty(); }
    std::string getModel() const { std::lock_guard<std::mutex> lock(mutex); return config.model; }

    bool streamBlocking(const ChatRequest& request, StreamCallbacks callbacks);
    void cancel();

    // Serialize one chat request to the JSON body we POST. Public so tests can
    // assert exactly which fields go on the wire (thinking, tool_choice, ...).
    std::string buildRequestBody(const ChatRequest& request);

    ChatResponse sendMessage(const ChatRequest& request, std::string* errorOut = nullptr);
    bool healthCheck(std::string* errorOut = nullptr);
    std::vector<ModelInfo> fetchModels(std::string* errorOut = nullptr);

    // -- Official Search / Fetch tools ------------------------------------
    // POST {baseUrl}/v1/tools/search  {query, count, need_content}
    // POST {baseUrl}/v1/tools/fetch   {url}
    // Both use the same Bearer key as chat. Billed per successful request.
    // Return a model-readable text block, or a string starting with "Error:".
    std::string searchWeb(const std::string& query, int count, bool needContent,
                          std::string* errorOut = nullptr);
    std::string fetchWeb(const std::string& url, std::string* errorOut = nullptr);

    // Pure response formatters (no network) -- exposed for unit tests.
    // Search: one block per results[] item. Empty string == no results.
    static std::string formatSearchResults(const std::string& jsonBody, bool needContent);
    // Fetch: "# title\n\n<markdown content>", truncated to 100KB.
    static std::string formatFetchPage(const std::string& jsonBody);
    // OpenAI-compatible error body -> "Error: HTTP <code> ...: <message>".
    static std::string formatApiError(long httpStatus, const std::string& body);

    // Exposed for SSE write callback
    size_t parseSSEChunk(const std::string& buffer, size_t startOffset, StreamCallbacks& callbacks);

private:
    AppConfig config;
    std::atomic<bool> cancelFlag{false};
    std::atomic<bool> streaming{false};
    mutable std::mutex mutex;
    bool inToolCall_ = false;
    // DSML detection: once markers appear, suppress subsequent content chunks.
    bool dsmlDetected_ = false;
    RetryConfig retryConfig_;

    static bool isRetryableHttpStatus(unsigned long statusCode);

    // libcurl helpers
    void* createEasyHandle();
    void initLibcurl();
    int postWithRetry(void* curl, const std::string& url, const std::string& body, long* outHttpStatus);

    // POST a JSON body to {baseUrl}{path} with Bearer auth + retry.
    // Returns a CURLcode (0 == CURLE_OK); the body and HTTP status are out params.
    int postJson(const std::string& path, const std::string& body,
                 std::string& outBody, long* outHttpStatus);
};
