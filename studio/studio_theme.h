// proJV studio -- theme and fonts for the ImGui front-end (design 08.5, D7).
//
// One theme, two conversions: the TUI turns ThemeColors into ftxui::Color
// (tui/theme_map.cpp), the studio turns the same ThemeColors into ImU32 here.
// Neither invents a colour. AGENTS.md forbids colours outside the theme system,
// and this file is the studio's only place allowed to build one.
//
// Fonts: the TUI's layout is built on a monospaced cell (box drawing, tables,
// column alignment, the spinner). A proportional GUI font would misalign all of
// it, so the studio loads a monospace face and merges a CJK face behind it.
#pragma once

#include "imgui.h"

#include <string>

#include "design/tokens.h"
#include "tui/theme_colors.h"

namespace studio_theme {

// "#RRGGBB" (or "#RGB") -> ImU32 with the given alpha. An unparseable string is
// opaque magenta rather than black: a broken theme field must be visible.
ImU32 fromHex(const std::string& hex, float alpha = 1.0f);

// Resolve a ThemeColors field of the ACTIVE theme (see ThemeManager::current).
ImU32 field(const std::string ThemeColors::*member, float alpha = 1.0f);

// Resolve by field NAME (that is what a view_model snapshot carries).
ImU32 fieldNamed(const std::string& name, float alpha = 1.0f);

// Nudge `fg` toward black or white until it has at least `minRatio` contrast
// against `bg` (WCAG relative luminance). Secondary text -- hints, captions, the
// disabled-looking lines -- is where a hand-authored theme usually fails: on the
// Light theme "#9999A0 on #F5F5F5" is a 2.7:1 ratio, which reads as washed out.
// Semantic colours are left alone; only text that is *meant* to be dim passes
// through here.
ImU32 readable(ImU32 fg, ImU32 bg, float minRatio = 4.5f);

// Linear blend, `t` = 0 gives `a` and 1 gives `b`. The one piece of colour
// arithmetic the widget kit needs (a tinted selection, a pressed fill), and it
// still starts from two theme colours -- never from an invented RGB.
ImU32 mix(ImU32 a, ImU32 b, float t);

// ImGuiCol_* mapping so stock widgets (frames, buttons, scrollbars) follow the
// active theme instead of ImGui's dark defaults. Colours only: metrics are set
// once by applyMetrics(), because ImGui's sizes are NOT scaled by FontScaleDpi
// and re-applying a base every frame would undo that scaling.
void applyStyle();

// Set the metric ladder (padding / spacing / rounding) at 100%, then scale it by
// `dpi` ONCE. Without this, a 150% monitor gets 27 px glyphs inside 6 px padding:
// the text grows, the space around it does not, and every panel looks cramped --
// which is what "ugly" looked like before this existed.
void applyMetrics(float dpi);
// The same ladder, resolved for one frame's metrics (DPI x user zoom). The shell
// calls this every frame so ImGui's own widgets (text input, scrollbars) follow
// the same tokens as the painted controls.
void applyMetrics(const studio::tokens::Metrics& m);

// Type scale. One size for everything reads as a terminal; three or four tiers
// with a ~1.15-1.33 ratio is what makes a hierarchy visible without a font family
// per role. Sizes are unscaled: ImGui multiplies by FontScaleDpi/FontScaleMain.
enum class TextRole {
    Title,   // window / dialog title
    Heading, // section header (TODO, dialog sections)
    Body,    // default
    Small,   // labels, status bar, hints
    Tiny,    // keycaps, footnotes
};

float fontPx(TextRole role);

// Push the chrome face at `role`'s size (pair every push with popText).
void pushText(TextRole role);
// Push the monospace face at `role`'s size (chat body, prompt, aligned output).
void pushMono(TextRole role);
void popText();

// The dim text colour, nudged to at least 4.5:1 against the window background.
// Every "hint" in the studio goes through here: a hand-authored theme's disabled
// grey is unreadable on the light background (2.7:1 in the shipped Light theme).
ImU32 dimText();

// A panel surface derived from a theme colour: `base` moved `amount` (0..1) of the
// way toward white on a dark background, or toward black on a light one. Themes do
// not ship a "slightly different from the window" shade -- Obsidian's menuBarBg and
// windowBg are the same near-black -- so a header/footer strip built from a theme
// field is invisible. Deriving it keeps AGENTS.md's rule (no colour outside the
// theme) while giving the window the two surfaces every GUI has.
ImU32 surface(ImU32 base, float amount = 0.06f);

// ---- fonts ----------------------------------------------------------------
// Sizes and spacing are NOT decided here any more: they come from
// design/tokens.h, and studio_theme::fontPx delegates to that ladder so the theme
// layer and the widget kit cannot drift apart.
//
// Load (or reload) the font atlas at `scale`. Returns false and fills `err`
// when no usable font was found -- the caller decides how loud to be.
bool loadFonts(float scale, std::string* err);

// Two faces, because one face cannot do both jobs:
//   uiFont()   proportional (Segoe UI / Noto Sans) for chrome: titles, buttons,
//              dialogs, status bar. A monospace window reads as a terminal.
//   monoFont() monospace (Consolas / DejaVu Sans Mono) for the chat, the prompt and
//              anything that must align: tables, box drawing, code.
// Both have a CJK face merged behind them.
ImFont* uiFont();
ImFont* monoFont();

// Rebuild the atlas at a new scale (hi-DPI monitor switch).
void setFontScale(float scale);
} // namespace studio_theme
