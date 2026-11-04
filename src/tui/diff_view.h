// proJV TUI -- diff bubble rendering: a VSCode-style side-by-side patch.
//
// The session database stores one unified diff per file-writing tool call. That
// string already carries everything a two-column view needs: the "@@ -a,b +c,d @@"
// header holds both line numbers and the context lines are the unchanged text.
// So this module is a pure re-projection of the stored string -- no schema
// change, no extra capture, and old sessions replay in the new layout for free.
#pragma once

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace diff_view {

// Below this many usable columns two panes are narrower than any real source
// line, so the caller keeps the unified block instead of splitting.
constexpr int kSplitMinWidth = 100;

// Unchanged lines kept above and below a change. Two, not three: a split row
// carries twice the text, so the same vertical budget shows fewer rows.
constexpr int kContextLines = 2;

// One aligned display row. A change pairs the k-th removed line with the k-th
// added line, the way VSCode does; a side with no counterpart keeps 0 / empty.
struct Row {
    int oldNo = 0; // 0 = no line on this side
    int newNo = 0;
    std::string oldText;
    std::string newText;
    bool oldIsChange = false;
    bool newIsChange = false;
    bool isHeader = false; // "@@ ... @@" or free text: rendered across both panes
    std::string headerText;
};

// Palette handed in by chat_view, which owns the theme mapping.
struct Colors {
    ftxui::Color added;
    ftxui::Color removed;
    ftxui::Color hunk;
    ftxui::Color text;
    ftxui::Color gutter;
    ftxui::Color title;
};

// Parse a unified diff into aligned rows: context trimmed per hunk, output
// capped at `maxRows`. `outTotalRows` (optional) receives the row count before
// the cap so the caller can say how much was cut. Never throws; a line that is
// not patch syntax is kept as a header row rather than dropped.
std::vector<Row> alignRows(const std::string& unified, int contextLines = kContextLines,
                           int maxRows = 40, int* outTotalRows = nullptr);

// Two panes, two gutters, changes paired left-to-right. Falls back to
// renderUnified() when the panes would be too narrow to read.
ftxui::Element renderSplit(const std::string& unified, const Colors& colors, int width,
                           int maxRows = 40);

// The single-column block: one line per patch line, coloured by its marker.
ftxui::Element renderUnified(const std::string& unified, const Colors& colors, int maxRows = 40);

} // namespace diff_view
