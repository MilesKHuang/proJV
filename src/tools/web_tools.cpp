#include "web_tools.h"
#include "registry.h"
#include "json_utils.h"
#include "debug_log.h"

// -- Tool description strings -----------------------------------------------
static constexpr const char* TOOL_WEB_SEARCH_DESC =
    "Search the web using Bing. Returns up to 5 results with titles, URLs, and snippets. No API key required.";
static constexpr const char* TOOL_FETCH_URL_DESC =
    "Fetch the content of a URL. For text/html pages. Max 200KB. Use for reading API docs, web pages, or raw text files.";
static constexpr const char* TOOL_PARAM_QUERY = "The search query";
static constexpr const char* TOOL_PARAM_URL = "The URL to fetch (http:// or https://)";

#include <curl/curl.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstdio>

// --- libcurl helpers -------------------------------------------------------

static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    if (str->size() > 200 * 1024)  // cap at 200KB
        return 0;
    return total;
}

static std::string httpGet(const std::string& url,
                           const std::string& userAgent = "proJV/1.0")
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return "Error: curl_easy_init failed";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // auto-decompress gzip/deflate
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return std::string("Error: ") + curl_easy_strerror(res);
    if (httpCode != 200)
        return "Error: HTTP " + std::to_string(httpCode);
    if (response.empty())
        return "Error: No data received";
    return response;
}

// --- HTML entity decoder ---------------------------------------------------

static std::string decodeHtmlEntities(const std::string& html) {
    std::string text;
    bool inTag = false;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) {
            if (c == '&' && html.substr(i, 5) == "&amp;") { text += '&'; i += 4; continue; }
            if (c == '&' && html.substr(i, 4) == "&lt;")  { text += '<'; i += 3; continue; }
            if (c == '&' && html.substr(i, 4) == "&gt;")  { text += '>'; i += 3; continue; }
            if (c == '&' && html.substr(i, 6) == "&quot;"){ text += '"'; i += 5; continue; }
            if (c == '&' && html.substr(i, 2) == "&#") {
                auto semi = html.find(';', i);
                if (semi != std::string::npos && semi - i < 10) {
                    text += '?'; i = semi; continue;
                }
            }
            text += c;
        }
    }
    return text;
}

// --- Search Bing (no API key needed, HTML scraping) ------------------------
static std::string searchBing(const std::string& query) {
    // URL-encode the query
    std::string encoded;
    for (char c : query) {
        if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-') {
            if (c == ' ') encoded += '+';
            else encoded += c;
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            encoded += buf;
        }
    }

    std::string html = httpGet("https://www.bing.com/search?q=" + encoded);
    if (html.substr(0, 5) == "Error") return html;

    std::string result;
    int count = 0;
    const int MAX_RESULTS = 5;

    // Bing result blocks: <li class="b_algo" ...>
    size_t pos = 0;
    while (count < MAX_RESULTS) {
        auto blockStart = html.find("<li class=\"b_algo\"", pos);
        if (blockStart == std::string::npos) break;

        // Find the end of this <li> block (next <li class="b_algo" or end)
        auto blockEnd = html.find("<li class=\"b_algo\"", blockStart + 1);
        if (blockEnd == std::string::npos)
            blockEnd = html.size();
        std::string block = html.substr(blockStart, blockEnd - blockStart);

        // (a) Extract URL from <a class="tilk" ... href="URL">
        auto tilkPos = block.find("class=\"tilk\"");
        if (tilkPos == std::string::npos) { pos = blockEnd; continue; }
        auto hrefPos = block.find("href=\"", tilkPos);
        if (hrefPos == std::string::npos) { pos = blockEnd; continue; }
        hrefPos += 6;
        auto hrefEnd = block.find('"', hrefPos);
        if (hrefEnd == std::string::npos) { pos = blockEnd; continue; }
        std::string url = block.substr(hrefPos, hrefEnd - hrefPos);
        url = decodeHtmlEntities(url);

        // (b) Extract title from <h2 ...><a ...>TITLE</a></h2>
        auto h2Pos = block.find("<h2");
        if (h2Pos == std::string::npos) { pos = blockEnd; continue; }
        auto titleA = block.find("<a ", h2Pos);
        if (titleA == std::string::npos) { pos = blockEnd; continue; }
        auto titleStart = block.find('>', titleA);
        if (titleStart == std::string::npos) { pos = blockEnd; continue; }
        ++titleStart;
        auto titleEnd = block.find("</a>", titleStart);
        if (titleEnd == std::string::npos) { pos = blockEnd; continue; }
        std::string title = block.substr(titleStart, titleEnd - titleStart);
        title = decodeHtmlEntities(title);

        // (c) Extract snippet from <div class="b_caption"> ... <p> ... </p>
        std::string snippet;
        auto capPos = block.find("class=\"b_caption\"");
        if (capPos != std::string::npos) {
            auto pPos = block.find("<p", capPos);
            if (pPos != std::string::npos) {
                auto pStart = block.find('>', pPos);
                if (pStart != std::string::npos) {
                    ++pStart;
                    auto pEnd = block.find("</p>", pStart);
                    if (pEnd != std::string::npos) {
                        snippet = block.substr(pStart, pEnd - pStart);
                        snippet = decodeHtmlEntities(snippet);
                    }
                }
            }
        }

        ++count;
        result += std::to_string(count) + ". " + title + "\n";
        result += "   " + url + "\n";
        if (!snippet.empty())
            result += "   " + snippet + "\n";
        result += "\n";

        pos = blockEnd;
    }

    if (result.empty())
        return "Error: No results found for \"" + query + "\"";
    return result;
}

// --- web_search tool --------------------------------------------------------
void registerSearchTool(ToolRegistry& registry) {
    ToolDefinition def;
    def.name = "web_search";
    def.description = TOOL_WEB_SEARCH_DESC;
    def.parameters = {
        {"query", "string", TOOL_PARAM_QUERY, true}
    };
    registry.registerTool(def, [](const std::string& args) -> std::string {
        std::string query = extractStringArg(args, "query");
        if (query.empty()) return "Error: Missing 'query' argument";
        debugLogf("[Web] Searching Bing: %s", query.c_str());
        std::string result = searchBing(query);
        debugLogf("[Web] Search done: %zu bytes", result.size());
        return result;
    });
}

// --- fetch_url tool ---------------------------------------------------------
void registerFetchTool(ToolRegistry& registry) {
    ToolDefinition def;
    def.name = "fetch_url";
    def.description = TOOL_FETCH_URL_DESC;
    def.parameters = {
        {"url", "string", TOOL_PARAM_URL, true}
    };
    registry.registerTool(def, [](const std::string& args) -> std::string {
        std::string url = extractStringArg(args, "url");
        if (url.empty()) return "Error: Missing 'url' argument";
        debugLogf("[Web] Fetching: %s", url.c_str());

        if (url.substr(0, 8) != "https://" && url.substr(0, 7) != "http://")
            return "Error: URL must start with http:// or https://";

        std::string result = httpGet(url);
        if (result.substr(0, 5) == "Error") return result;

        std::string clean = decodeHtmlEntities(result);
        if (clean.size() > 100 * 1024)
            clean = clean.substr(0, 100 * 1024) + "\n... (truncated to 100KB)";

        if (clean.size() < result.size() * 0.3) {
            if (result.size() > 100 * 1024)
                result = result.substr(0, 100 * 1024) + "\n... (truncated)";
            return result;
        }
        return clean;
    });
}
