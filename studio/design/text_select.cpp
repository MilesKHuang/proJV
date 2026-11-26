// proJV studio -- selection arithmetic (see text_select.h).
#include "text_select.h"

namespace studio {

void Selection::ordered(int* b0, int* l0, int* c0, int* b1, int* l1, int* c1) const {
    const bool forward = (blockA < blockB) ||
                         (blockA == blockB && (lineA < lineB || (lineA == lineB && colA <= colB)));
    *b0 = forward ? blockA : blockB;
    *l0 = forward ? lineA : lineB;
    *c0 = forward ? colA : colB;
    *b1 = forward ? blockB : blockA;
    *l1 = forward ? lineB : lineA;
    *c1 = forward ? colB : colA;
}

bool lineInRange(int block, int line, int block0, int line0, int block1, int line1) {
    const bool afterStart = block > block0 || (block == block0 && line >= line0);
    const bool beforeEnd = block < block1 || (block == block1 && line <= line1);
    return afterStart && beforeEnd;
}

bool columnSpan(int block, int line, int columns, int block0, int line0, int col0, int block1,
                int line1, int col1, int* from, int* to) {
    if (!lineInRange(block, line, block0, line0, block1, line1)) return false;
    // The first line starts at the anchor's column, the last ends at the caret's; a
    // line in between is covered whole.
    const int f = (block == block0 && line == line0) ? col0 : 0;
    const int t = (block == block1 && line == line1) ? col1 : columns;
    if (t <= f) return false;
    if (from) *from = f;
    if (to) *to = t;
    return true;
}

} // namespace studio
