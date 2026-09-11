// proJV TUI -- FTXUI event loop + App wiring (replaces the ImGui/GLFW main loop).
#include "terminal.h"
#include "app_tui.h"
#include "chat_view.h"
#include "status_line.h"
#include "status_bar.h"
#include "todo_view.h"
#include "config_view.h"
#include "platform/isystem_util.h"
#include "platform/iprocess_runner.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>

using namespace ftxui;

int main() {
    tui::initTerminal();

    ISystemUtil* sys = CreateSystemUtil();
    SystemUtil::Init(sys);
    IProcessRunner* procRunner = CreateProcessRunner();

    TuiApp app;
    app.initialize(procRunner);

    auto screen = ScreenInteractive::Fullscreen();
    std::string input;
    bool show_config = false;
    bool show_approval = false;
    bool show_save = false;
    bool show_open = false;

    // Agent turns run on a background thread; post a Custom event so the
    // FTXUI loop redraws when a turn completes.
    app.onTurnComplete = [&]() { screen.PostEvent(Event::Custom); };

    InputOption input_opt;
    input_opt.multiline = false;
    Component input_comp = Input(&input, "type a message (Enter=send, Esc=quit, F2=config)", input_opt);

    input_comp |= CatchEvent([&](Event e) {
        if (e == Event::Return) {
            if (!input.empty()) {
                app.sendMessage(input);
                input.clear();
            }
            return true;
        }
        if (e == Event::Escape) {
            screen.Exit();
            return true;
        }
        if (e == Event::F2) {
            show_config = true;
            return true;
        }
        if (e == Event::F3) {
            app.newChat();
            return true;
        }
        if (e == Event::F4) {
            show_save = true;
            return true;
        }
        if (e == Event::F5) {
            show_open = true;
            return true;
        }
        return false;
    });

    auto main_renderer = Renderer(input_comp, [&] {
        app.syncChatFromAgent();

        // Drive the approval modal from the agent phase.
        if (app.getPhase() == AgentPhase::AwaitApproval) show_approval = true;
        else if (show_approval && app.getPhase() != AgentPhase::AwaitApproval) show_approval = false;

        Elements els;
        els.push_back(text("proJV (TUI)") | bold);
        els.push_back(separator());

        // Main area: chat + TODO side panel.
        Element chat = chat_view::renderBubbles(app.bubbles());

        // Live streaming: spinner animates and forces FTXUI to redraw.
        if (app.getPhase() == AgentPhase::Streaming) {
            auto st = app.getStatus();
            chat = vbox({
                chat,
                separator(),
                hbox({
                    text("Thinking... ") | color(Color::RGB(224, 200, 96)),
                    spinner(6, 0) | color(Color::RGB(224, 200, 96)),
                }),
            });
            if (!st.streamingText.empty() || !st.reasoningText.empty()) {
                chat = vbox({ chat, chat_view::renderStreamingBubble(st) });
            }
        }

        Element todo = todo_view::renderTodoPanel(app.copyTodoData());
        els.push_back(hbox({
            chat | flex,
            separator(),
            todo | size(WIDTH, EQUAL, 40),
        }));

        els.push_back(separator());
        els.push_back(text(status_line::render(app.getStatus())));
        els.push_back(input_comp->Render());
        els.push_back(separator());
        els.push_back(text(status_bar::render(app.getStatusBarData())) | dim);
        return vbox(std::move(els));
    });

    Component config_dialog = config_view::makeConfigDialog(app, [&] { show_config = false; });
    Component approval_dialog = config_view::makeApprovalDialog(app, [&] { show_approval = false; });
    Component save_dialog = config_view::makePathDialog("Save Chat As (.db)", [&](const std::string& p) {
        app.saveDialogToFile(p);
        show_save = false;
    }, [&] { show_save = false; });
    Component open_dialog = config_view::makePathDialog("Open Chat (.db)", [&](const std::string& p) {
        app.switchToDialog(p);
        show_open = false;
    }, [&] { show_open = false; });
    Component welcome = config_view::makeWelcome(app);

    Component with_modals = Modal(Modal(Modal(Modal(
        main_renderer, config_dialog, &show_config),
        approval_dialog, &show_approval),
        save_dialog, &show_save),
        open_dialog, &show_open);

    Component root = Renderer([&] {
        return app.hasApiKey() ? with_modals->Render() : welcome->Render();
    });

    screen.Loop(root);

    app.shutdown();
    delete procRunner;
    return 0;
}
