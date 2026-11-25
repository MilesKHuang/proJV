// proJV studio -- the widget kit: one visual language for the whole window.
//
// The window used to draw controls three different ways: a custom
// `studio_theme::button`, raw ImGui::Button in some dialogs, and raw
// ImGui::Checkbox / RadioButton in others. Three looks for one idea is what makes
// a screen read as unfinished, no matter how good each piece is on its own.
//
// Every control here is painted by this file, not by ImGui's style, so:
//   - the radius, padding, height and hover state of a button are decided ONCE;
//   - a dialog and the main window cannot disagree about what a control is;
//   - the palette below is the only place a GUI colour is resolved (from
//     ThemeColors -- AGENTS.md forbids inventing one).
//
// ImGui is still the host: an InvisibleButton gives us hover/active/click with
// correct input routing, and we paint inside its rect. The text input is the one
// control not reimplemented -- IME, selection, clipboard and undo are ImGui's --
// but its frame, padding and radius come from the same tokens as everything else.
//
// Text drawing has two modes on purpose:
//   - item mode  (`text`, `hint`): flows in the current window, used inside
//     scroll areas and dialogs, where ImGui owns the cursor;
//   - placed mode (`textAt`, `badge`, `keycap`): absolute rects, used by the
//     window shell, which computes every band before it draws anything.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

#include "design/fit.h"
#include "design/layout.h"
#include "design/tokens.h"

namespace studio {
namespace ui {

using layout::Rect;
using tokens::Metrics;
using tokens::Radius;
using tokens::Space;
using tokens::Type;

// ---- palette --------------------------------------------------------------
// Semantic names, resolved from the active ThemeColors. Nothing outside this
// struct picks a colour for a control.
struct Palette {
    ImU32 window = 0;
    ImU32 surface = 0;    // the raised card a panel sits on
    ImU32 surfaceAlt = 0; // a second level: header strip, footer toolbar
    ImU32 overlay = 0;    // dialog background
    ImU32 border = 0;
    ImU32 divider = 0;

    ImU32 text = 0;
    ImU32 textDim = 0;
    ImU32 textOnAccent = 0;

    ImU32 accent = 0;
    ImU32 accentHover = 0;
    ImU32 accentPressed = 0;
    ImU32 hover = 0;
    ImU32 selected = 0;
    ImU32 pressed = 0;

    ImU32 danger = 0;
    ImU32 success = 0;
    ImU32 warning = 0;
    ImU32 info = 0;

