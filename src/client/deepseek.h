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

    ChatResponse sendMessage(const ChatRequest& request, std::string* errorOut = nullptr);
    bool healthCheck(std::string* errorOut = nullptr);
    std::vector<ModelInfo> fetchModels(std::string* errorOut = nullptr);

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
    std::string buildRequestBody(const ChatRequest& request);
    int postWithRetry(void* curl, const std::string& url, const std::string& body, long* outHttpStatus);
};