#include "web_tools.h"
#include "registry.h"
#include "json_utils.h"
#include "client/deepseek.h"
#include "debug_log.h"

#include <string>

// -- Tool description strings -----------------------------------------------
// Both tools are billed per successful request, so the descriptions say so
// explicitly to suppress careless model usage.
static constexpr const char* TOOL_WEB_SEARCH_DESC =
    "Search the web with DeepSeek's official Search API (structured results with a "
    "rerank score and an authority tier). Returns up to `count` results, each with "
    "title, url, summary, date and authority. Supports query operators: site:, -site:, "
    "\"exact phrase\", -term, lang:, after:, before:. Billed per request -- call it only "
    "when you need real-time or authoritative information.";
static constexpr const char* TOOL_FETCH_URL_DESC =
    "Fetch one web page with DeepSeek's official Fetch API and return its main content "
    "as Markdown (truncated to 100KB), prefixed with the page title. Billed per request. "
    "Prefer web_search summaries for cheap facts; use this when you need the full page.";

static constexpr const char* TOOL_PARAM_QUERY =
    "The search query (1-128 characters). Query operators are forwarded verbatim.";
static constexpr const char* TOOL_PARAM_COUNT = "Number of results to return, 1-50. Default 10.";
static constexpr const char* TOOL_PARAM_NEED_CONTENT =
    "Set true to also return each page's extracted content (costs more context). "
    "Default false -- summary/snippet is enough for most questions.";
static constexpr const char* TOOL_PARAM_URL =
    "The page URL to fetch (http:// or https://, at most 8192 characters).";

static constexpr int kMaxUrlLength = 8192;

// --- web_search tool --------------------------------------------------------
void registerSearchTool(ToolRegistry& registry, DeepSeekClient& client) {
    ToolDefinition def;
    def.name = "web_search";
    def.description = TOOL_WEB_SEARCH_DESC;
    def.parameters = {{"query", "string", TOOL_PARAM_QUERY, true},
                      {"count", "number", TOOL_PARAM_COUNT, false},
                      {"need_content", "boolean", TOOL_PARAM_NEED_CONTENT, false}};
    registry.registerTool(def, [&client](const std::string& args) -> std::string {
        std::string query = extractStringArg(args, "query");
        if (query.empty()) return "Error: Missing 'query' argument";

        int count = extractIntArg(args, "count", 10);
        if (count < 1) count = 1;
        if (count > 50) count = 50;
        bool needContent = extractBoolArg(args, "need_content", false);

        debugLogf("[Web] Search: %s (count=%d need_content=%d)", query.c_str(), count,
                  needContent ? 1 : 0);
        std::string error;
        std::string result = client.searchWeb(query, count, needContent, &error);
        debugLogf("[Web] Search done: %zu bytes", result.size());
        return result;
    });
}

// --- fetch_url tool ---------------------------------------------------------
void registerFetchTool(ToolRegistry& registry, DeepSeekClient& client) {
    ToolDefinition def;
    def.name = "fetch_url";
    def.description = TOOL_FETCH_URL_DESC;
    def.parameters = {{"url", "string", TOOL_PARAM_URL, true}};
    registry.registerTool(def, [&client](const std::string& args) -> std::string {
        std::string url = extractStringArg(args, "url");
        if (url.empty()) return "Error: Missing 'url' argument";
        if (url.size() > (size_t)kMaxUrlLength)
            return "Error: URL exceeds the 8192 character limit";
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            return "Error: URL must start with http:// or https://";

        debugLogf("[Web] Fetch: %s", url.c_str());
        std::string error;
        std::string result = client.fetchWeb(url, &error);
        debugLogf("[Web] Fetch done: %zu bytes", result.size());
        return result;
    });
}
