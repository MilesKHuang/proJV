#pragma once

#include <imgui.h>
#include <string>
#include <vector>

// -----------------------------------------------------------------------
// Inline helper -- renders one line's worth of **bold**, *italic*, `code`.
// Updates x position and advances the pointer p past the end of the line.
// -----------------------------------------------------------------------
static void RenderInlineText(ImDrawList* dl, const char*& p, float& x, float y, ImFont* monoFont)
{
    const char* s = p;
    while (*s && *s != '\n')
    {
        if (*s == '`')
        {
            ++s;
            const char* start = s;
            while (*s && *s != '`') ++s;
            if (*s == '`')
            {
                ImFont* prev = ImGui::GetFont();
                ImGui::PushFont(monoFont ? monoFont : prev);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y),
                    IM_COL32(214, 157, 133, 255), start, s);
                x += ImGui::CalcTextSize(start, s).x;
                ImGui::PopFont();
                ++s;
            }
            else
            {
                dl->AddText(ImVec2(x, y), IM_COL32(200, 200, 200, 255), start - 1, s);
                x += ImGui::CalcTextSize(start - 1, s).x;
            }
        }
        else if (*s == '*' && *(s + 1) == '*')
        {
            s += 2;
            const char* start = s;
            while (*s && !(*s == '*' && *(s + 1) == '*')) ++s;
            if (*s == '*' && *(s + 1) == '*')
            {
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y),
                    IM_COL32(255, 200, 100, 255), start, s);
                x += ImGui::CalcTextSize(start, s).x;
                s += 2;
            }
            else
            {
                dl->AddText(ImVec2(x, y), IM_COL32(200, 200, 200, 255), start - 2, s);
                x += ImGui::CalcTextSize(start - 2, s).x;
            }
        }
        else if (*s == '*')
        {
            ++s;
            const char* start = s;
            while (*s && *s != '*') ++s;
            if (*s == '*')
            {
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y),
                    IM_COL32(150, 200, 255, 255), start, s);
                x += ImGui::CalcTextSize(start, s).x;
                ++s;
            }
            else
            {
                dl->AddText(ImVec2(x, y), IM_COL32(200, 200, 200, 255), start - 1, s);
                x += ImGui::CalcTextSize(start - 1, s).x;
            }
        }
        else
        {
            const char* start = s;
            while (*s && *s != '\n' && *s != '`' && *s != '*') ++s;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y),
                IM_COL32(200, 200, 200, 255), start, s);
            x += ImGui::CalcTextSize(start, s).x;
        }
    }
    p = s;
}

// -----------------------------------------------------------------------
// Render a finished code block at (startX, y). Returns the y after the
// block (including spacing).
// -----------------------------------------------------------------------
static float RenderCodeBlock(ImDrawList* dl, float startX, float y,
    float wrapWidth, float fontSize, ImFont* monoFont,
    const std::string& accum)
{
    // Split lines
    std::vector<const char*> lines;
    std::vector<size_t> lineLens;
    size_t pos = 0;
    while (pos < accum.size())
    {
        size_t nl = accum.find('\n', pos);
        if (nl == std::string::npos)
        {
            lines.push_back(accum.c_str() + pos);
            lineLens.push_back(accum.size() - pos);
            break;
        }
        lines.push_back(accum.c_str() + pos);
        lineLens.push_back(nl - pos);
        pos = nl + 1;
    }

    if (lines.empty())
        return y + fontSize * 0.5f;

    float lineH = (ImGui::GetFontSize()) * 1.3f;
    float bgH = (float)lines.size() * lineH + fontSize * 0.5f;
    dl->AddRectFilled(ImVec2(startX, y), ImVec2(startX + wrapWidth, y + bgH),
        IM_COL32(40, 40, 50, 255), 4.0f);

    float codeY = y + fontSize * 0.25f;
    if (monoFont) ImGui::PushFont(monoFont);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        dl->AddText(monoFont ? monoFont : ImGui::GetFont(),
            ImGui::GetFontSize(),
            ImVec2(startX + 8.0f, codeY),
            IM_COL32(220, 200, 180, 255), lines[i], lines[i] + lineLens[i]);
        codeY += lineH;
    }
    if (monoFont) ImGui::PopFont();

    return y + bgH + fontSize * 0.4f;
}

