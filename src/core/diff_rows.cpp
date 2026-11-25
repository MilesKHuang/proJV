// proJV -- unified diff -> aligned rows (see diff_rows.h).
//
// Moved verbatim out of tui/diff_view.cpp: the pairing rule, the context trim and
// the row cap. Rendering stayed behind.
#include "diff_rows.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace diff_rows {
namespace {

bool readInt(const std::string& s, size_t& i, int& out) {
    while (i < s.size() && s[i] == ' ')
        ++i;
    const size_t begin = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        ++i;
    if (i == begin) return false;
    out = std::stoi(s.substr(begin, i - begin));
    return true;
}

// "@@ -12,5 +12,6 @@" -> oldStart 12, newStart 12. A missing ",count" is legal
// unified-diff syntax, so the count is parsed but not required.
bool parseHunkHeader(const std::string& line, int& oldStart, int& newStart) {
    const size_t minus = line.find('-');
    if (minus == std::string::npos) return false;
    size_t i = minus + 1;
    int oldCount = 0;
    if (!readInt(line, i, oldStart)) return false;
    if (i < line.size() && line[i] == ',') {
        ++i;
        if (!readInt(line, i, oldCount)) return false;
    }
    const size_t plus = line.find('+', i);
    if (plus == std::string::npos) return false;
    i = plus + 1;
    int newCount = 0;
    if (!readInt(line, i, newStart)) return false;
    if (i < line.size() && line[i] == ',') {
        ++i;
        if (!readInt(line, i, newCount)) return false;
    }
    return true;
}

bool isContextRow(const Row& r) {
    return !r.isHeader && !r.oldIsChange && !r.newIsChange;
}

// Emit the pending run of removals and additions as paired rows.
void flushRun(std::vector<Row>& out, std::vector<std::string>& dels, std::vector<std::string>& adds,
              int& oldNo, int& newNo) {
    const size_t pairs = std::max(dels.size(), adds.size());
    for (size_t k = 0; k < pairs; ++k) {
        Row r;
        if (k < dels.size()) {
            r.oldNo = oldNo++;
            r.oldText = std::move(dels[k]);
            r.oldIsChange = true;
        }
        if (k < adds.size()) {
            r.newNo = newNo++;
            r.newText = std::move(adds[k]);
            r.newIsChange = true;
        }
        out.push_back(std::move(r));
    }
    dels.clear();
    adds.clear();
}

// Drop the outermost context rows of one hunk down to `keep` per side.
void trimContext(std::vector<Row>& rows, int keep) {
    if (keep < 0) keep = 0;
    size_t lead = 0;
    while (lead < rows.size() && isContextRow(rows[lead]))
        ++lead;
    size_t trail = 0;
    while (trail < rows.size() - lead && isContextRow(rows[rows.size() - 1 - trail]))
        ++trail;
    const size_t dropLead = lead > static_cast<size_t>(keep) ? lead - keep : 0;
    const size_t dropTrail = trail > static_cast<size_t>(keep) ? trail - keep : 0;
    if (dropTrail > 0) rows.erase(rows.end() - dropTrail, rows.end());
    if (dropLead > 0) rows.erase(rows.begin(), rows.begin() + dropLead);
}

} // namespace

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

int digitCount(int n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        ++d;
    }
    return d;
}

std::vector<Row> alignRows(const std::string& unified, int contextLines, int maxRows,
                           int* outTotalRows) {
    std::vector<Row> rows;
    const std::vector<std::string> lines = splitLines(unified);
    size_t i = 0;
    int oldNo = 0;
    int newNo = 0;

    while (i < lines.size()) {
        const std::string& line = lines[i];

        // File headers carry no line numbers and are noise in a two-column view.
        if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0) {
            ++i;
            continue;
        }

        if (line.rfind("@@", 0) == 0) {
            Row header;
            header.isHeader = true;
            header.headerText = line;
            int oldStart = 0;
            int newStart = 0;
            const bool parsed = parseHunkHeader(line, oldStart, newStart);
            rows.push_back(std::move(header));
            ++i;
            // An unparseable header leaves the numbers unknown: the body still
            // shows, with blank gutters.
            oldNo = parsed ? oldStart : 0;
            newNo = parsed ? newStart : 0;

            std::vector<Row> hunk;
            std::vector<std::string> dels;
            std::vector<std::string> adds;
            while (i < lines.size()) {
                const std::string& body = lines[i];
                const char tag = body.empty() ? ' ' : body[0];
                if (tag != ' ' && tag != '-' && tag != '+') break;
                if (tag == ' ') {
                    flushRun(hunk, dels, adds, oldNo, newNo);
                    Row ctx;
                    if (!body.empty()) ctx.oldText = body.substr(1);
                    if (oldNo > 0) ctx.oldNo = oldNo++;
                    if (newNo > 0) ctx.newNo = newNo++;
                    ctx.newText = ctx.oldText;
                    hunk.push_back(std::move(ctx));
                } else if (tag == '-') {
                    dels.push_back(body.substr(1));
                } else {
                    adds.push_back(body.substr(1));
                }
                ++i;
            }
            flushRun(hunk, dels, adds, oldNo, newNo);
            trimContext(hunk, contextLines);
            for (auto& row : hunk)
                rows.push_back(std::move(row));
            continue;
        }

        // The truncation marker (and anything else that is not patch syntax) is
        // shown full width so nothing disappears silently.
        Row plain;
        plain.isHeader = true;
        plain.headerText = line;
        rows.push_back(std::move(plain));
        ++i;
    }

    const int total = static_cast<int>(rows.size());
    if (outTotalRows) *outTotalRows = total;
    if (maxRows > 0 && total > maxRows) rows.resize(static_cast<size_t>(maxRows));
    return rows;
}

} // namespace diff_rows
