#include "diff_view.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace diff_view {
namespace {

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

bool readInt(const std::string& s, size_t& i, int& out) {
    while (i < s.size() && s[i] == ' ') ++i;
    const size_t begin = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
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
void flushRun(std::vector<Row>& out, std::vector<std::string>& dels,
              std::vector<std::string>& adds, int& oldNo, int& newNo) {
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
    while (lead < rows.size() && isContextRow(rows[lead])) ++lead;
    size_t trail = 0;
    while (trail < rows.size() - lead && isContextRow(rows[rows.size() - 1 - trail])) ++trail;
    const size_t dropLead = lead > static_cast<size_t>(keep) ? lead - keep : 0;
    const size_t dropTrail = trail > static_cast<size_t>(keep) ? trail - keep : 0;
    if (dropTrail > 0) rows.erase(rows.end() - dropTrail, rows.end());
    if (dropLead > 0) rows.erase(rows.begin(), rows.begin() + dropLead);
}

ftxui::Color colorForLine(const std::string& line, const Colors& c) {
    if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0) return c.hunk;
    if (!line.empty()) {
        if (line[0] == '@') return c.hunk;
        if (line[0] == '+') return c.added;
        if (line[0] == '-') return c.removed;
    }
    return c.text;
}

ftxui::Element gutterCell(int number, int width, ftxui::Color color) {
    std::string s = (number > 0) ? std::to_string(number) : std::string();
    if (static_cast<int>(s.size()) < width) s = std::string(width - s.size(), ' ') + s;
    return ftxui::text(s) | ftxui::color(color);
}

ftxui::Element title(const Colors& c) {
    return ftxui::text("── Diff ──") | ftxui::bold | ftxui::color(c.title);
}

} // namespace

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
            for (auto& row : hunk) rows.push_back(std::move(row));
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

ftxui::Element renderUnified(const std::string& unified, const Colors& colors, int maxRows) {
    const std::vector<std::string> lines = splitLines(unified);
    ftxui::Elements els;
    els.push_back(title(colors));
    const int total = static_cast<int>(lines.size());
    const int keep = (maxRows > 0 && total > maxRows) ? maxRows : total;
    for (int i = 0; i < keep; ++i) {
        const std::string& line = lines[static_cast<size_t>(i)];
        els.push_back(ftxui::text(line) | ftxui::color(colorForLine(line, colors)));
    }
    if (total > keep) {
        els.push_back(ftxui::text("... (" + std::to_string(total - keep) +
                                 " more lines, full diff in session db)") |
                      ftxui::dim);
    }
    return ftxui::vbox(std::move(els));
}

ftxui::Element renderSplit(const std::string& unified, const Colors& colors, int width,
                           int maxRows) {
    int total = 0;
    const std::vector<Row> rows = alignRows(unified, kContextLines, maxRows, &total);

    int maxNumber = 0;
    for (const auto& r : rows) {
        maxNumber = std::max(maxNumber, std::max(r.oldNo, r.newNo));
    }
    const int gutterWidth = std::max(3, digitCount(maxNumber));
    // gutter + " " + pane + " | " + gutter + " " + pane == width
    const int paneWidth = (width - 2 * gutterWidth - 5) / 2;
    if (paneWidth < 12) return renderUnified(unified, colors, maxRows);

    ftxui::Elements els;
    els.push_back(title(colors));
    for (const auto& r : rows) {
        if (r.isHeader) {
            els.push_back(ftxui::text(r.headerText) | ftxui::color(colors.hunk));
            continue;
        }
        // The marker stays inside the pane so a colour-blind theme still shows
        // which side changed.
        const std::string leftCell = (r.oldIsChange ? "-" : " ") + r.oldText;
        const std::string rightCell = (r.newIsChange ? "+" : " ") + r.newText;
        els.push_back(ftxui::hbox({
            gutterCell(r.oldNo, gutterWidth, colors.gutter),
            ftxui::text(" "),
            ftxui::text(leftCell) | ftxui::color(r.oldIsChange ? colors.removed : colors.text) |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, paneWidth),
            ftxui::text(" │ ") | ftxui::color(colors.gutter),
            gutterCell(r.newNo, gutterWidth, colors.gutter),
            ftxui::text(" "),
            ftxui::text(rightCell) | ftxui::color(r.newIsChange ? colors.added : colors.text) |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, paneWidth),
        }));
    }
    if (total > static_cast<int>(rows.size())) {
        els.push_back(ftxui::text("... (" + std::to_string(total - rows.size()) +
                                 " more rows, full diff in session db)") |
                      ftxui::dim);
    }
    return ftxui::vbox(std::move(els));
}

} // namespace diff_view
