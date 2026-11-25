// proJV studio -- draw markdown (design 08.5, D5/D7).
//
// The GRAMMAR is not re-implemented here: studio_markdown consumes
// tui/markdown_text.h's parseMarkdown(), the very same tokenizer the TUI renders,
// so a message cannot look like one thing in the terminal and another in the
// window. What this file owns is the part that must differ -- turning styled
// segments into ImGui draw calls, including the soft wrap the terminal got for
// free from its fixed cell grid.
#pragma once

#include <string>

#include "imgui.h"

#include "tui/markdown_text.h"

namespace studio_md {

// Measure one line of text in pixels at the current font (UTF-8 aware).
float textWidth(const std::string& utf8);

// Wrap one paragraph of styled segments to `wrapWidth` pixels. Returns lines, each
// a list of segments that fit. Pure apart from the font measurement.
std::vector<std::vector<markdown_text::Segment>>
wrap(const std::vector<markdown_text::Segment>& segs, float wrapWidth);

// Draw plain text with soft wrap (no markdown grammar), in `color`.
void drawPlain(const std::string& text, float wrapWidth, ImU32 color);

// One muted line, used for hints ("Esc close . Up/Down move").
void drawHint(const std::string& text, float wrapWidth);

} // namespace studio_md
