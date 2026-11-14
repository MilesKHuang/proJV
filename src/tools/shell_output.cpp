// proJV -- output shaping for exec_shell (see shell_output.h).
#include "shell_output.h"

#include <algorithm>
#include <cstdio>

namespace shell_output {
namespace {

// Number of '\n' in [0, pos). A trailing newline does not start a new line.
size_t countLines(const std::string& s, size_t upTo) {
    size_t n = 0;
    for (size_t i = 0; i < upTo && i < s.size(); ++i) {
        if (s[i] == '\n') ++n;
    }
    return n;
}

bool isContinuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

std::string humanBytes(size_t bytes) {
    char buf[64];
    if (bytes >= 1024u * 1024u)
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024u)
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%zu bytes", bytes);
    return buf;
}

} // namespace

bool keepTailBytes(std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return false;
    size_t pos = s.size() - maxBytes;
    while (pos < s.size() && isContinuation(s[pos]))
        ++pos;
    s.erase(0, pos);
    return true;
}

void keepTailWithMiddleNotice(std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return;
    const size_t dropped = s.size() - maxBytes;
    // A quarter of the budget for the head: the first error line is often the
    // only place that names the failing file.
    const size_t headBytes = maxBytes / 4;
    size_t headEnd = headBytes;
    while (headEnd > 0 && headEnd < s.size() && isContinuation(s[headEnd]))
        --headEnd;
    std::string tail = s.substr(s.size() - (maxBytes - headBytes));
    // Do not start the tail in the middle of a UTF-8 sequence either.
    size_t skip = 0;
    while (skip < tail.size() && isContinuation(tail[skip]))
        ++skip;
    const std::string notice = "\n[... " + humanBytes(dropped) +
                               " dropped from the middle: the head and the tail are kept ...]\n";
    s = s.substr(0, headEnd) + notice + tail.substr(skip);
}

Applied apply(const std::string& raw, int tailLines, bool fullOutput) {
    Applied out;
    out.text = raw;
    out.totalBytes = raw.size();

    if (fullOutput) {
        if (raw.size() <= kFullOutputMaxBytes) return out;
        out.truncated = true;
        std::string s = raw;
        keepTailBytes(s, kFullOutputMaxBytes);
        out.text = "[output truncated: kept " + humanBytes(kFullOutputMaxBytes) + " of " +
                   humanBytes(raw.size()) + "]\n" +
                   "Use a narrower command: append `| grep -i error`, `| tail -n 500`, "
                   "or `| sed -n '1200,1400p'` to select what you need.\n" +
                   s;
        return out;
    }

    int keep = tailLines;
    if (keep <= 0) keep = kDefaultTailLines;
    keep = std::max(kMinTailLines, std::min(kMaxTailLines, keep));
    const size_t keepN = static_cast<size_t>(keep);

    // A single '\n' terminates the last line; it does not open a new one.
    size_t contentBytes = raw.size();
    while (contentBytes > 0 && (raw[contentBytes - 1] == '\n' || raw[contentBytes - 1] == '\r'))
        --contentBytes;
    const size_t totalLines = countLines(raw, contentBytes) + (contentBytes > 0 ? 1 : 0);
    if (totalLines <= keepN) return out;

    // Walk back keepN lines from the end of the content.
    size_t pos = contentBytes;
    size_t seen = 0;
    while (pos > 0) {
        --pos;
        if (raw[pos] == '\n') {
            ++seen;
            if (seen == keepN) {
                ++pos;
                break;
            }
        }
    }
    // Do not start with a UTF-8 continuation byte.
    while (pos < raw.size() && isContinuation(raw[pos]))
        ++pos;

    out.truncated = true;
    out.droppedLines = totalLines - keepN;
    out.text = "[... " + std::to_string(out.droppedLines) +
               " earlier line(s) omitted; showing the last " + std::to_string(keepN) + " of " +
               std::to_string(totalLines) + " lines. Pass tail_lines (up to " +
               std::to_string(kMaxTailLines) + ") or full_output: true to widen the window ...]\n" +
               raw.substr(pos, contentBytes - pos);
    return out;
}

} // namespace shell_output
