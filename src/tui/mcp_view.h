// proJV TUI -- F4 MCP server panel.
#pragma once

#include "app_tui.h"

#include <ftxui/component/component.hpp>

#include <functional>

namespace mcp_view {

// One row per configured server: on/off, protocol mode, connection state and how
// many tools it contributed. Enter toggles, R restarts, Esc closes.
// All actions are refused while a turn is running (see TuiApp::setMcpEnabled):
// starting or stopping a server writes the tool table, and that is only legal at
// a turn boundary.
ftxui::Component makeMcpDialog(TuiApp& app, std::function<void()> onClose);

} // namespace mcp_view
