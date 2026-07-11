#include "web_tools.h"
#include "registry.h"
#include "json_utils.h"
#include "debug_log.h"

// -- Tool description strings (moved from prompts.h) --------------------
static constexpr const char* TOOL_WEB_SEARCH_DESC =
    "Search the web using DuckDuckGo. Returns up to 5 results with titles, URLs, and snippets. No API key required.";
static constexpr const char* TOOL_FETCH_URL_DESC =
    "Fetch the content of a URL. For text/html pages. Max 200KB. Use for reading API docs, web pages, or raw text files.";
static constexpr const char* TOOL_PARAM_QUERY = "The search query";
static constexpr const char* TOOL_PARAM_URL = "The URL to fetch (http:// or https://)";
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

// --- WinHTTP helpers -------------------------------------------------

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

static std::string wide_to_utf8(const wchar_t* w, int wlen) {
    if (wlen <= 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, &s[0], len, nullptr, nullptr);
    return s;
}

static std::string httpGet(const std::string& host, const std::string& path,
                           bool isSecure = true, const std::string& userAgent = "proJV/0.1")
{
    HINTERNET hSession = WinHttpOpen(
        utf8_to_wide(userAgent).c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) return "Error: WinHttpOpen failed";

    // R4: 显式超时（兜底，防止网络异常无限挂起）
    WinHttpSetTimeouts(hSession, 10000, 30000, 30000, 30000);

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        utf8_to_wide(host).c_str(),
        isSecure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
        0
    );
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "Error: WinHttpConnect failed";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET",
        utf8_to_wide(path).c_str(),
        nullptr, nullptr, nullptr,
        isSecure ? WINHTTP_FLAG_SECURE : 0
    );
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "Error: WinHttpOpenRequest failed";
    }

    // Set user agent
    std::wstring ua = utf8_to_wide(userAgent);
    WinHttpAddRequestHeaders(hRequest, ua.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto));

    std::string result;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr))
    {
        std::vector<char> buf(4096);
        DWORD bytesRead = 0;
        while (WinHttpReadData(hRequest, buf.data(), (DWORD)buf.size(), &bytesRead) && bytesRead > 0) {
            result.append(buf.data(), bytesRead);
            if (result.size() > 200 * 1024) { // 200KB limit
                result += "\n... (truncated)";
                break;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (result.empty()) return "Error: No data received";
    return result;
}

// --- HTML parsing helper for DuckDuckGo lite ------------------------

static std::string extractTextContent(const std::string& html) {
    std::string text;
    bool inTag = false;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) {
            // Decode common HTML entities
            if (c == '&' && html.substr(i, 5) == "&amp;") { text += '&'; i += 4; continue; }
            if (c == '&' && html.substr(i, 4) == "&lt;") { text += '<'; i += 3; continue; }
            if (c == '&' && html.substr(i, 4) == "&gt;") { text += '>'; i += 3; continue; }
            if (c == '&' && html.substr(i, 6) == "&quot;") { text += '"'; i += 5; continue; }
            if (c == '&' && html.substr(i, 2) == "&#") {
                auto semi = html.find(';', i);
                if (semi != std::string::npos && semi - i < 10) {
                    text += '?';
                    i = semi;
                    continue;
                }
            }
            text += c;
        }
    }
    return text;
}

// --- Search DuckDuckGo (no API key needed) --------------------------
static std::string searchDuckDuckGo(const std::string& query) {
    // DuckDuckGo lite HTML endpoint
    std::string encodedQuery;
    for (char c : query) {
        if (isalnum(c) || c == ' ' || c == '-') {
            if (c == ' ') encodedQuery += '+';
            else encodedQuery += c;
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            encodedQuery += buf;
        }
    }

    std::string html = httpGet("lite.duckduckgo.com",
        "/lite/?q=" + encodedQuery, true);

    if (html.substr(0, 5) == "Error") return html;

    // Parse results from the HTML
    std::string result;
    size_t pos = 0;
    int count = 0;
    const int MAX_RESULTS = 5;

    // Find result links in the HTML
    while (count < MAX_RESULTS) {
        // Each result has a format: <a href="url" class="result-link">title</a>
        auto linkStart = html.find("<a rel=\"nofollow\" href=\"", pos);
        if (linkStart == std::string::npos) break;

        linkStart += 24;
        auto linkEnd = html.find('"', linkStart);
        if (linkEnd == std::string::npos) break;
        std::string url = html.substr(linkStart, linkEnd - linkStart);

        // Find the result snippet area
        auto snipStart = html.find("<a rel=\"nofollow\" class=\"result-link\"", linkEnd);
        if (snipStart == std::string::npos) break;
        auto snipClose = html.find("</a>", snipStart);
        if (snipClose == std::string::npos) break;
        // Extract title from within the <a> tag
        auto titleStart = html.find('>', snipStart + 44);
        if (titleStart == std::string::npos || titleStart > snipClose) break;
        std::string title = html.substr(titleStart + 1, snipClose - titleStart - 1);
        title = extractTextContent(title);

        // Get snippet (the text after the link tag)
        auto snipTextStart = html.find("class=\"result-snippet\">", snipClose);
        if (snipTextStart != std::string::npos) {
            snipTextStart += 22;
            auto snipTextEnd = html.find("</", snipTextStart);
            if (snipTextEnd != std::string::npos) {
                std::string snippet = html.substr(snipTextStart, snipTextEnd - snipTextStart);
                snippet = extractTextContent(snippet);

                ++count;
                result += std::to_string(count) + ". " + title + "\n";
                result += "   " + url + "\n";
                result += "   " + snippet + "\n\n";
            }
        }

        pos = snipClose + 4;
    }

    if (result.empty()) {
        // Fallback: just return raw text
        std::string clean = extractTextContent(html);
        // Find first meaningful text
        auto bodyStart = clean.find("\n");
        if (bodyStart != std::string::npos)
            result = clean.substr(bodyStart + 1);
        if (result.size() > 2000)
            result = result.substr(0, 2000) + "\n... (truncated)";
    }

    return result;
}

// --- web_search tool -------------------------------------------------
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

        debugLogf("[Web] Searching: %s", query.c_str());
        std::string result = searchDuckDuckGo(query);
        debugLogf("[Web] Search done: %zu bytes", result.size());
        return result;
    });
}

// --- fetch_url tool --------------------------------------------------
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

        // Parse URL
        bool isSecure = true;
        std::string host, path;
        if (url.substr(0, 8) == "https://") {
            host = url.substr(8);
            isSecure = true;
        } else if (url.substr(0, 7) == "http://") {
            host = url.substr(7);
            isSecure = false;
        } else {
            return "Error: URL must start with http:// or https://";
        }

        auto slashPos = host.find('/');
        if (slashPos != std::string::npos) {
            path = host.substr(slashPos);
            host = host.substr(0, slashPos);
        } else {
            path = "/";
        }

        std::string result = httpGet(host, path, isSecure);
        if (result.substr(0, 5) == "Error") return result;

        // Clean up the result: remove HTML tags, limit size
        std::string clean = extractTextContent(result);
        if (clean.size() > 100 * 1024) // 100KB text limit
            clean = clean.substr(0, 100 * 1024) + "\n... (truncated to 100KB)";

        // If the clean text is too short, maybe it's not HTML
        if (clean.size() < result.size() * 0.3) {
            // Probably non-HTML content, return raw but truncated
            if (result.size() > 100 * 1024)
                result = result.substr(0, 100 * 1024) + "\n... (truncated)";
            return result;
        }

        return clean;
    });
}
