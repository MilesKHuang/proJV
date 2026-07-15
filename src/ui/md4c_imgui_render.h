#pragma once
#include <string>
#include <cstddef>

// --- md4c → ImGui render bridge ------------------------------------------
// Replaces imgui_markdown.h with md4c (CommonMark-compliant C parser).
// All rendering uses ImGui native widgets — no external markdown lib needed.

// Callback for link clicks. url is NOT null-terminated; urlLen gives length.
using Md4cLinkCallback = void(*)(const char* url, size_t urlLen, void* userdata);

// Render markdown text using md4c + ImGui native widgets.
// bubbleWidth: available content width (used for code blocks, quote borders).
// linkCb: optional. Called when user clicks a link.
void md4c_imgui_render(const std::string& text, float bubbleWidth,
                       Md4cLinkCallback linkCb = nullptr, void* userdata = nullptr);
