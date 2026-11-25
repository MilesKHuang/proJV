// proJV studio -- the header's buttons, one row per key the product documents.
//
// Why this file exists. The TUI has a keycap bar: every key is visible, all the
// time. The studio dropped that bar (two places listing the same keys read as one
// unfinished feature) and put the shortcuts on the header buttons instead -- but
// then the button list was hand-written at the draw call, six entries long, and it
// silently DROPPED the rest when the window got narrow. Six buttons for a
// product with eleven keys is the report "the buttons do not correspond to what
// the TUI can do", and it was right.
//
// So the key table lives here, once, as data:
//   - the header renders every row of it, wrapping into extra rows instead of
//     hiding anything (actions::pack);
//   - its unit test asserts that the set of shortcuts equals the documented key
//     map, so a key added to the product without a button fails the build's test
//     run rather than the user's expectation.
//
// ImGui-free on purpose: the table and the packing are arithmetic, so they are
// pinned by tests/unit/test_studio_design.cpp without a window or a GPU.
#pragma once

#include <string>
#include <vector>

namespace studio {
namespace actions {

// One entry per key in docs/*/user-guide/08-reference.md. The order is the order
// the header draws them, which is the order they are reached for, not the order
// of the key numbers: writing first, then presentation, then configuration.
enum class Id {
    Role,        // Tab -- cycle the prompt file (the "role")
    NewChat,     // F3
    Sessions,    // F4
    Model,       // F10
    Reasoning,   // F8 (toggle)
    Dock,        // F9 (toggle)
    Theme,       // F6
    ThemeEditor, // F7
    Mcp,         // F5
    Config,      // F2
    Help,        // F1
};

// One row of the table. The header draws all of them as Ghost buttons: the header
// strip and a Secondary button share the same surface colour, so a filled button
// strip reads as eleven floating words. Ghost has no fill until hover, and hover
// is where the shortcut shows anyway.
struct Item {
    Id id;
    const char* label;    // used when the label does not depend on live state
    const char* shortcut; // "F3" -- what the tooltip leads with, and what a test pins
};

// The header's buttons, in drawing order. Never empty, and never stateful: the
// label/tooltip functions below are the part that reacts to the window.
const std::vector<Item>& header();

// The live state a label or a tooltip depends on.
struct State {
    bool dock = true;
    bool reasoning = false;
    std::string role; // Screen::roleName ("" when the prompt file has no name)
    int promptFiles = 0;
};

// The label drawn on the button: constant for most actions, the role's own name
// for the role chip, and the state for the two toggles ("Dock on" / "Dock off").
std::string label(Id id, const State& st);

// The hover text: the shortcut, then what it does in this state. Hover is where a
// GUI documents a key without a permanent second row of keycaps under the window.
std::string tooltip(Id id, const State& st);

// Greedy wrap of control widths into rows. Row 0 only gets `firstRowAvail` (the
// title and its pills share that line); every later row gets `rowAvail`. A control
// wider than a whole row still gets a row of its own -- the caller ellipsizes it,
// because dropping a function to make the chrome fit is the bug this replaces.
std::vector<std::vector<int>> pack(const std::vector<float>& widths, float gap, float firstRowAvail,
                                   float rowAvail);

} // namespace actions
} // namespace studio
