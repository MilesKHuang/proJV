// proJV studio -- theme + fonts (see studio_theme.h).
#include "studio_theme.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "design/tokens.h"
#include "tui/theme_manager.h"

// studio_theme lives at global scope (the theme layer predates the studio
// namespace), so the tree is spelled once here instead of at every call site.
namespace tokens = studio::tokens;

namespace studio_theme {namespace {

float g_fontScale = 1.0f;
ImFont* g_ui = nullptr;
ImFont* g_mono = nullptr;

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Candidate faces, first hit wins. Monospace first (the layout depends on it),
// CJK second (merged into the same atlas).
struct FaceList {
    const char* mono[4];
    const char* cjk[5];
    const char* ui[4];
};

FaceList faceList() {
#ifdef _WIN32
    // Consolas / Segoe UI ship with Windows; MS YaHei is the UI CJK face.
    FaceList f{{"C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\lucon.ttf",
                "C:\\Windows\\Fonts\\cour.ttf", nullptr},
               {"C:\\Windows\\Fonts\\msyhl.ttc", "C:\\Windows\\Fonts\\msyh.ttc",
                "C:\\Windows\\Fonts\\msyh.ttf", "C:\\Windows\\Fonts\\simhei.ttf", nullptr},
               {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\tahoma.ttf",
                "C:\\Windows\\Fonts\\arial.ttf", nullptr}};
    return f;
#else
    FaceList f{{"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", nullptr},
               {"/usr/share/fonts/opentype/noto/NotoSansMonoCJKsc-Regular.otf",
                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", nullptr, nullptr},
               {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", nullptr}};
    return f;
#endif
}

ImFont* addFirst(ImFontAtlas* atlas, const char* const* candidates, float size, bool merge,
                 const ImWchar* ranges) {
    for (int i = 0; i < 4 && candidates[i]; ++i) {
        ImFontConfig cfg;
        cfg.MergeMode = merge;
        // One size is baked up front; 1.92+ re-rasterizes on demand for a new
        // size, so a fractional DPI scale stays crisp without a rebuild.
        ImFont* f = atlas->AddFontFromFileTTF(candidates[i], size, &cfg, ranges);
        if (f) return f;
    }
    return nullptr;
}

} // namespace

ImU32 fromHex(const std::string& hex, float alpha) {
    const size_t offset = (!hex.empty() && hex[0] == '#') ? 1 : 0;
    const char* s = hex.c_str() + offset;
    const size_t n = hex.size() - offset;
    const int a8 = static_cast<int>(alpha * 255 + 0.5f);
    int r = 0, g = 0, b = 0;
    if (n >= 6) {
        const int a = hexNibble(s[0]), bb = hexNibble(s[1]);
        const int c = hexNibble(s[2]), d = hexNibble(s[3]);
        const int e = hexNibble(s[4]), f = hexNibble(s[5]);
        if (a < 0 || bb < 0 || c < 0 || d < 0 || e < 0 || f < 0) return IM_COL32(255, 0, 255, a8);
        r = a * 16 + bb;
        g = c * 16 + d;
        b = e * 16 + f;
    } else if (n >= 3) {
        const int a = hexNibble(s[0]), c = hexNibble(s[1]), e = hexNibble(s[2]);
        if (a < 0 || c < 0 || e < 0) return IM_COL32(255, 0, 255, a8);
        r = a * 17;
        g = c * 17;
        b = e * 17;
    } else {
        return IM_COL32(255, 0, 255, a8);
    }
    return IM_COL32(r, g, b, a8);
}

ImU32 field(const std::string ThemeColors::*member, float alpha) {
    if (!member) return IM_COL32(255, 0, 255, 255);
    return fromHex(ThemeManager::instance().current().*member, alpha);
}

ImU32 fieldNamed(const std::string& name, float alpha) {
    if (name.empty()) return field(&ThemeColors::text, alpha);
    for (const auto& f : themeColorFields()) {
        if (name == f.name) return field(f.member, alpha);
    }
    return field(&ThemeColors::text, alpha);
}

namespace {

float channelLum(int c) {
    const float s = static_cast<float>(c) / 255.0f;
    return s <= 0.03928f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

float luminance(ImU32 c) {
    const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    return 0.2126f * channelLum(r) + 0.7152f * channelLum(g) + 0.0722f * channelLum(b);
}

float contrast(ImU32 a, ImU32 b) {
    const float la = luminance(a);
    const float lb = luminance(b);
    const float hi = std::max(la, lb);
    const float lo = std::min(la, lb);
    return (hi + 0.05f) / (lo + 0.05f);
}

} // namespace

ImU32 readable(ImU32 fg, ImU32 bg, float minRatio) {
    if (contrast(fg, bg) >= minRatio) return fg;
    // Move toward whichever end (black or white) the background is not.
    const bool darkBg = luminance(bg) < 0.5f;
    const ImU32 target = darkBg ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
    const int fr = (fg >> IM_COL32_R_SHIFT) & 0xFF, fgn = (fg >> IM_COL32_G_SHIFT) & 0xFF,
              fb = (fg >> IM_COL32_B_SHIFT) & 0xFF;
    const int tr = (target >> IM_COL32_R_SHIFT) & 0xFF, tg = (target >> IM_COL32_G_SHIFT) & 0xFF,
              tb = (target >> IM_COL32_B_SHIFT) & 0xFF;
    for (float t = 0.1f; t <= 1.0f; t += 0.1f) {
        const ImU32 mixed = IM_COL32(static_cast<int>(fr + (tr - fr) * t),
                                     static_cast<int>(fgn + (tg - fgn) * t),
                                     static_cast<int>(fb + (tb - fb) * t),
                                     (fg >> IM_COL32_A_SHIFT) & 0xFF);
        if (contrast(mixed, bg) >= minRatio) return mixed;
    }
    return target;
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const auto blend = [a, b, t](int shift) {
        const int ca = static_cast<int>((a >> shift) & 0xFF);
        const int cb = static_cast<int>((b >> shift) & 0xFF);
        return static_cast<int>(ca + static_cast<float>(cb - ca) * t);
    };
    return IM_COL32(blend(IM_COL32_R_SHIFT), blend(IM_COL32_G_SHIFT), blend(IM_COL32_B_SHIFT),
                    blend(IM_COL32_A_SHIFT));
}

void applyStyle() {
    const ThemeColors& T = ThemeManager::instance().current();
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec4* c = st.Colors;
    const auto set = [&](ImGuiCol idx, const std::string& hex, float a = 1.0f) {
        c[idx] = ImGui::ColorConvertU32ToFloat4(fromHex(hex, a));
    };
    set(ImGuiCol_Text, T.text);
    set(ImGuiCol_TextDisabled, T.textDisabled);
    set(ImGuiCol_WindowBg, T.windowBg);
    set(ImGuiCol_ChildBg, T.childBg);
    set(ImGuiCol_PopupBg, T.popupBg);
    set(ImGuiCol_Border, T.border);
    set(ImGuiCol_BorderShadow, T.border, 0.0f);
    set(ImGuiCol_FrameBg, T.frameBg);
    set(ImGuiCol_FrameBgHovered, T.buttonHovered);
    set(ImGuiCol_FrameBgActive, T.buttonActive);
    set(ImGuiCol_TitleBg, T.titleBg);
    set(ImGuiCol_TitleBgActive, T.titleBg);
    set(ImGuiCol_TitleBgCollapsed, T.titleBg);
    set(ImGuiCol_MenuBarBg, T.menuBarBg);
    set(ImGuiCol_ScrollbarBg, T.scrollbarBg);
    set(ImGuiCol_ScrollbarGrab, T.scrollbarGrab);
    set(ImGuiCol_ScrollbarGrabHovered, T.scrollbarGrab);
    set(ImGuiCol_ScrollbarGrabActive, T.scrollbarGrab);
    set(ImGuiCol_CheckMark, T.phaseStreaming);
    set(ImGuiCol_SliderGrab, T.scrollbarGrab);
    set(ImGuiCol_SliderGrabActive, T.buttonActive);
    set(ImGuiCol_Button, T.buttonBg);
    set(ImGuiCol_ButtonHovered, T.buttonHovered);
    set(ImGuiCol_ButtonActive, T.buttonActive);
    set(ImGuiCol_Header, T.headerBg);
    // Hover must be visible against its own row. The shipped themes give headerBg and
    // headerHovered the same value, so selecting a list row gave no feedback at all;
    // derive the hover shade from the window background instead.
    c[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(surface(fromHex(T.windowBg), 0.14f));
    set(ImGuiCol_HeaderActive, T.headerActive);
    set(ImGuiCol_Separator, T.separator);
    set(ImGuiCol_SeparatorHovered, T.separator);
    set(ImGuiCol_SeparatorActive, T.separator);
    set(ImGuiCol_ResizeGrip, T.scrollbarGrab, 0.5f);
    set(ImGuiCol_ResizeGripHovered, T.scrollbarGrab);
    set(ImGuiCol_ResizeGripActive, T.buttonActive);
    set(ImGuiCol_Tab, T.headerBg);
    set(ImGuiCol_TabHovered, T.headerHovered);
    set(ImGuiCol_TabSelected, T.headerActive);
    set(ImGuiCol_TabDimmed, T.headerBg);
    set(ImGuiCol_TabDimmedSelected, T.headerActive);
    set(ImGuiCol_DockingPreview, T.phaseStreaming, 0.7f);
    set(ImGuiCol_DockingEmptyBg, T.windowBg);
    set(ImGuiCol_TextSelectedBg, T.buttonActive, 0.7f);
    set(ImGuiCol_NavCursor, T.phaseStreaming);
    set(ImGuiCol_TableHeaderBg, T.headerBg);
    set(ImGuiCol_TableBorderStrong, T.border);
    set(ImGuiCol_TableBorderLight, T.separator);
    set(ImGuiCol_TableRowBg, T.childBg, 0.0f);
    set(ImGuiCol_TableRowBgAlt, T.frameBg, 0.4f);
    set(ImGuiCol_ModalWindowDimBg, T.windowBg, 0.6f);

    // Dim text is the one colour a theme almost always gets wrong: it is authored
    // against the terminal's own palette, not against this window's background.
    const ImU32 dim = readable(fromHex(T.textDisabled), fromHex(T.windowBg), 4.5f);
    c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(dim);
}

ImU32 dimText() {
    const ThemeColors& T = ThemeManager::instance().current();
    return readable(fromHex(T.textDisabled), fromHex(T.windowBg), 4.5f);
}

ImU32 surface(ImU32 base, float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    const bool darkBg = luminance(base) < 0.5f;
    const int target = darkBg ? 255 : 0;
    const auto mix = [&](int shift) {
        const int c = (base >> shift) & 0xFF;
        return static_cast<ImU32>(c + (target - c) * amount);
    };
    return IM_COL32(mix(IM_COL32_R_SHIFT), mix(IM_COL32_G_SHIFT), mix(IM_COL32_B_SHIFT),
                    (base >> IM_COL32_A_SHIFT) & 0xFF);
}

void applyMetrics(const tokens::Metrics& m) {
    // ImGui multiplies font sizes by the scale factors but NOT the sizes below, so
    // every value here is already in final pixels. That is what keeps the space
    // around the glyphs proportional to the glyphs, which is the whole reason the
    // window used to look cramped at 150%: 27 px text inside 6 px padding.
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f; // the root window is the window
    st.ChildRounding = tokens::radiusPx(m, tokens::Radius::Md);
    st.PopupRounding = tokens::radiusPx(m, tokens::Radius::Lg);
    st.FrameRounding = tokens::radiusPx(m, tokens::Radius::Sm);
    st.ScrollbarRounding = m.scrollbar * 0.5f;
    st.GrabRounding = tokens::radiusPx(m, tokens::Radius::Xs);
    st.WindowBorderSize = m.hairline;
    st.FrameBorderSize = m.hairline;
    st.WindowPadding = ImVec2(m.pagePad, m.pagePad * 0.5f);
    st.FramePadding =
        ImVec2(tokens::spacePx(m, tokens::Space::Sm), tokens::spacePx(m, tokens::Space::Xs));
    st.ItemSpacing =
        ImVec2(tokens::spacePx(m, tokens::Space::Sm), tokens::spacePx(m, tokens::Space::Xs));
    st.ItemInnerSpacing = ImVec2(tokens::spacePx(m, tokens::Space::Xs),
                                 tokens::spacePx(m, tokens::Space::Xs));
    st.CellPadding =
        ImVec2(tokens::spacePx(m, tokens::Space::Sm), tokens::spacePx(m, tokens::Space::Xs));
    st.ScrollbarSize = m.scrollbar;
    st.GrabMinSize = m.scrollbar * 0.8f;
    st.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    st.SeparatorTextBorderSize = m.hairline;
}

void applyMetrics(float dpi) {
    applyMetrics(tokens::build(dpi, 1.0f));
}

float fontPx(TextRole role) {
    // The ladder lives in design/tokens.h: one source for both front-end layers,
    // so a dialog title and the header title cannot disagree about their size.
    // These are 100% sizes; ImGui multiplies by FontScaleMain * FontScaleDpi.
    switch (role) {
    case TextRole::Title:
        return tokens::kTypePx[static_cast<int>(tokens::Type::Title)];
    case TextRole::Heading:
        return tokens::kTypePx[static_cast<int>(tokens::Type::Heading)];
    case TextRole::Small:
        return tokens::kTypePx[static_cast<int>(tokens::Type::Small)];
    case TextRole::Tiny:
        return tokens::kTypePx[static_cast<int>(tokens::Type::Caption)];
    case TextRole::Body:
    default:
        return tokens::kTypePx[static_cast<int>(tokens::Type::Body)];
    }
}

void pushText(TextRole role) {
    if (g_ui) ImGui::PushFont(g_ui, fontPx(role));
}

void pushMono(TextRole role) {
    if (g_mono) ImGui::PushFont(g_mono, fontPx(role));
}

void popText() {
    ImGui::PopFont();
}

bool loadFonts(float scale, std::string* err) {
    ImGuiIO& io = ImGui::GetIO();
    g_fontScale = scale > 0.1f ? scale : 1.0f;

    io.Fonts->Clear();
    const FaceList faces = faceList();
    const float size = 18.0f;
    // Optical size, measured off the screenshots: a CJK ideograph fills ~0.88 of its
    // em, a Latin cap height is ~0.70. At 1.0 the Chinese glyphs were ~27% taller than
    // the digits beside them ("大小还是有差距"); 0.85 lands them within a pixel of the
    // Latin ascender. One constant, one line, if it still reads off.
    constexpr float kCjkScale = 0.85f;
    const float cjkSize = size * kCjkScale;

    // 1. Monospace + CJK behind it: chat, prompt, tables, box drawing.
    g_mono = addFirst(io.Fonts, faces.mono, size, false, nullptr);
    if (!g_mono) {
        if (err) *err = "no monospace font found (looked for Consolas / DejaVu Sans Mono)";
        io.Fonts->AddFontDefault();
        ImGui::GetStyle().FontScaleMain = 1.0f;
        ImGui::GetStyle().FontScaleDpi = g_fontScale;
        g_ui = io.Fonts->Fonts[0];
        g_mono = g_ui;
        io.FontDefault = g_ui;
        return false;
    }
    ImFontConfig cjkCfg;
    cjkCfg.MergeMode = true;
    cjkCfg.GlyphOffset.y = 0.0f; // the smaller glyph sits on the Latin baseline
    bool cjk = false;
    for (int i = 0; i < 5 && faces.cjk[i]; ++i) {
        if (io.Fonts->AddFontFromFileTTF(faces.cjk[i], cjkSize, &cjkCfg,
                                         io.Fonts->GetGlyphRangesChineseFull())) {
            cjk = true;
            break;
        }
    }
    if (!cjk && err) *err = "no CJK font found; Chinese text will show as boxes";

    // 2. Proportional UI face + the same CJK fallback: chrome, dialogs, buttons.
    // A window whose every glyph is monospaced reads as a terminal no matter how
    // it is coloured, which is exactly the complaint.
    g_ui = addFirst(io.Fonts, faces.ui, size, false, nullptr);
    if (g_ui) {
        ImFontConfig uiCjk;
        uiCjk.MergeMode = true;
        uiCjk.GlyphOffset.y = 0.0f;
        for (int i = 0; i < 5 && faces.cjk[i]; ++i) {
            if (io.Fonts->AddFontFromFileTTF(faces.cjk[i], cjkSize, &uiCjk,
                                             io.Fonts->GetGlyphRangesChineseFull()))
                break;
        }
    } else {
        g_ui = g_mono; // no proportional face: fall back rather than crash
    }

    io.Fonts->Build();
    // 1.92+ split the old FontGlobalScale in two: FontScaleMain is a user zoom,
    // FontScaleDpi follows the monitor.
    ImGui::GetStyle().FontScaleMain = 1.0f;
    ImGui::GetStyle().FontScaleDpi = g_fontScale;
    io.FontDefault = g_ui;
    return true;
}

ImFont* uiFont() {
    return g_ui;
}

ImFont* monoFont() {
    return g_mono;
}

void setFontScale(float scale) {
    if (scale < 0.1f) scale = 1.0f;
    if (scale == g_fontScale) return;
    g_fontScale = scale;
    ImGui::GetStyle().FontScaleDpi = g_fontScale;
}

} // namespace studio_theme
