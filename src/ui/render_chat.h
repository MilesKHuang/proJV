#pragma once
// Chat rendering helpers extracted from app.cpp
// Contains: renderChatArea, renderInputArea, bubble helpers
// All static helper functions live in render_chat.cpp

#include <imgui.h>
#include <string>
#include <vector>
#include <deque>

struct ChatBubble; // forward decl from app.h
class App;         // forward decl

// RenderCopyButton: small inline helper for copying text
void RenderCopyButton(const char* label, const char* text);

// renderFormattedText: renders markdown + code blocks using md4c + ImGui
void renderFormattedText(const std::string& text, float bubbleWidth);


