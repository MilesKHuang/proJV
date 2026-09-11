// proJV TUI -- config/welcome/approval dialog components.
#pragma once

#include "app_tui.h"

#include <ftxui/component/component.hpp>

#include <functional>

namespace config_view {

// Full configuration dialog (form fields + Save & Connect / Cancel).
ftxui::Component makeConfigDialog(TuiApp& app, std::function<void()> onClose);

// Welcome page shown when no API key is configured.
ftxui::Component makeWelcome(TuiApp& app);

// Approval dialog shown while the agent awaits tool approval.
ftxui::Component makeApprovalDialog(TuiApp& app, std::function<void()> onClose);

} // namespace config_view
