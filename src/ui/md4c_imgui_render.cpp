#include "md4c_imgui_render.h"
#include "debug_log.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <vector>
#include <cstring>
#include <algorithm>

extern "C" {
#include "md4c.h"
#include "entity.h"
}

// ============================================================================
// Constants
// ============================================================================

// Colors — aligned with old imgui_markdown MdFormatCallback
static const ImVec4 COLOR_EM        = ImVec4(0.55f, 0.85f, 1.0f,  1.0f);
static const ImVec4 COLOR_STRONG    = ImVec4(1.0f,  0.85f, 0.35f, 1.0f);
static const ImVec4 COLOR_LINK      = ImVec4(0.5f,  0.8f,  1.0f,  1.0f);
static const ImVec4 COLOR_HEADING   = ImVec4(1.0f,  1.0f,  1.0f,  1.0f);
static const ImVec4 COLOR_QUOTE     = ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
static const ImVec4 COLOR_CODE_BG   = ImVec4(0.18f, 0.18f, 0.22f, 1.0f);
static const ImVec4 COLOR_DEL       = ImVec4(0.5f,  0.5f,  0.55f, 1.0f);
static const ImVec4 COLOR_U         = ImVec4(0.85f, 0.85f, 0.95f, 1.0f);
static const ImVec4 COLOR_CODE_TEXT = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);  // inline code + code block
static const ImVec4 COLOR_LINK_UL   = ImVec4(0.4f,  0.6f,  0.9f,  1.0f);  // underline colour

static const int   MAX_CODE_LINES   = 30;
static const int   OL_COUNTER_MAX   = 16;

// ============================================================================
// Render context
// ============================================================================

struct RenderCtx {
    std::string buf;
    float       bubbleWidth;
    Md4cLinkCallback linkCb;
    void*       linkUserdata;

    struct BlockInfo { MD_BLOCKTYPE type; int extra; };
    std::vector<BlockInfo> blockStack;

    int spanStylePushes = 0;
    int pushVarCount    = 0;
    int pushColorCount  = 0;
    int pushIdCount     = 0;

    std::string tmpLinkUrl;

    int  olCounter[OL_COUNTER_MAX] = {};
    int  listDepth = 0;
    int  headingLevel = 0;
    int  tableCols = 0;
    bool inTableHeader = false;

    // --- Span helpers ---

    void flush() {
        if (buf.empty()) return;
        ImGui::TextWrapped("%s", buf.c_str());
        buf.clear();
    }

    void pushSpanColor(const ImVec4& c) {
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ++spanStylePushes;
    }
    void popSpanColor() {
        if (spanStylePushes > 0) { ImGui::PopStyleColor(); --spanStylePushes; }
    }
    void clearSpans() {
        while (spanStylePushes > 0) { ImGui::PopStyleColor(); --spanStylePushes; }
    }

    // --- Block helpers ---

    void pushExtraVar()   { ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f); ++pushVarCount; }
    void pushExtraVar2()  { ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f); ++pushVarCount; }
    void popExtraVars(int n) { while (n-- > 0 && pushVarCount > 0) { ImGui::PopStyleVar(); --pushVarCount; } }

    void pushExtraColor(const ImVec4& c) { ImGui::PushStyleColor(ImGuiCol_ChildBg, c); ++pushColorCount; }
    void popExtraColors(int n) { while (n-- > 0 && pushColorCount > 0) { ImGui::PopStyleColor(); --pushColorCount; } }

    void pushExtraId()    { static int gId = 0; ImGui::PushID(++gId); ++pushIdCount; }
    void popExtraId()     { if (pushIdCount > 0) { ImGui::PopID(); --pushIdCount; } }

    void pushBlock(MD_BLOCKTYPE t, int extra = 0) { blockStack.push_back({t, extra}); }
    void popBlock() { if (!blockStack.empty()) blockStack.pop_back(); }
    MD_BLOCKTYPE topBlockType() const { return blockStack.empty() ? MD_BLOCK_DOC : blockStack.back().type; }
    MD_BLOCKTYPE parentBlockType() const {
        if (blockStack.size() < 2) return MD_BLOCK_DOC;
        return blockStack[blockStack.size() - 2].type;
    }
};

// ============================================================================
// md4c callbacks
// ============================================================================

