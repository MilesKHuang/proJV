// proJV -- unified diff -> aligned rows (design 08.5, D5).
//
// The session database stores ONE unified diff per file-writing tool call. That
// string already carries everything a two-column view needs: the "@@ -a,b +c,d @@"
// header holds both line numbers, and the context lines are the unchanged text. So
// this is a pure re-projection of the stored string -- no schema change, no extra
// capture, and old sessions replay in the new layout for free.
//
// Why it lives here: the TUI draws the rows with FTXUI (tui/diff_view.cpp) and the
// studio draws them with ImGui (studio/studio_ui.cpp). Both must pair the k-th
// removed line with the k-th added line, trim context the same way and hit the
// same row cap, so the pairing rule is shared instead of written twice. This file
// has no FTXUI and no ImGui type.
#pragma once

#include <string>
#include <vector>

namespace diff_rows {

// Below this many usable columns two panes are narrower than any real source
// line, so the caller keeps the single-column block instead of splitting.
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

// Split on '\n', dropping a trailing '\r'. An empty input yields no lines.
std::vector<std::string> splitLines(const std::string& text);

// Decimal digit count, for the gutter width (1 for 0..9).
int digitCount(int n);

// Parse a unified diff into aligned rows: context trimmed per hunk, output
// capped at `maxRows`. `outTotalRows` (optional) receives the row count before
// the cap so the caller can say how much was cut. Never throws; a line that is
// not patch syntax is kept as a header row rather than dropped.
std::vector<Row> alignRows(const std::string& unified, int contextLines = kContextLines,
                           int maxRows = 40, int* outTotalRows = nullptr);

} // namespace diff_rows
