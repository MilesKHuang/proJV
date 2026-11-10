// proJV TUI -- F4 MCP server panel implementation.
#include "mcp_view.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace ftxui;

namespace mcp_view {
namespace {

std::string selectedServer(TuiApp& app, int index) {
    mcp::Manager* m = app.mcpManager();
    if (!m) return std::string();
    auto names = m->serverNames();
    if (index < 0 || index >= (int)names.size()) return std::string();
    return names[index];
}

} // namespace

Component makeMcpDialog(TuiApp& app, std::function<void()> onClose) {
    auto entries = std::make_shared<std::vector<std::string>>();
    auto selected = std::make_shared<int>(0);

    MenuOption opt;
    opt.on_enter = [&app, selected] {
        if (app.isBusy()) return;
        mcp::Manager* m = app.mcpManager();
        if (!m) return;
        const std::string name = selectedServer(app, *selected);
        if (name.empty()) return;
        app.setMcpEnabled(name, !m->enabledOf(name));
    };

    Component menu = Menu(entries.get(), selected.get(), opt);

    Component restartBtn = Button("Restart", [&app, selected] {
        if (app.isBusy()) return;
        mcp::Manager* m = app.mcpManager();
        if (!m) return;
        const std::string name = selectedServer(app, *selected);
        if (!name.empty()) m->restart(name);
    });
    Component closeBtn = Button("Done", onClose);
    Component buttons = Container::Horizontal({restartBtn, closeBtn});
    Component container = Container::Vertical({menu, buttons});

    auto root = container | CatchEvent([&app, selected, onClose](Event e) {
                    if (e == Event::Escape) {
                        onClose();
                        return true;
                    }
                    if (e.is_character() && (e.character() == "r" || e.character() == "R")) {
                        if (!app.isBusy()) {
                            if (mcp::Manager* m = app.mcpManager()) {
                                const std::string name = selectedServer(app, *selected);
                                if (!name.empty()) m->restart(name);
                            }
                        }
                        return true;
                    }
                    return false;
                });

    return Renderer(root, [&app, entries, selected, menu, restartBtn, closeBtn] {
        mcp::Manager* m = app.mcpManager();

        // Rebuild the rows every frame: enabled/state/tool count all change
        // behind the panel's back (workers finish their handshake asynchronously).
        entries->clear();
        if (!m) {
            entries->push_back("(MCP manager not initialized)");
        } else {
            for (const auto& name : m->serverNames()) {
                const bool on = m->enabledOf(name);
                const bool trusted = m->trustedOf(name);
                std::string line = on ? "[x] " : "[ ] ";
                line += name;
                line += "  " + m->protocolOf(name);
                line += "  " + std::string(mcp::connStateName(m->connState(name)));
                const size_t tools = m->toolCount(name);
                if (tools > 0) line += "  " + std::to_string(tools) + " tools";
                if (!trusted) line += "  (needs \"trusted\": true)";
                entries->push_back(line);
            }
            if (entries->empty()) entries->push_back("(no servers in projv_files/mcp.json)");
        }
        if (*selected >= (int)entries->size()) *selected = (int)entries->size() - 1;
        if (*selected < 0) *selected = 0;

        Elements els;
        els.push_back(text("MCP Servers") | bold);
        els.push_back(separator());
        els.push_back(menu->Render() | frame | size(HEIGHT, LESS_THAN, 14));
        // Purpose of the selected server: the same line P3 gives the model, so what
        // the user sees in F4 and what the model is told cannot diverge.
        if (m && *selected >= 0 && *selected < static_cast<int>(entries->size())) {
            const std::string name = selectedServer(app, *selected);
            if (!name.empty()) {
                const std::string purpose = m->descriptionOf(name);
                if (!purpose.empty()) els.push_back(text(purpose) | dim);
                const std::string err = m->lastError(name);
                if (!err.empty()) els.push_back(text(err));
            }
        }
        els.push_back(separator());
        if (app.isBusy())
            els.push_back(text("busy -- finish the current turn first") | dim);
        else
            els.push_back(text("Enter toggle · R restart · Esc close") | dim);
        els.push_back(hbox({restartBtn->Render(), text("  "), closeBtn->Render()}));
        return vbox(std::move(els)) | border;
    });
}

} // namespace mcp_view