static int enterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<RenderCtx*>(userdata);

    switch (type) {

    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_H: {
        auto* hd = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        ctx->headingLevel = hd->level;

        // Spacing before heading — matches old MdFormatCallback HEADING
        ImGui::Dummy(ImVec2(0.0f, 16.0f));

        // Font scale (old code used PushFont; we fall back to SetWindowFontScale)
        float scale = 1.4f - hd->level * 0.12f;
        if (scale < 0.9f) scale = 0.9f;
        ImGui::SetWindowFontScale(scale);

        // Background bar — width uses content region, not bubbleWidth (matching old code)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float h = ImGui::GetTextLineHeight() + 6.0f;
        dl->AddRectFilled(cp, ImVec2(cp.x + w, cp.y + h),
                          IM_COL32(255, 255, 255, 20), 3.0f);

        ctx->pushSpanColor(COLOR_HEADING);
        break;
    }

    case MD_BLOCK_P:
        break;

    case MD_BLOCK_QUOTE:
        ctx->pushSpanColor(COLOR_QUOTE);
        ctx->buf += "\xe2\x96\x8c ";  // ▌
        break;

    case MD_BLOCK_CODE: {
        // Same child-window style as old code-block rendering
        ctx->pushExtraVar();    // ChildRounding
        ctx->pushExtraVar2();   // ChildBorderSize
        ctx->pushExtraColor(COLOR_CODE_BG);
        ctx->pushExtraId();
        ImGui::BeginChild("##code", ImVec2(ctx->bubbleWidth - 16.0f, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        // Use subtle light colour for code text (old code used default markdown font = equiv. to no special colour)
        ctx->pushSpanColor(COLOR_CODE_TEXT);
        break;
    }

    case MD_BLOCK_UL:
        if (ctx->listDepth < OL_COUNTER_MAX) ++ctx->listDepth;
        break;

    case MD_BLOCK_OL: {
        auto* od = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        if (ctx->listDepth < OL_COUNTER_MAX) {
            ctx->olCounter[ctx->listDepth] = (int)od->start;
            ++ctx->listDepth;
        }
        break;
    }

    case MD_BLOCK_LI: {
        int indent = ctx->listDepth;
        if (indent > OL_COUNTER_MAX) indent = OL_COUNTER_MAX;
        ImGui::Dummy(ImVec2(indent * 16.0f, 0));
        ImGui::SameLine(0, 4);

        MD_BLOCKTYPE parent = ctx->parentBlockType();
        if (parent == MD_BLOCK_UL) {
            ImGui::Bullet();
            ImGui::SameLine(0, 4);
        } else if (parent == MD_BLOCK_OL && ctx->listDepth > 0 && ctx->listDepth <= OL_COUNTER_MAX) {
            int idx = ctx->listDepth - 1;
            ImGui::Text("%d.", ctx->olCounter[idx]++);
            ImGui::SameLine(0, 4);
        }
        break;
    }

    case MD_BLOCK_HR:
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
        break;

    case MD_BLOCK_TABLE: {
        auto* td = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
        ctx->tableCols = td->col_count;
        ctx->pushExtraId();
        ImGui::BeginTable("##tbl", td->col_count,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchSame);
        break;
    }
    case MD_BLOCK_THEAD:
        ctx->inTableHeader = true;
        break;
    case MD_BLOCK_TBODY:
        ctx->inTableHeader = false;
        break;
    case MD_BLOCK_TR:
        ImGui::TableNextRow();
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        ImGui::TableNextColumn();
        if (type == MD_BLOCK_TH)
            ctx->pushSpanColor(COLOR_HEADING);
        break;

    case MD_BLOCK_HTML:
    default:
        break;
    }

    ctx->pushBlock(type);
    return 0;
}

static int leaveBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<RenderCtx*>(userdata);
    (void)detail;

    switch (type) {

    case MD_BLOCK_DOC:
        ctx->flush();
        break;

    case MD_BLOCK_H:
        ctx->flush();
        ctx->popSpanColor();
        ImGui::SetWindowFontScale(1.0f);
        // Spacing after heading — matches old MdFormatCallback HEADING
        ImGui::Dummy(ImVec2(0.0f, 16.0f));
        break;

    case MD_BLOCK_P:
        ctx->flush();
        ImGui::Dummy(ImVec2(0, 2));
        break;

    case MD_BLOCK_QUOTE:
        ctx->flush();
        ctx->popSpanColor();
        break;

    case MD_BLOCK_CODE:
        ctx->flush();
        ctx->popSpanColor();
        ImGui::EndChild();
        ctx->popExtraColors(1);
        ctx->popExtraId();
        ctx->popExtraVars(2);
        break;

    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        ctx->flush();
        if (ctx->listDepth > 0) --ctx->listDepth;
        break;

    case MD_BLOCK_LI:
        ctx->flush();
        break;

    case MD_BLOCK_HR:
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        ctx->flush();
        if (type == MD_BLOCK_TH) ctx->popSpanColor();
        break;

    case MD_BLOCK_TABLE:
        ImGui::EndTable();
        ctx->popExtraId();
        ctx->tableCols = 0;
        break;

    default:
        break;
    }

    ctx->popBlock();
    return 0;
}

