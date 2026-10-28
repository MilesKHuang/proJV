// Token estimation: the official DeepSeek calibration is roughly 0.3 token per
// ASCII character and 0.6 token per CJK character. The previous byte-based
// formula charged a Chinese character 2 (bytes) * 2 / 4 = 1.5 tokens, i.e. about
// 2.5x too much, so Chinese sessions compacted long before the 1M window was used.
#include "doctest.h"

#include "core/session.h"
#include "core/token_estimate.h"

namespace {

// The estimate must land within +-20% of the official figure.
void checkWithin20Percent(size_t got, double expected) {
    const double g = static_cast<double>(got);
    CHECK(g >= expected * 0.8);
    CHECK(g <= expected * 1.2 + 0.5); // +0.5 absorbs the upward rounding
}

} // namespace

TEST_CASE("token estimate: ASCII text follows the 0.3 token/char rule") {
    const std::string s = "abcdefghijklmnopqrst"; // 20 ASCII characters
    checkWithin20Percent(estimateTokensForText(s), 20 * 0.3);
}

TEST_CASE("token estimate: CJK text follows the 0.6 token/char rule") {
    const std::string s = "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6" // 中文字符
                          "\xe6\xb5\x8b\xe8\xaf\x95";                         // 测试
    // 6 Chinese characters, 18 bytes (3 bytes each in UTF-8).
    CHECK(s.size() == 18);
    checkWithin20Percent(estimateTokensForText(s), 6 * 0.6);
}

TEST_CASE("token estimate: mixed text sums both rules") {
    const std::string s = "hello \xe4\xb8\x96\xe7\x95\x8c"; // "hello " + 世界
    checkWithin20Percent(estimateTokensForText(s), 6 * 0.3 + 2 * 0.6);
}

TEST_CASE("token estimate: Chinese is no longer charged per UTF-8 byte") {
    const std::string s = "\xe4\xb8\xad\xe6\x96\x87"; // 中文: 2 chars, 6 bytes
    const size_t oldFormula = std::max<size_t>(1, (6 * 2) / 4);
    const size_t now = estimateTokensForText(s);
    CHECK(now < oldFormula);
    CHECK(now * 3 <= oldFormula * 2); // at least ~1.5x cheaper than before
}

TEST_CASE("token estimate: edges") {
    CHECK(estimateTokensForText("") == 0);
    CHECK(estimateTokensForText("a") == 1);                    // never 0 for real text
    CHECK(estimateTokensForText("\xe4\xb8\xad") == 1);         // single 中
    CHECK(estimateTokensForText("\xf0\x9f\x99\x82") == 1);     // single emoji
}

TEST_CASE("token estimate: Session shares the same implementation") {
    const std::string samples[] = {"plain english text", "\xe4\xb8\xad\xe6\x96\x87 mixed",
                                   ""};
    for (const auto& s : samples)
        CHECK(Session::estimateTokens(s) == estimateTokensForText(s));
}
