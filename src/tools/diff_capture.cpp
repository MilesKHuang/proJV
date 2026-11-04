#include "diff_capture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace diff_capture {
namespace {

// Above this the O(n*m) table stops being worth it (16 MB at the cap) and the
// middle section is emitted as one replacement hunk instead.
constexpr size_t kMaxDpCells = 4'000'000;

// One line of the emitted patch: tag is ' ', '-' or '+'.
struct Op {
    char tag;
    std::string text;
};

// Split on '\n' and drop a trailing '\r' from every line. Without the CR strip a
// Windows file (CRLF on disk) compared against model text (LF) reports every
// line as changed. Empty input yields no lines at all.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        const size_t nl = text.find('\n', start);
        const size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

bool looksBinary(const std::string& s) {
    return s.find('\0') != std::string::npos;
}

// Classic LCS over the two middle sections. The caller has already bounded
// size(), so the table stays small.
std::vector<Op> lcsOps(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    const size_t stride = m + 1;
    std::vector<uint32_t> dp((n + 1) * stride, 0);
    auto at = [&](size_t i, size_t j) -> uint32_t& { return dp[i * stride + j]; };
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            at(i, j) = (a[i] == b[j]) ? at(i + 1, j + 1) + 1
                                      : std::max(at(i + 1, j), at(i, j + 1));
        }
    }
    std::vector<Op> ops;
    size_t i = 0;
    size_t j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            ops.push_back({' ', a[i]});
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ops.push_back({'-', a[i]});
            ++i;
        } else {
            ops.push_back({'+', b[j]});
            ++j;
        }
    }
    for (; i < n; ++i) ops.push_back({'-', a[i]});
    for (; j < m; ++j) ops.push_back({'+', b[j]});
    return ops;
}

// One slot per thread, shared by record/take/reset. A function-local thread_local
// is what gives every SubAgent worker its own copy.
Entry& slot() {
    thread_local Entry e;
    return e;
}

} // namespace

bool readFileIfExists(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    out.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return true;
}

std::string displayPath(const std::string& resolved, const std::string& workspace) {
    std::string p = resolved;
    if (!workspace.empty()) {
        std::error_code ec;
        // weakly_canonical underneath, so a file that write_file is about to
        // create still resolves.
        const fs::path rel = fs::relative(fs::path(resolved), fs::path(workspace), ec);
        const std::string native = rel.generic_string();
        if (!ec && !native.empty() && native.rfind("..", 0) != 0) p = native;
    }
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

std::string makeUnifiedDiff(const std::string& before, const std::string& after,
                            const std::string& path, int contextLines, int maxLines,
                            std::size_t maxChars) {
    if (before == after) return std::string();
    if (looksBinary(before) || looksBinary(after)) return std::string();

    const std::vector<std::string> a = splitLines(before);
    const std::vector<std::string> b = splitLines(after);

    // Trim the common head and tail first: an edit normally touches a handful of
    // lines, and this keeps both the table and the emitted patch small.
    const size_t minSize = std::min(a.size(), b.size());
    size_t head = 0;
    while (head < minSize && a[head] == b[head]) ++head;
    size_t tail = 0;
    while (tail < minSize - head && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) ++tail;

    const std::vector<std::string> midA(a.begin() + head, a.end() - tail);
    const std::vector<std::string> midB(b.begin() + head, b.end() - tail);
    if (midA.empty() && midB.empty()) return std::string();

    std::vector<Op> ops;
    if (midA.size() * midB.size() > kMaxDpCells) {
        for (const auto& line : midA) ops.push_back({'-', line});
        for (const auto& line : midB) ops.push_back({'+', line});
    } else {
        ops = lcsOps(midA, midB);
    }

    const size_t ctx = contextLines > 0 ? static_cast<size_t>(contextLines) : 0;
    const size_t leadCtx = std::min(ctx, head);
    const size_t trailCtx = std::min(ctx, tail);
    const long long oldStart = static_cast<long long>(head - leadCtx) + 1;
    const long long oldCount = static_cast<long long>(leadCtx + midA.size() + trailCtx);
    const long long newCount = static_cast<long long>(leadCtx + midB.size() + trailCtx);

    std::vector<std::string> lines;
    lines.push_back("--- a/" + path);
    lines.push_back("+++ b/" + path);
    char header[128];
    // Unified-diff convention: a side with no lines at all starts at 0, so a new
    // file reads "@@ -0,0 +1,N @@" and a deleted one "@@ -1,N +0,0 @@".
    const long long oldHunkStart = oldCount == 0 ? oldStart - 1 : oldStart;
    const long long newHunkStart = newCount == 0 ? oldStart - 1 : oldStart;
    std::snprintf(header, sizeof(header), "@@ -%lld,%lld +%lld,%lld @@", oldHunkStart, oldCount,
                  newHunkStart, newCount);
    lines.push_back(header);

    for (size_t k = head - leadCtx; k < head; ++k) lines.push_back(" " + a[k]);
    for (const auto& op : ops) lines.push_back(std::string(1, op.tag) + op.text);
    for (size_t k = a.size() - tail; k < a.size() - tail + trailCtx; ++k)
        lines.push_back(" " + a[k]);

    // Two independent caps. The line cap keeps the bubble readable; the char cap
    // stops one 5k-character minified line from blowing up the layout.
    size_t keep = lines.size();
    if (maxLines > 0 && keep > static_cast<size_t>(maxLines))
        keep = static_cast<size_t>(maxLines);
    size_t chars = 0;
    size_t fits = 0;
    while (fits < keep) {
        const size_t next = chars + lines[fits].size() + 1;
        if (maxChars > 0 && next > maxChars) break;
        chars = next;
        ++fits;
    }
    keep = std::min(keep, fits);

    std::string text;
    text.reserve(chars + 64);
    for (size_t k = 0; k < keep; ++k) {
        text += lines[k];
        text += '\n';
    }
    if (keep < lines.size()) {
        // Starts with '.', so the renderer colours it as an ordinary context line
        // and needs no special case.
        text += "... diff truncated (" + std::to_string(lines.size() - keep) + " more lines)\n";
    }
    return text;
}

void record(const std::string& path, const std::string& before, const std::string& after) {
    std::string unified = makeUnifiedDiff(before, after, path);
    if (unified.empty()) return; // nothing to show: no entry, not an empty one
    Entry& e = slot();
    e.path = path;
    e.unified = std::move(unified);
}

Entry take() {
    Entry& e = slot();
    Entry out;
    out.path = std::move(e.path);
    out.unified = std::move(e.unified);
    e = Entry{};
    return out;
}

void reset() {
    slot() = Entry{};
}

} // namespace diff_capture