// --- Span callbacks ---

static int enterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<RenderCtx*>(userdata);

    switch (type) {
    case MD_SPAN_EM:
        ctx->flush();
        ctx->pushSpanColor(COLOR_EM);
        break;
    case MD_SPAN_STRONG:
        ctx->flush();
        ctx->pushSpanColor(COLOR_STRONG);
        break;
    case MD_SPAN_CODE:
        ctx->flush();
        ctx->pushSpanColor(COLOR_CODE_TEXT);
        break;
    case MD_SPAN_A:
        ctx->flush();
        ctx->pushSpanColor(COLOR_LINK);
        break;
    case MD_SPAN_DEL:
        ctx->flush();
        ctx->pushSpanColor(COLOR_DEL);
        break;
    case MD_SPAN_U:
        ctx->flush();
        ctx->pushSpanColor(COLOR_U);
        break;
    default:
        break;
    }

    return 0;
}

static int leaveSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto* ctx = static_cast<RenderCtx*>(userdata);
    (void)detail;

    // Underline for links — draw before flush so we know text position
    // (matches old MdFormatCallback LINK)
    if (type == MD_SPAN_A && !ctx->buf.empty()) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float tw = ImGui::CalcTextSize(ctx->buf.c_str()).x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(pos.x, pos.y + ImGui::GetTextLineHeight() + 1.0f),
                    ImVec2(pos.x + tw, pos.y + ImGui::GetTextLineHeight() + 1.0f),
                    ImGui::ColorConvertFloat4ToU32(COLOR_LINK_UL));
    }

    // Flush text with current (link/emphasis/...) colour before restoring
    ctx->flush();

    // Restore previous text colour
    switch (type) {
    case MD_SPAN_A:
    case MD_SPAN_EM:
    case MD_SPAN_STRONG:
    case MD_SPAN_CODE:
    case MD_SPAN_DEL:
    case MD_SPAN_U:
        ctx->popSpanColor();
        break;
    default:
        break;
    }

    return 0;
}

static int textCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto* ctx = static_cast<RenderCtx*>(userdata);

    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
        ctx->buf.append(text, size);
        break;
    case MD_TEXT_BR:
        ctx->flush();
        break;
    case MD_TEXT_SOFTBR:
        ctx->buf += ' ';
        break;
    case MD_TEXT_HTML:
        break;
    case MD_TEXT_ENTITY: {
        const ENTITY* ent = entity_lookup(text, size);
        if (ent && ent->codepoints[0]) {
            if (ent->codepoints[0] < 128 && !ent->codepoints[1])
                ctx->buf += (char)ent->codepoints[0];
            else
                ctx->buf += '?';
        }
        break;
    }
    case MD_TEXT_LATEXMATH:
        break;
    }

    return 0;
}

// ============================================================================
// Public API
// ============================================================================

void md4c_imgui_render(const std::string& text, float bubbleWidth,
                       Md4cLinkCallback linkCb, void* userdata)
{
    if (text.empty()) return;

    RenderCtx ctx;
    ctx.bubbleWidth = bubbleWidth;
    ctx.linkCb = linkCb;
    ctx.linkUserdata = userdata;

    MD_PARSER parser = {};
    parser.abi_version  = 0;
    parser.flags        = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH;
    parser.enter_block  = enterBlock;
    parser.leave_block  = leaveBlock;
    parser.enter_span   = enterSpan;
    parser.leave_span   = leaveSpan;
    parser.text         = textCallback;
    parser.debug_log    = nullptr;
    parser.syntax       = nullptr;

    md_parse(text.c_str(), (MD_SIZE)text.size(), &parser, &ctx);

    // --- Safety cleanup: ensure all push/pop stacks are balanced ---
    ImGui::SetWindowFontScale(1.0f);
    ctx.clearSpans();
    ctx.popExtraVars(ctx.pushVarCount);
    ctx.popExtraColors(ctx.pushColorCount);
    while (ctx.pushIdCount > 0) ctx.popExtraId();
    ctx.flush();
}
