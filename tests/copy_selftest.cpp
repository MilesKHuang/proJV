// proJV -- headless copy check: no API key, no model, no GPU, no window.
//
// It proves the thing the user reported as wrong, end to end and without a model:
// a mouse DRAG over the chat text surface selects the glyphs it covers, and Ctrl+C
// copies exactly that -- including a mixed Latin/CJK line, where the old
// "columns x width('M')" hit-test drifted away from the pointer.
//
// It stays honest by construction:
//   - it drives the REAL studio::TextSurface (the same class the window uses),
//     through its real beginDraw + drawBlock + handleInput path;
//   - it feeds REAL ImGui mouse/keyboard events (io.AddMousePosEvent /
//     AddMouseButtonEvent / AddKeyEvent) into a headless ImGui context, so
//     ImGui::IsMouseClicked()/IsMouseDragging()/IsKeyPressed() answer exactly as in
//     the app;
//   - it captures the clipboard from ImGui's own Platform_SetClipboardTextFn, so it
//     reads what the app would actually paste.
//
// Run: proJV_copy_probe     (ctest name: studio_copy_probe; exit 0 = pass)
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

#include "studio_text.h"

namespace {

std::string g_copied;
void setClipboard(ImGuiContext*, const char* text) { g_copied = text ? text : ""; }

int g_failures = 0;
int g_cjkGlyphReady = 0; // 1 when a real CJK face was merged (glyph advances are real)

void check(bool ok, const std::string& what, const std::string& detail = std::string()) {
    if (ok) {
        std::printf("ok  : %s\n", what.c_str());
        return;
    }
    std::printf("FAIL: %s\n", what.c_str());
    if (!detail.empty()) std::printf("      %s\n", detail.c_str());
    ++g_failures;
}

// Load the faces the studio actually uses: a monospace Latin face with a CJK face
// merged behind it. With a real CJK face the two Chinese glyphs get their true,
// full-width advances -- and the whole point of the fix is that those advances are
// NOT "two Latin cells", which a fallback glyph could never show. On a machine
// without those fonts, the probe still runs with the default face and says so.
void loadFaces() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.MergeMode = false;
    ImFont* mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 18.0f, &cfg);
    if (!mono) mono = io.Fonts->AddFontDefault();
    const char* cjk[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf",
                         "C:\\Windows\\Fonts\\simsun.ttc"};
    for (const char* path : cjk) {
        ImFontConfig merge;
        merge.MergeMode = true;
        if (io.Fonts->AddFontFromFileTTF(path, 18.0f, &merge,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
            g_cjkGlyphReady = 1;
            break;
        }
    }
    if (!g_cjkGlyphReady) {
        io.Fonts->Fonts.resize(1); // keep the Latin/default face only
    }
}

using Lines = std::vector<studio::TextSurface::LineInfo>;

// One frame of the surface. `inject` runs BEFORE NewFrame, which is when ImGui
// consumes queued input (mouse/key), exactly like a backend's event pump. The window
// is anchored at the origin, so a line's screen box IS its layout box.
Lines frame(ImGuiIO& io, studio::TextSurface& surface, const std::string& body,
            const std::function<void()>& inject) {
    io.DisplaySize = ImVec2(1000.0f, 700.0f);
    io.DeltaTime = 1.0f / 60.0f;
    if (inject) inject();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1000, 700));
    ImGui::Begin("##probe", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    surface.beginDraw();
    // Plain text (no markdown) keeps the probe's coordinates trivial: one paragraph,
    // explicit newlines only. The chat draws in the monospace face at body size; the
    // default context's single face stands in, and the layout and the copy read the
    // SAME face because both are measured here.
    surface.drawBlock(0, body, /*markdown=*/false, 640.0f, IM_COL32(255, 255, 255, 255));
    // handleInput runs INSIDE the window, exactly like drawChat() calls it: it reads
    // the foreground draw list and hit-tests the lines this frame laid out.
    surface.handleInput();
    ImGui::End();
    ImGui::Render();
    return surface.laidOutLines();
}

float midY(const studio::TextSurface::LineInfo& ln) { return (ln.y0 + ln.y1) * 0.5f; }

// Screen x for character `col` on a line, from the offsets the layout measured.
float charX(const studio::TextSurface::LineInfo& ln, size_t col) {
    if (ln.charX.empty()) return ln.x0;
    const size_t i = col < ln.charX.size() ? col : ln.charX.size() - 1;
    return ln.x0 + ln.charX[i];
}