    ImU32 scrollTrack = 0;
    ImU32 scrollThumb = 0;
    ImU32 inputBg = 0;
};

// Resolve the palette for the theme that is active right now. Cheap enough to
// call per frame (a dozen hex parses); it re-reads the theme so a theme switch
// or a live theme edit is picked up on the next frame with no cache to invalidate.
Palette palette();

// ---- metrics for the current frame ---------------------------------------
// The scale comes from one pair: the monitor DPI the window reports and the
// user's own zoom. Everything in the window is derived from it.
void setZoom(float zoom);
float zoom();
Metrics metricsFor(float dpi);

// ---- text -----------------------------------------------------------------
// Push the face and size for one tier. Base sizes are pushed unscaled: ImGui
// multiplies by style.FontScaleMain * FontScaleDpi, which is exactly the factor
// Metrics used, so a box measured in tokens and a glyph drawn here agree.
void pushType(const Metrics& m, Type tier, bool mono = false);
void popType();

float textWidth(const std::string& utf8); // current face and size
fit::Measure measurer();                  // absolute px, current face+size
fit::UnitMeasure unitMeasurer();          // px at 1 px font size
// Width of `s` at one tier, without disturbing the caller's pushed face.
float measure(const Metrics& m, const std::string& s, Type tier, bool mono = false);
// Black or white, whichever `bg` is not: the label colour for any accent fill.
ImU32 contrastOn(ImU32 bg);
// Item mode: draws one line at the cursor and advances it.
void text(const Metrics& m, const std::string& s, ImU32 color, Type tier = Type::Body,
          bool mono = false);
// Item mode, one wrapped paragraph of body text.
void paragraph(const Metrics& m, const std::string& s, ImU32 color, Type tier = Type::Body);
// Item mode, secondary colour, Small tier: the "hint" line under a control.
void hint(const Metrics& m, const std::string& s);

// Placed mode: draws `s` inside `box` with no item, clipped to the box.
// alignX/alignY are 0..1 (0 = left/top, 0.5 = centre).
void textAt(const Metrics& m, const Rect& box, const std::string& s, ImU32 color, Type tier,
            bool mono, float alignX = 0.0f, float alignY = 0.5f);
// Placed mode for chrome: choose the largest of `tiers` that fits (listed largest
// first) and ellipsize when even the last one does not.
void fittedText(const Metrics& m, const Rect& box, const std::string& s, ImU32 color,
                const Type* tiers, int tierCount, bool mono, float alignX = 0.0f);
// A label allowed exactly one line that must not overflow: ellipsis.
std::string fitLine(const Metrics& m, const std::string& s, float width, Type tier, bool mono);

// ---- surfaces -------------------------------------------------------------
void fill(const Rect& box, ImU32 color);
void stroke(const Rect& box, ImU32 color, float thickness, float radius);
void fillRound(const Rect& box, ImU32 color, float radius);
// A card: filled surface + hairline border + token radius.
void card(const Rect& box, const Metrics& m, bool raised = false);
// A hairline across the current window at the cursor, then a small gap.
void divider(const Metrics& m);
void dividerAt(const Metrics& m, const Rect& box);

// ---- controls -------------------------------------------------------------
// Four weights, one shape. Primary carries the accent (the eye lands on Save,
// not on whichever button came first); Ghost has no fill until hover.
enum class Btn { Primary, Secondary, Ghost, Danger };

// The width a button wants: its label plus 2 x Md padding, never below 88 px.
float buttonWidth(const Metrics& m, const std::string& label, Btn kind = Btn::Secondary);
// Draw at the cursor using its own width and the token control height.
bool button(const Metrics& m, const std::string& label, Btn kind = Btn::Secondary,
            const char* tooltip = nullptr);
// Draw inside an exact rect (the footer layout decides the positions). `tooltip`
// is shown on hover: the header buttons carry their shortcut that way, so the key
// is documented without a permanent second row of keycaps under the window.
bool buttonAt(const Metrics& m, const Rect& box, const std::string& label, Btn kind,
              const char* tooltip = nullptr);

// A square ghost button holding one glyph (a close cross, a chevron).
bool iconButton(const Metrics& m, const Rect& box, const std::string& glyph);
bool checkbox(const Metrics& m, const std::string& label, bool* value);
bool radio(const Metrics& m, const std::string& label, bool selected);
// N options in one row, the selected one filled with the accent. Returns the new
// index, or -1 when nothing was clicked.
int segmented(const Metrics& m, const std::vector<std::string>& options, int selected, float width);
// The one ImGui-native control: IME, selection, clipboard, undo. Styled entirely
// from tokens so it matches the painted controls around it. Returns true when
// Enter was pressed; fieldEdited() reports whether the text changed.
bool field(const Metrics& m, const char* id, std::string* text, const char* hintText, bool password,
           float width = -1.0f);
bool fieldEdited();
// A full-width selectable row: hover fill, selected fill, optional subtitle.
// Returns true on click; listRowDoubleClicked() reports a double click.
bool listRow(const Metrics& m, const std::string& label, bool selected, bool disabled = false);
bool listRowDoubleClicked();

// The same row inside a rect the caller owns, for panels that place their rows
// themselves (the tree, the session list). Draws the same hover/selection look and
// keeps the tooltip-free contract: the caller paints its own icons in the space it
// reserved, so a click on an icon cannot also count as a click on the row.
bool listRowAt(const Metrics& m, const Rect& box, const std::string& label, bool selected,
               bool disabled = false, Type tier = Type::Body);

// A small pill of text (a phase name, a model name).
void badge(const Metrics& m, const Rect& box, const std::string& label, ImU32 fg, ImU32 bg);

// ---- scrolling ------------------------------------------------------------
// One scroll region, themed: the child window gets the surface's background and
// the scrollbar is painted from the palette instead of ImGui's defaults.
bool beginScroll(const char* id, const Rect& box, const Metrics& m, ImU32 bg);
// Move the visible window inside the region currently being built.
void scrollBy(float pixels);
float scrollMax();
// Pixels between the view and the bottom of its content (0 = at the bottom, and
// 0 when there is nothing to scroll). Read it before endScroll(): ImGui knows the
// real content height only once this frame's items have been laid out.
float scrollFromBottom();
// Jump to the bottom. Call it every frame for as long as the view should follow:
// ImGui's content height is one frame behind the rows drawn this frame, so a single
// call at the moment content arrives is clamped to the old bottom and stays short.
void scrollToBottom();
// Call before EndChild. `follow` requests a jump to the bottom (new content);
// the return value reports whether the view sits at the bottom now, which is
// what the caller stores as the next frame's sticky-bottom state.
bool endScroll(bool follow);

} // namespace ui
} // namespace studio