// -----------------------------------------------------------------------
// RenderMarkdown
// -----------------------------------------------------------------------
static ImVec2 RenderMarkdown(const char* text, const ImVec2& maxSize)
{
    if (!text || !*text)
        return ImGui::GetCursorScreenPos();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float fontSize = ImGui::GetFontSize();
    float wrapW = maxSize.x > 0.0f ? maxSize.x : 800.0f;

    // Find a monospace font in the atlas
    ImFont* monoFont = nullptr;
    for (ImFont* f : ImGui::GetIO().Fonts->Fonts)
    {
        if (f && strstr(f->GetDebugName(), "mono") != nullptr)
        { monoFont = f; break; }
    }

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float x0 = cursor.x;
    float y  = cursor.y;

    const char* p = text;
    bool inCode = false;
    std::string codeAccum;

    while (*p)
    {
        const char* lineStart = p;
        const char* lineEnd = p;
        while (*lineEnd && *lineEnd != '\n') ++lineEnd;
        std::string line(lineStart, lineEnd);
        p = (*lineEnd == '\n') ? lineEnd + 1 : lineEnd;

        // Code block fence
        if (line.size() >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`')
        {
            if (!inCode) { inCode = true; codeAccum.clear(); continue; }
            y = RenderCodeBlock(dl, x0, y, wrapW, fontSize, monoFont, codeAccum);
            inCode = false; continue;
        }
        if (inCode)
        {
            if (!codeAccum.empty()) codeAccum += '\n';
            codeAccum += line; continue;
        }

        // Strip leading whitespace for block detection
        const char* t = line.c_str();
        while (*t == ' ') ++t;
        int indent = (int)(t - line.c_str());
        const char* tend = line.c_str() + line.size();

        // Headers
        if (t[0] == '#' && t[1] == '#' && t[2] == '#' && t[3] == ' ')
        {
            dl->AddText(ImGui::GetFont(), fontSize * 1.15f, ImVec2(x0, y),
                IM_COL32(255, 220, 160, 255), t + 4, tend);
            y += fontSize * 1.4f; continue;
        }
        if (t[0] == '#' && t[1] == '#' && t[2] == ' ')
        {
            dl->AddText(ImGui::GetFont(), fontSize * 1.3f, ImVec2(x0, y),
                IM_COL32(255, 200, 120, 255), t + 3, tend);
            y += fontSize * 1.6f; continue;
        }
        if (t[0] == '#' && t[1] == ' ')
        {
            dl->AddText(ImGui::GetFont(), fontSize * 1.5f, ImVec2(x0, y),
                IM_COL32(255, 180, 80, 255), t + 2, tend);
            y += fontSize * 1.8f; continue;
        }

        // Unordered list
        if (t[0] == '-' && t[1] == ' ')
        {
            float bx = x0 + (float)indent * fontSize * 0.5f;
            dl->AddCircleFilled(ImVec2(bx + fontSize * 0.3f, y + fontSize * 0.45f),
                fontSize * 0.18f, IM_COL32(180, 220, 255, 255));
            float x = bx + fontSize * 0.7f;
            const char* cp = t + 2;
            RenderInlineText(dl, cp, x, y, monoFont);
            y += fontSize * 1.35f; continue;
        }

        // Numbered list
        {
            const char* ns = t;
            while (*ns && isdigit((unsigned char)*ns)) ++ns;
            if (ns > t && *ns == '.' && (ns[1] == ' ' || ns[1] == '\t'))
            {
                float lx = x0 + (float)indent * fontSize * 0.5f;
                std::string numStr(t, ns + 1);
                dl->AddText(ImGui::GetFont(), fontSize, ImVec2(lx, y),
                    IM_COL32(180, 255, 180, 255), numStr.c_str());
                const char* content = ns + 1;
                while (*content == ' ') ++content;
                float x = lx + ImGui::CalcTextSize(numStr.c_str()).x + fontSize * 0.3f;
                const char* cp = content;
                RenderInlineText(dl, cp, x, y, monoFont);
                y += fontSize * 1.35f; continue;
            }
        }

        // Empty line
        if (line.empty() || line.find_first_not_of(' ') == std::string::npos)
        {
            y += fontSize * 0.5f;
            continue;
        }

        // Paragraph
        float x = x0;
        const char* cp = line.c_str();
        RenderInlineText(dl, cp, x, y, monoFont);
        y += fontSize * 1.35f;
    }

    // Flush unterminated code block
    if (inCode && !codeAccum.empty())
        y = RenderCodeBlock(dl, x0, y, wrapW, fontSize, monoFont, codeAccum);

    return ImVec2(x0, y);
}
