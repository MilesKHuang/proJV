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

    // Handle mouse + Ctrl+C for everything drawn this frame. Call once per frame,
    // after all blocks, so hit boxes are complete. `interactive` false while a modal
    // owns the input: the boxes are still cleared (they are per frame) but nothing is
    // selected or highlighted, because the highlight is drawn on the foreground list
    // and would sit ON TOP of the dialog.
    void handleInput(bool interactive = true);

    // The rectangle the chat area occupies this frame. The highlight is painted on the
    // foreground draw list (a child window's text would otherwise cover it), so it
    // must be clipped to the chat and nothing else -- without this an over-long drag
    // paints across the composer and the dock.
    void setClip(float x0, float y0, float x1, float y1);

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
        // The character width this line was WRAPPED with, in pixels. Hit-testing, the
        // highlight and the clipboard all read it instead of asking ImGui for the
        // current font: handleInput() runs after the caller has popped the chat face,
        // so "the current font" there is the proportional UI face -- a different ruler
        // from the one buildLines measured with, which is why a click landed a few
        // characters away from the pointer. 0 means "no text on this line" (an HR or a
        // table placeholder), and every reader falls back to 8 px.
        float cw = 0.0f;
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
    // The chat area this frame, for the foreground highlight (see setClip). Kept as
    // four floats rather than an ImVec4 so an unset clip is a plain `false`.
    bool hasClip_ = false;
    float clipX0_ = 0.0f;
    float clipY0_ = 0.0f;
    float clipX1_ = 0.0f;
    float clipY1_ = 0.0f;
    bool dragging_ = false;
    int dragBlock_ = -1;
    bool haveDragOrigin_ = false;
    int originBlock_ = 0;
    int originLine_ = 0;
    int originCol_ = 0;

    const Cached* cachedLayout(int blockId, const std::string& text, bool markdown, float width);
    // x,y -> (block, line, column) under the cursor, or false when it misses text.
    bool hitTest(float x, float y, int* block, int* line, int* col) const;
    // The nearest line to a pointer that missed every hit box (a drag through the gap
    // between two bubbles), with its first column, its last, or the one under x.
    bool snapToNearest(float x, float y, int* block, int* line, int* col) const;
    static int columnAtX(const VisualLine& line, float x);
    static std::string textInLine(const VisualLine& line, int c0, int c1);
};

} // namespace studio
