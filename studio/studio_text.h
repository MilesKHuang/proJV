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
// Hit-testing reads the offsets the layout MEASURED (see VisualLine::charX), not a
// guessed "columns x one character width": a monospace Latin face and a merged CJK
// face do not share a cell size, so the guess drifted over Chinese. One ruler, the
// one the glyphs were drawn with, for the click, the highlight and the clipboard.
#pragma once

#include <string>
#include <vector>

#include "design/text_runs.h"
#include "design/text_select.h"
#include "imgui.h"
#include "tui/markdown_text.h"

namespace studio {

// Per-frame state: one drag in progress, the hit boxes of the last frame.
class TextSurface {
  public:
    // Draw one block of text (markdown or plain) at the current cursor, wrapped to
    // `width`. `blockId` must be stable across frames (the chat row index).
    void drawBlock(int blockId, const std::string& text, bool markdown, float width,
                   ImU32 plainColor);

    // Start a draw pass: forget last frame's hit boxes. Call ONCE before the frame's
    // drawBlock calls (drawChat does), so the lines collected are exactly this frame's.
    void beginDraw();

    // Handle mouse + Ctrl+C for everything drawn this frame. Call once per frame,
    // after all blocks, so hit boxes are complete. `interactive` false while a modal
    // owns the input: the lines are still collected as this frame's truth (so a copy
    // made with the keyboard elsewhere is not stale) but nothing is selected or
    // highlighted, because the highlight is painted on the foreground list and would
    // sit ON TOP of the dialog. `acceptClick` false on the frame a chat row was
    // toggled: the same press must not also start a selection.
    void handleInput(bool interactive = true, bool acceptClick = true);

    // The rectangle the chat area occupies this frame. The highlight is painted on the
    // foreground draw list (a child window's text would otherwise cover it), so it
    // must be clipped to the chat and nothing else -- without this an over-long drag
    // paints across the composer and the dock.
    void setClip(float x0, float y0, float x1, float y1);

    // Selection bookkeeping.
    Selection& selection() { return sel_; }

    // What was drawn on the last beginDraw..handleInput pass, in visual order, with
    // each line's geometry and measured per-character offsets. Read-only; used by the
    // headless copy probe (tests/copy_selftest.cpp) so the drag -> highlight -> copy
    // path can be checked without a GPU, a key or a model.
    struct LineInfo {
        int block = 0;
        int line = 0;
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        bool soft = false;
        std::string text;
        std::vector<float> charX;
    };
    std::vector<LineInfo> laidOutLines() const;

    // Text the selection covers (plain, no styling). Empty when nothing is selected.
    std::string selectedText() const;

  private:
    struct VisualLine {
        // Runs keep their style so drawing stays coloured; `plain` is the same text
        // flattened, used for hit-testing and for the clipboard.
        std::vector<markdown_text::Segment> runs;
        std::string plain;
        // Where every character starts, in pixels from this line's left edge: one
        // entry per code point, plus a trailing entry for the line's total width.
        // Measured once, at layout time, with the font that will draw it, so the
        // click, the highlight and the clipboard all read ONE ruler. A CJK glyph is
        // its own font-wide box here, not "two Latin cells" -- that assumption is
        // what made a drag over Chinese drift away from the pointer.
        std::vector<float> charX;
        // This visual line continues the previous one (a soft wrap): the clipboard
        // joins them with NO separator, because the space the break dropped is still
        // at the end of the previous line (studio_md::wrap keeps it there).
        bool soft = false;
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

    // Lines drawn this frame, in visual order, with the pixel offset of every
    // character (see VisualLine::charX). Kept until the next beginDraw() rather than
    // cleared in handleInput: a selection lives across frames (a drag, then Ctrl+C
    // later), and the click, the highlight and the clipboard all read THIS one list,
    // so what is copied is exactly what is highlighted and on screen. Copying from
    // the layout cache instead (append-only) is what leaked an old streamed block into
    // the clipboard with no highlight to match it.
    //
    // Consequence, deliberate: only what was drawn this frame can be copied. A folded
    // tool row is not drawn, so a selection that spans it skips its body -- the reader
    // sees a highlight on what they will get.
    std::vector<VisualLine> lines_;
    std::vector<std::pair<int, Cached>> cache_;
    Selection sel_;
    // The chat area this frame, for the foreground highlight (see setClip). Kept as
    // four floats rather than an ImVec4 so an unset clip is a plain `false`.
    bool hasClip_ = false;
    float clipX0_ = 0.0f;
    float clipY0_ = 0.0f;
    float clipX1_ = 0.0f;
    float clipY1_ = 0.0f;
    bool dragging_ = false;

    const Cached* cachedLayout(int blockId, const std::string& text, bool markdown, float width);
    // x,y -> (block, line, column) under the cursor, or false when it misses text.
    bool hitTest(float x, float y, int* block, int* line, int* col) const;
    // The nearest line to a pointer that missed every hit box (a drag through the gap
    // between two bubbles), with its first column, its last, or the one under x.
    bool snapToNearest(float x, float y, int* block, int* line, int* col) const;
    // The column under x on ONE line, read from the offsets the layout measured.
    static int columnAtX(const VisualLine& line, float x);
};

} // namespace studio
