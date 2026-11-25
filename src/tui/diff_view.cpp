// proJV TUI -- diff bubble rendering (see diff_view.h).
//
// The alignment rule lives in core/diff_rows.cpp (design 08.5, D5): the studio
// draws the same rows, so pairing/trimming/capping must not exist twice. This
// file only turns rows into ftxui::Elements.
#include "diff_view.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace diff_view {
namespace {

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

ftxui::Element renderUnified(const std::string& unified, const Colors& colors, int maxRows) {
    const std::vector<std::string> lines = diff_rows::splitLines(unified);
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
    const int gutterWidth = std::max(3, diff_rows::digitCount(maxNumber));
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