// A pixel a real mouse would report to land on character `col`: the MIDDLE of that
// glyph's cell, floored to a whole pixel (ImGui floors mouse positions, and an OS
// mouse only reports whole pixels anyway). Using the left edge instead makes the
// floor() fall into the previous cell, which is the probe tripping over its own
// precision -- not the bug under test.
float pxOnCol(const studio::TextSurface::LineInfo& ln, size_t col) {
    if (ln.charX.size() <= 1) return ln.x0;
    const size_t n = ln.charX.size() - 1; // number of characters
    if (col >= n) return ln.x1;           // past the last glyph -> caret at line end
    const float left = ln.x0 + ln.charX[col];
    const float right = ln.x0 + ln.charX[col + 1];
    return std::floor((left + right) * 0.5f);
}

// Drag from (ax,ay) to (bx,by) with the left button, then press Ctrl+C. Each step is
// its own frame so IsMouseClicked / IsMouseDragging / IsMouseReleased fire once each,
// the way a real drag does over several frames.
void dragAndCopy(ImGuiIO& io, studio::TextSurface& surface, const std::string& body, float ax,
                 float ay, float bx, float by) {
    // hover (settle the pointer inside the surface)
    frame(io, surface, body, [&] { io.AddMousePosEvent(ax, ay); });
    // press
    frame(io, surface, body, [&] {
        io.AddMousePosEvent(ax, ay);
        io.AddMouseButtonEvent(0, true);
    });
    // drag (button still down)
    frame(io, surface, body, [&] { io.AddMousePosEvent(bx, by); });
    // release
    frame(io, surface, body, [&] { io.AddMouseButtonEvent(0, false); });
    // Ctrl+C
    g_copied.clear();
    frame(io, surface, body, [&] {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_C, true);
    });
    // settle the modifier state
    frame(io, surface, body, [&] {
        io.AddKeyEvent(ImGuiKey_C, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
    });
}

} // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // This context has no renderer backend, so tell ImGui to build the font atlas
    // itself at frame start (the "dynamic textures" path) instead of waiting for a
    // legacy GetTexDataAsRGBA32() call that never comes. Without this, the first
    // CalcTextSize asserts "font atlas is not built".
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    loadFaces();
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = setClipboard;
    std::printf("faces: %s\n", g_cjkGlyphReady ? "mono + CJK" : "default (no CJK face found)");

    // 1 + 2. A mixed Latin/CJK line: "Hi 中文 AB" (2 ASCII, space, 2 CJK, space, 2
    // ASCII). Dragging from column 3 to column 5 must copy exactly 中文; the OLD
    // model ("2 x width('M')" per CJK) put the caret a couple of characters off, so
    // the highlight and the copy disagreed.
    {
        studio::TextSurface surface;
        const std::string body = "Hi \xe4\xb8\xad\xe6\x96\x87 AB";
        const Lines lines = frame(io, surface, body, nullptr);
        check(!lines.empty(), "cjk: the block laid out a line");
        if (!lines.empty()) {
            const auto& ln = lines[0];
            // With a real CJK face, prove the premise of the fix: a Chinese glyph is
            // NOT two Latin cells. "H" advances by a Latin width, 中 by its own; if
            // they happened to be exactly 2:1 the old model would not have drifted,
            // and it always does.
            if (g_cjkGlyphReady && ln.charX.size() >= 5) {
                const float latin = ln.charX[1] - ln.charX[0];       // H -> i
                const float cjkAdv = ln.charX[4] - ln.charX[3];      // 中 -> 文
                check(cjkAdv > latin * 2.05f || cjkAdv < latin * 1.95f,
                      "a CJK advance is not exactly 2 Latin cells (the old model's premise fails)",
                      "latin=" + std::to_string(latin) + " cjk=" + std::to_string(cjkAdv));
            }
            const float x0 = pxOnCol(ln, 3);    // inside 中
            const float x1 = pxOnCol(ln, 5);    // end of 文
            const float y = midY(ln);
            dragAndCopy(io, surface, body, x0, y, x1, y);

            const std::string expect = "\xe4\xb8\xad\xe6\x96\x87"; // 中文
            check(g_copied == expect, "drag over 中文 copies exactly 中文",
                  "got [" + g_copied + "], expected [" + expect + "]");
            check(surface.selectedText() == g_copied,
                  "clipboard equals the selection's own text (highlight == copy)");

            // The HIGHLIGHT's ruler is checked here indirectly, and deliberately so.
            // Reading the painted rectangle back would need the draw list, and every
            // comparison written from `ln.charX` on both sides (the obvious version of
            // this check) is a tautology: |A-A| < 1 can never fail. What CAN fail, and
            // is worth pinning, is that the drag landed on the two characters and that
            // the copy is their text -- the highlight reads those same columns through
            // the same charX, so a wrong ruler moves the copy first.
            const studio::Selection& s = surface.selection();
            check(s.colA == 3 && s.colB == 5,
                  "the drag selected character columns 3..5 (the two CJK glyphs)",
                  "colA=" + std::to_string(s.colA) + " colB=" + std::to_string(s.colB));
            const float midLeft = ln.x0 + ln.charX[3];
            const float midRight = ln.x0 + ln.charX[4];
            check(x0 >= midLeft - 1.0f && x0 <= midRight + 1.0f,
                  "the drag started on the first selected glyph (中)",
                  "x0=" + std::to_string(x0) + " 中=[" + std::to_string(midLeft) + "," +
                      std::to_string(midRight) + "]");
        }
    }

    // 3. A backwards drag (right to left) copies the same glyphs, in forward order.
    {
        studio::TextSurface surface;
        const std::string body = "Hi \xe4\xb8\xad\xe6\x96\x87 AB";
        const Lines lines = frame(io, surface, body, nullptr);
        if (!lines.empty()) {
            const auto& ln = lines[0];
            const float x0 = pxOnCol(ln, 3);    // inside 中
            const float x1 = pxOnCol(ln, 5);    // end of 文
            const float y = midY(ln);
            // Start on the RIGHT and drag LEFT.
            dragAndCopy(io, surface, body, x1, y, x0, y);
            const std::string expect = "\xe4\xb8\xad\xe6\x96\x87";
            check(g_copied == expect, "a backwards drag copies the same 中文 forward",
                  "got [" + g_copied + "], expected [" + expect + "]");
        }
    }

    // 4. A drag from the first line into the second keeps the newline.
    {
        studio::TextSurface surface;
        const std::string body = "first line\nsecond line";
        const Lines lines = frame(io, surface, body, nullptr);
        check(lines.size() >= 2, "two paragraphs laid out two visual lines");
        if (lines.size() >= 2) {
            const auto& a = lines[0];
            const auto& b = lines[1];
            dragAndCopy(io, surface, body, pxOnCol(a, 0), midY(a), b.x1, midY(b));
            const std::string got = surface.selectedText();
            check(got == "first line\nsecond line",
                  "a drag across two real lines keeps the newline", "got [" + got + "]");
        }
    }

    // 5. A soft-wrapped sentence is copied as one sentence, character for character.
    {
        studio::TextSurface surface;
        // Long enough to wrap inside 640 px. Spaces let the wrapper break cleanly; the
        // copy must rejoin the visual lines with no newline AND without gluing two
        // words together -- the space a break drops sits at the end of the line above,
        // so the whole paragraph has to come back byte for byte.
        const std::string body =
            "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron "
            "pi rho sigma tau upsilon phi chi psi omega";
        const Lines lines = frame(io, surface, body, nullptr);
        check(lines.size() >= 2, "a long paragraph soft-wraps into several visual lines");
        if (lines.size() >= 2 && lines[1].soft) {
            dragAndCopy(io, surface, body, pxOnCol(lines.front(), 0), midY(lines.front()),
                        lines.back().x1, midY(lines.back()));
            const std::string got = surface.selectedText();
            check(got.find('\n') == std::string::npos,
                  "a soft wrap is copied without a newline", "got [" + got + "]");
            check(got == body, "a soft-wrapped paragraph comes back exactly, spaces intact",
                  "got [" + got + "]");
        } else {
            check(false, "expected the second visual line to be marked soft (a wrap)");
        }
    }

    ImGui::DestroyContext();
    if (g_failures == 0) {
        std::printf("\nPASS: copy selection matches the highlighted glyphs\n");
        return 0;
    }
    std::printf("\nFAILED: %d check(s)\n", g_failures);
    return 1;
}
