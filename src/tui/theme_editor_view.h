// proJV TUI -- theme switch menu + theme editor (hex-input, like legacy manual entry).
#pragma once

#include <ftxui/component/component.hpp>

#include <functional>

namespace theme_editor {

// List builtin + installed themes and apply the selected one.
ftxui::Component makeThemeMenu(std::function<void()> onClose);

// Edit every color field as hex, with live preview, Apply and Save (export JSON).
ftxui::Component makeThemeEditor(std::function<void()> onClose);

} // namespace theme_editor
