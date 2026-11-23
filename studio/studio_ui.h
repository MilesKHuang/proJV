// proJV studio -- the main screen's geometry and one frame of it.
//
// The window is five bands, and they are computed BEFORE the first draw call:
//
//   window
//   |- header    title, phase pill, model chip, actions
//   |- body      [ chat column | dock ]        (dock optional, F9)
//   |- composer  the prompt card, shared left edge with the chat column
//   |- footer    status line, queued line, service bar, keycap bar
//   \- slash     the floating "/" menu, anchored above the composer
//
// Nothing here reads a height recorded last frame: every band is derived from the
// live window size, the design tokens and the measured height of what it holds,
// so a resize lands correctly on the same frame it happens.
#pragma once

#include <vector>

#include "design/layout.h"
#include "design/tokens.h"

namespace studio {

class App;
struct Frame;

// Draw the whole window: header, body, composer, footer, then the modal layer.
void drawFrame(App& app);

// Draw only the modal layer (called by drawFrame; exposed for screenshots).
void drawModals(App& app);

// The bands of the main window for one frame, plus the metrics they were
// resolved at. Published so the composer and the modals can align to the same
// grid the shell used.
struct Frame {
    layout::Rect window;
    layout::Rect header;
    layout::Rect body;
    layout::Rect chat;
    layout::Rect dock;
    layout::Rect footer;
    layout::Rect statusLine;
    layout::Rect queued;
    layout::Rect composer;
    layout::Rect statusBar;
    layout::Rect slash;
    layout::Rect chatColumn; // centred reading column inside `chat`
    bool dockOpen = false;
    tokens::Metrics metrics;

    // Visual rows the prompt field needs for the text it holds (wrap + explicit
    // newlines, clamped). The composer is multi-line, so its band is sized from
    // the text before anything is drawn; the field itself reads this to know how
    // much of the box it may scroll inside.
    int composerRows = 1;

    // The agent strip wraps, so it is packed during the measuring pass: one entry
    // per row, holding the indices of the units that landed on it. Packing once
    // means the height used for the layout and the drawing cannot disagree.
    std::vector<std::vector<int>> headerAgentRows;
    float headerAgentRowsY = 0.0f;
    float headerAgentRowH = 0.0f;
};

// The frame drawn by the last drawFrame call (defaults before the first one).
const Frame& lastFrame();

} // namespace studio
