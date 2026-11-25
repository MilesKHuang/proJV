// proJV TUI -- diff bubble rendering: a VSCode-style side-by-side patch.
//
// The alignment rule (unified diff -> paired rows, context trimming, the row cap)
// moved to core/diff_rows.h in design 08.5: the studio draws the same rows with
// ImGui, so the rule cannot live in one front-end. What stays here is the FTXUI
// rendering, plus forwarding names so existing callers are untouched.
#pragma once

#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

#include "core/diff_rows.h"

namespace diff_view {

// Below this many usable columns two panes are narrower than any real source
// line, so the caller keeps the unified block instead of splitting.
constexpr int kSplitMinWidth = diff_rows::kSplitMinWidth;

// Unchanged lines kept above and below a change.
constexpr int kContextLines = diff_rows::kContextLines;

// One aligned display row (definition in core/diff_rows.h).
using Row = diff_rows::Row;

// Palette handed in by chat_view, which owns the theme mapping.
struct Colors {
    ftxui::Color added;
    ftxui::Color removed;
    ftxui::Color hunk;
    ftxui::Color text;
    ftxui::Color gutter;
    ftxui::Color title;
};

// Forwarder: the implementation lives in core/diff_rows.cpp so both front-ends
// pair and trim rows identically.
inline std::vector<Row> alignRows(const std::string& unified, int contextLines = kContextLines,
                                  int maxRows = 40, int* outTotalRows = nullptr) {
    return diff_rows::alignRows(unified, contextLines, maxRows, outTotalRows);
}

// Two panes, two gutters, changes paired left-to-right. Falls back to
// renderUnified() when the panes would be too narrow to read.
ftxui::Element renderSplit(const std::string& unified, const Colors& colors, int width,
                           int maxRows = 40);

// The single-column block: one line per patch line, coloured by its marker.
ftxui::Element renderUnified(const std::string& unified, const Colors& colors, int maxRows = 40);

} // namespace diff_view
