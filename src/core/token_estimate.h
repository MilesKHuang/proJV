// proJV -- token estimation, shared by Session (context pressure) and Storage
// (the messages.token_count column). One implementation, so the two can never
// drift apart again.
//
// Calibration: DeepSeek's tokenizer yields roughly 0.3 tokens per ASCII
// character and 0.6 tokens per CJK character. Counting CHARACTERS (UTF-8 lead
// bytes) instead of raw bytes keeps Chinese close to reality; the old
// byte-based version overestimated it by roughly 2.5x, which made Chinese
// sessions compact far earlier than the 1M window allows.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

inline size_t estimateTokensForText(const std::string& text) {
    if (text.empty()) return 0;
    size_t ascii = 0; // characters in the ASCII range
    size_t wide = 0;  // characters outside it (CJK, emoji, accented Latin)
    for (unsigned char c : text) {
        if ((c & 0xC0) == 0x80) continue; // UTF-8 continuation byte: same character
        if (c < 0x80)
            ++ascii;
        else
            ++wide;
    }
    // 3/10 token per ASCII char, 6/10 per non-ASCII char, rounded up.
    return std::max<size_t>(1, (ascii * 3 + wide * 6 + 9) / 10);
}
