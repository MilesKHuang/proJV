// proJV studio -- what a text selection covers: the one ordering rule the chat
// surface, the highlight and the clipboard all have to agree on.
//
// Why this is a separate, ImGui-free file. The surface that draws the text cannot be
// unit-tested -- it needs a font, a draw list and a frame -- but the arithmetic that
// decides WHICH characters a drag selected can. A wrong order there is a wrong
// clipboard, and it is exactly the kind of bug a screenshot never shows: the
// highlight looks plausible while the copied text is scrambled.
#pragma once

namespace studio {

// One selection over the whole chat view. Blocks are the chat rows, lines are the
// visual lines of one block, columns are CHARACTER indices into that line's text (a
// CJK glyph is one character, not two cells): the pixel span a selection becomes is
// measured per character by the font that draws it, so a terminal-style display
// column would be a second, disagreeing unit.
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
    // Normalised order: the earlier end first. A drag runs in either direction, so
    // every reader of a selection goes through this and never compares A against B.
    void ordered(int* blockA_, int* lineA_, int* colA_, int* blockB_, int* lineB_,
                 int* colB_) const;
};

// Does visual line (block, line) fall inside the normalised range
// [block0,line0 .. block1,line1]? Blocks are chat rows, so the comparison is numeric
// and depends on the ids being in visual order (see blockSeq in studio_ui.cpp).
bool lineInRange(int block, int line, int block0, int line0, int block1, int line1);

// The [from, to) character span one line contributes to the selection.
// `columns` is that line's own length in characters (text_runs::charCount of its
// text). False when the line contributes nothing -- the range ends before its first
// column, which is the case that must not emit an empty line into the clipboard.
bool columnSpan(int block, int line, int columns, int block0, int line0, int col0, int block1,
                int line1, int col1, int* from, int* to);

} // namespace studio
