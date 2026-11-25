// proJV studio -- selectable, cached text layout.
//
// Two problems this solves, both of which made the window feel like a terminal
// screenshot rather than an application:
//
//   1. Text could not be selected. A GUI user drags across a message and copies it.
//      ImGui's TextUnformatted cannot do that, so this module lays text out itself,
//      records where every visual line landed, and owns one selection (anchor +
//      caret) with drag, double-click-word and Ctrl+C.
//   2. Every frame re-parsed and re-wrapped every message. On a long conversation
//      that is quadratic and makes the window stop responding -- the "Esc does
//      nothing while a skill runs" report. Layout is cached per (block, width,
//      content hash) and rebuilt only when the text or the width changes.
//
// Monospace is what makes the hit-testing exact: a column is a multiple of the
// character width, so x -> column is arithmetic, not a search.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"
#include "tui/markdown_text.h"

namespace studio {

// One selection over the whole chat view. Blocks are the chat rows, lines are the
// visual lines of one block, columns are display columns (CJK counts as 2).
struct Selection {
    int blockA = -1;
    int lineA = 0;
    int colA = 0;
    int blockB = -1;
    int lineB = 0;
    int colB = 0;
    bool active = false;

    bool empty() const { return blockA < 0 || blockB < 0; }
    void clear() {
        blockA = blockB = -1;
        lineA = lineB = colA = colB = 0;
        active = false;
    }
    // Normalised order: the earlier end first.
    void ordered(int* blockA_, int* lineA_, int* colA_, int* blockB_, int* lineB_,
                 int* colB_) const;
};

// Per-frame state: one drag in progress, the hit boxes of the last frame.
class TextSurface {
  public:
    // Draw one block of text (markdown or plain) at the current cursor, wrapped to
    // `width`. `blockId` must be stable across frames (the chat row index).
    void drawBlock(int blockId, const std::string& text, bool markdown, float width,
                   ImU32 plainColor);

    // Handle mouse + Ctrl+C for everything drawn this frame. Call once per frame,
    // after all blocks, so hit boxes are complete.
    void handleInput();

    // Selection bookkeeping.
    Selection& selection() { return sel_; }

    // Text the selection covers (plain, no styling). Empty when nothing is selected.
    std::string selectedText() const;

  private:
    struct VisualLine {
        // Runs keep their style so drawing stays coloured; `plain` is the same text
        // flattened, used for hit-testing and for the clipboard.
        std::vector<markdown_text::Segment> runs;
        std::string plain;
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        int block = 0;
        int line = 0;
        // Heading tier: 0 = body, 1 = H3, 2 = H2, 3 = H1. A heading that is only a
        // different colour is invisible next to the body text (the TUI could afford
        // that: every line there is the same cell height anyway).
        int tier = 0;
        // Draw a thin rule under this line: the table header row, so the columns read
        // as a header and not as another data row.
        bool rule = false;
        // A markdown table is ONE line here: the drawer hands the parsed rows to
        // ImGui's table widget (the GUI's own equivalent of the ftxui::Table the TUI
        // uses) instead of padding text into columns. -1 = not a table.
        int tableIndex = -1;
        // Block decoration: a quote bar on the left, or a code-band background.
        bool quote = false;
        bool codeBg = false;
        // Pixels this line starts indented by (hanging indent of a wrapped list item).
        float indent = 0.0f;
    };
    // One parsed table: rows[0] is the header; `weights` is each column's natural
    // width in character cells, which becomes ImGui's proportional column width.
    struct TableData {
        std::vector<std::vector<std::string>> rows;
        std::vector<float> weights;
    };
    struct Cached {
        size_t hash = 0;
        float width = 0.0f;
        bool markdown = false;
        std::vector<VisualLine> lines;
        std::vector<TableData> tables;
    };

    // Markdown -> drawable lines (block model: indent, tier, decoration). Tables are
    // collected into `tables`; the line that stood in for one carries its index.
    static std::vector<VisualLine> buildLines(const std::string& text, bool markdown, float width,
                                              int blockId, std::vector<TableData>* tables);
    // ImGui::BeginTable with the theme's table colours: borders, header band, zebra.
    void drawTable(const TableData& t);

    std::vector<VisualLine> frameLines_; // hit boxes for THIS frame
    std::vector<std::pair<int, Cached>> cache_;
    Selection sel_;
    bool dragging_ = false;
    int dragBlock_ = -1;
    bool haveDragOrigin_ = false;
    int originBlock_ = 0;
    int originLine_ = 0;
    int originCol_ = 0;

    const Cached* cachedLayout(int blockId, const std::string& text, bool markdown, float width);
    // x,y -> (block, line, column) under the cursor, or false when it misses text.
    bool hitTest(float x, float y, int* block, int* line, int* col) const;
    static int columnAtX(const VisualLine& line, float x);
    static std::string textInLine(const VisualLine& line, int c0, int c1);
};

} // namespace studio
