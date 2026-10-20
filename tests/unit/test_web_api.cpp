// Tests for the official DeepSeek Search / Fetch integration (doc 03).
// No network: the pure formatters and the error mapper are exercised directly.
#include "doctest.h"

#include "client/deepseek.h"
#include "json.hpp"

#include <string>

namespace {

std::string searchBody(std::initializer_list<nlohmann::json> items) {
    nlohmann::json j;
    j["results"] = nlohmann::json::array();
    for (const auto& it : items) j["results"].push_back(it);
    return j.dump();
}

} // namespace

TEST_CASE("search: results are rendered with score, authority, summary") {
    nlohmann::json a = {
        {"rerank_score", 2.4},
        {"title", "DeepSeek V4 model released"},
        {"url", "https://example.com/deepseek-v4"},
        {"snippet", "short"},
        {"summary", "long summary text"},
        {"content", "page body"},
        {"site_name", "Example News"},
        {"date", "2026-06-01T00:00:00+00:00"},
        {"authority", 3}
    };
    nlohmann::json b = {
        {"rerank_score", 0.9},
        {"title", "Low quality"},
        {"url", "https://example.com/low"},
        {"snippet", "snippet only"},
        {"content", ""},
        {"site_name", nullptr},
        {"date", nullptr},
        {"authority", -1}
    };
    const std::string out = DeepSeekClient::formatSearchResults(searchBody({a, b}), false);

    CHECK(out.find("1. [score=2.4][authority=3] DeepSeek V4 model released") != std::string::npos);
    CHECK(out.find("   https://example.com/deepseek-v4") != std::string::npos);
    CHECK(out.find("   date: 2026-06-01T00:00:00+00:00") != std::string::npos);
    CHECK(out.find("   site: Example News") != std::string::npos);
    CHECK(out.find("   long summary text") != std::string::npos);
    // need_content=false: page bodies must not leak into the output.
    CHECK(out.find("page body") == std::string::npos);
    // summary empty -> snippet fallback; null site/date are skipped, not "null".
    CHECK(out.find("   snippet only") != std::string::npos);
    CHECK(out.find("null") == std::string::npos);
    CHECK(out.find("2. [score=0.9][authority=-1]") != std::string::npos);
}

TEST_CASE("search: need_content emits the page body") {
    nlohmann::json a = {
        {"rerank_score", 2.0}, {"title", "T"}, {"url", "https://e.com"},
        {"snippet", "s"}, {"summary", "sum"}, {"content", "BODY TEXT"},
        {"authority", 4}
    };
    const std::string out = DeepSeekClient::formatSearchResults(searchBody({a}), true);
    CHECK(out.find("--- content ---") != std::string::npos);
    CHECK(out.find("BODY TEXT") != std::string::npos);
}

TEST_CASE("search: empty results array and malformed bodies") {
    CHECK(DeepSeekClient::formatSearchResults(searchBody({}), false).empty());
    CHECK(DeepSeekClient::formatSearchResults("{\"results\":[]}", true).empty());
    CHECK(DeepSeekClient::formatSearchResults("not json", false)
              .rfind("Error:", 0) == 0);
    CHECK(DeepSeekClient::formatSearchResults("{}", false).rfind("Error:", 0) == 0);
}

TEST_CASE("fetch: title + markdown content") {
    nlohmann::json j = {{"title", "Example article"}, {"content", "# Head\n\nBody."}};
    const std::string out = DeepSeekClient::formatFetchPage(j.dump());
    CHECK(out == "# Example article\n\n# Head\n\nBody.");
}

TEST_CASE("fetch: empty title keeps the markdown, empty content is an error") {
    nlohmann::json noTitle = {{"title", ""}, {"content", "Body."}};
    CHECK(DeepSeekClient::formatFetchPage(noTitle.dump()) == "Body.");

    nlohmann::json noContent = {{"title", "T"}, {"content", ""}};
    CHECK(DeepSeekClient::formatFetchPage(noContent.dump()).rfind("Error:", 0) == 0);

    CHECK(DeepSeekClient::formatFetchPage("<<<").rfind("Error:", 0) == 0);
}

TEST_CASE("fetch: content is truncated to 100KB") {
    std::string big(120 * 1024, 'x');
    nlohmann::json j = {{"title", "T"}, {"content", big}};
    const std::string out = DeepSeekClient::formatFetchPage(j.dump());
    CHECK(out.size() < big.size());
    CHECK(out.find("truncated to 100KB") != std::string::npos);
}

TEST_CASE("errors: OpenAI-compatible body -> readable text, no retry for 4xx") {
    const std::string body = R"({"error":{"message":"'count' must be between 1 and 50","type":"invalid_request_error","param":null,"code":"invalid_request_error"}})";

    const std::string e400 = DeepSeekClient::formatApiError(400, body);
    CHECK(e400.rfind("Error: HTTP 400", 0) == 0);
    CHECK(e400.find("'count' must be between 1 and 50") != std::string::npos);

    CHECK(DeepSeekClient::formatApiError(402, body).find("insufficient balance") != std::string::npos);
    CHECK(DeepSeekClient::formatApiError(403, body).find("security policy") != std::string::npos);
    CHECK(DeepSeekClient::formatApiError(404, body).find("no usable page content") != std::string::npos);
    CHECK(DeepSeekClient::formatApiError(413, body).find("too large") != std::string::npos);

    // Non-JSON bodies still yield a usable message.
    const std::string raw = DeepSeekClient::formatApiError(500, "upstream exploded");
    CHECK(raw.rfind("Error: HTTP 500", 0) == 0);
    CHECK(raw.find("upstream exploded") != std::string::npos);
}
