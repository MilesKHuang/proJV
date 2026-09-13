// proJV TUI -- FTXUI event loop + App wiring (replaces the ImGui/GLFW main loop).
#include "terminal.h"
#include "app_tui.h"
#include "chat_view.h"
#include "markdown_view.h"
#include "status_line.h"
#include "status_bar.h"
#include "todo_view.h"
#include "config_view.h"
#include "theme_editor_view.h"
#include "theme_manager.h"
#include "theme_map.h"
#include "platform/isystem_util.h"
#include "platform/iprocess_runner.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

using namespace ftxui;

namespace {

// Observes the laid-out box of the chat content and reports its height in rows.
// SetBox is the layout step where soft-wrap is already resolved, so this gives
// the real rendered row count. Unlike ftxui::reflect, Render() does not
// overwrite the captured box, so the height survives for the next frame's
// row-granular scrolling.
class ObserveHeightNode : public ftxui::Node {
public:
    ObserveHeightNode(ftxui::Elements children, std::function<void(int)> on_height)
        : Node(std::move(children)), on_height_(std::move(on_height)) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        if (on_height_) on_height_(box.y_max - box.y_min);
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

private:
    std::function<void(int)> on_height_;
};

ftxui::Element observeHeight(ftxui::Element child, std::function<void(int)> on_height) {
    return std::make_shared<ObserveHeightNode>(
        ftxui::unpack(std::move(child)), std::move(on_height));
}

} // namespace

int main() {
    tui::initTerminal();

    ISystemUtil* sys = CreateSystemUtil();
    SystemUtil::Init(sys);
    IProcessRunner* procRunner = CreateProcessRunner();

    TuiApp app;
    app.initialize(procRunner);

    // Window close / Ctrl+C must still close the DB before the process is
    // reaped (FTXUI does not handle console close events on its own).
    SystemUtil::Instance().InstallExitHandler([&app]() {
        app.emergencyShutdown();
    });

    auto screen = ScreenInteractive::Fullscreen();
    std::string input;
    bool show_config = false;
    bool show_approval = false;
    bool show_open = false;
    bool show_theme = false;
    bool show_theme_editor = false;
    bool show_todo = true;
    bool show_model = false;
    bool show_about = false;

    // Agent turns run on a background thread; post a Custom event on every
    // streamed token update and on turn completion so the FTXUI loop redraws
    // in real time (independent of animation/spinner behavior).
    app.onTurnComplete = [&]() { screen.PostEvent(Event::Custom); };
    app.onStreamingTick = [&]() { screen.PostEvent(Event::Custom); };
    app.onThemeChanged = [&]() { screen.PostEvent(Event::Custom); };

    // Model picker state (F11): names come from the API when available.
    auto model_names = std::make_shared<std::vector<std::string>>();
    auto model_selected = std::make_shared<int>(0);
    auto refresh_models = [&] {
        model_names->clear();
        auto models = app.getAvailableModels();
        if (!models.empty()) {
            for (auto& m : models) model_names->push_back(m.id);
        } else if (app.modelsLoading()) {
            model_names->push_back("(loading models...)");
        } else {
            model_names->push_back("deepseek-chat");
            model_names->push_back("deepseek-reasoner");
            model_names->push_back("deepseek-v4-flash");
            model_names->push_back("deepseek-v4-pro");
        }
        *model_selected = 0;
        for (int mi = 0; mi < static_cast<int>(model_names->size()); ++mi) {
            if ((*model_names)[mi] == app.getConfig().model) { *model_selected = mi; break; }
        }
    };
    refresh_models();
    app.onModelsUpdated = [&] {
        if (show_model) refresh_models();
        screen.PostEvent(Event::Custom);
    };

    // Session picker state (F5): display names + full paths.
    auto session_names = std::make_shared<std::vector<std::string>>();
    auto session_paths = std::make_shared<std::vector<std::string>>();
    auto session_selected = std::make_shared<int>(0);
    auto refresh_sessions = [&] {
        session_names->clear();
        session_paths->clear();
        for (auto& p : app.listSessions()) {
            session_paths->push_back(p);
            session_names->push_back(std::filesystem::path(p).filename().string());
        }
        if (session_names->empty()) {
            session_names->push_back("(no sessions)");
            session_paths->push_back("");
        }
        *session_selected = 0;
    };

    int input_cursor = 0;
    InputOption input_opt;
    input_opt.multiline = false;
    input_opt.cursor_position = Ref<int>(&input_cursor);
    input_opt.transform = [&input, &input_cursor](InputState state) -> Element {
        if (input.empty()) {
            return state.element;  // placeholder
        }
        return markdown_view::renderSoftWrappedInput(input, input_cursor, state.focused)
            | yframe | size(HEIGHT, LESS_THAN, 8) | xflex;
    };
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
            if (app.isBusy()) app.cancelTurn();
            else screen.Exit();
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
        if (e == Event::F5) {
            refresh_sessions();
            show_open = true;
            return true;
        }
        if (e == Event::F6) {
            show_theme = true;
            return true;
        }
        if (e == Event::F7) {
            show_theme_editor = true;
            return true;
        }
        if (e == Event::F8) {
            app.toggleReasoning();
            return true;
        }
        if (e == Event::F9) {
            show_todo = !show_todo;
            return true;
        }
        if (e == Event::F10) {
            app.refreshModels();
            refresh_models();
            show_model = true;
            return true;
        }
        if (e == Event::F1) {
            show_about = true;
            return true;
        }
        if (e == Event::Tab) {
            if (!app.isBusy()) {
                int n = static_cast<int>(app.promptFiles().size());
                if (n > 0) app.switchPrompt((app.activePromptIndex() + 1) % n);
            }
            return true;
        }
        if (e == Event::ArrowUp) {
            app.scrollChat(1);
            return true;
        }
        if (e == Event::ArrowDown) {
            app.scrollChat(-1);
            return true;
        }
        if (e == Event::PageUp) {
            app.scrollChat(10);
            return true;
        }
        if (e == Event::PageDown) {
            app.scrollChat(-10);
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

        // Agent phase indicator (1:1 with renderChatArea's state machine line).
        {
            const char* phaseName = "?";
            switch (app.getPhase()) {
                case AgentPhase::Idle:           phaseName = "Idle"; break;
                case AgentPhase::Streaming:      phaseName = "Streaming"; break;
                case AgentPhase::ExecutingTools: phaseName = "ExecutingTools"; break;
                case AgentPhase::AwaitApproval:  phaseName = "AwaitApproval"; break;
                case AgentPhase::Error:          phaseName = "Error"; break;
            }
            els.push_back(text(" [Agent: " + std::string(phaseName) + "]") | dim);
        }
        els.push_back(separator());

        // Chat area (row-granular scrolling via focusPosition).
        Element chat = chat_view::renderBubbles(app.bubbles(), app.reasoningExpanded());

        // Live streaming: append the in-progress bubble.
        if (app.getPhase() == AgentPhase::Streaming) {
            auto st = app.getStatus();
            if (!st.streamingText.empty() || !st.reasoningText.empty()) {
                chat = vbox({ chat, chat_view::renderStreamingBubble(st) });
            }
        }

        els.push_back(
            observeHeight(
                observeHeight(chat, [&](int rows) { app.setChatContentRows(rows); })
                    | focusPosition(0, app.chatFocusRow())
                    | vscroll_indicator | yframe,
                [&](int rows) { app.setChatViewportRows(rows); })
            | flex);
        els.push_back(separator());

        // Status line + spinner (outside the frame so the animation redraws).
        {
            Element statusEl = text(status_line::render(app.getStatus()));
            if (app.getPhase() == AgentPhase::Streaming) {
                statusEl = hbox({
                    statusEl,
                    text("  "),
                    spinner(6, 0) | color(Color::RGB(224, 200, 96)),
                });
            }
            els.push_back(statusEl);
        }
        els.push_back(input_comp->Render());
        if (app.pendingCount() > 0) {
            els.push_back(text("Queued: " + std::to_string(app.pendingCount()) + " message(s) — sends after current turn") | color(Color::RGB(224, 200, 96)));
        }
        els.push_back(separator());
        {
            auto sb = app.getStatusBarData();
            const auto& T = ThemeManager::instance().current();
            Elements segEls;
            for (const auto& seg : status_bar::renderSegments(sb)) {
                segEls.push_back(text(seg.text) | color(theme_map::hexToColor(T.*seg.color)));
            }
            // Role pill: user-bubble background with the streaming accent so the
            // active role pops against the status bar.
            Element role = text(sb.roleName.empty() ? "?" : sb.roleName)
                | bold
                | color(theme_map::hexToColor(T.phaseStreaming))
                | bgcolor(theme_map::hexToColor(T.bubbleUserBg));
            segEls.push_back(text("  role: "));
            segEls.push_back(role);
            segEls.push_back(text("  (Tab)") | dim);
            els.push_back(xflex_grow(hbox(std::move(segEls)))
                          | bgcolor(theme_map::hexToColor(T.menuBarBg)));
        }
        if (show_todo) {
            els.push_back(todo_view::renderTodoPanel(app.copyTodoData()) | size(HEIGHT, LESS_THAN, 6) | yframe);
        }
        {
            // Compact keycap hint bar: colored keycaps replace the long "F1 about"
            // labels and the · separators, so the line stays short.
            const auto& T = ThemeManager::instance().current();
            Color capFg = theme_map::hexToColor(T.text);
            Color capBg = theme_map::hexToColor(T.buttonActive);
            auto keycap = [&](const std::string& k) {
                return text(k) | bold | color(capFg) | bgcolor(capBg);
            };
            auto hint = [&](Element cap, const std::string& label) {
                return hbox({ std::move(cap), text(" " + label) | dim });
            };
            els.push_back(hbox({
                hint(keycap("F1"), "help"), text("  "),
                hint(keycap("F2"), "cfg"), text("  "),
                hint(keycap("F3"), "new"), text("  "),
                hint(keycap("F5"), "open"), text("  "),
                hint(keycap("F6"), "theme"), text("  "),
                hint(keycap("F7"), "edit"), text("  "),
                hint(keycap("F8"), "think"), text("  "),
                hint(keycap("F9"), "todo"), text("  "),
                hint(keycap("F10"), "model"), text("  "),
                hint(keycap("↑↓"), "scroll"), text("  "),
                hint(keycap("Enter"), "send"), text("  "),
                hint(keycap("Esc"), "quit"),
            }));
        }
        return vbox(std::move(els));
    });

    Component config_dialog = config_view::makeConfigDialog(app, [&] { show_config = false; });
    Component approval_dialog = config_view::makeApprovalDialog(app, [&] { show_approval = false; });

    // F5: session picker (sessions live in a fixed directory).
    MenuOption open_opt;
    open_opt.on_enter = [&] {
        if (*session_selected >= 0 && *session_selected < (int)session_paths->size()
            && !(*session_paths)[*session_selected].empty()) {
            app.switchToDialog((*session_paths)[*session_selected]);
        }
        show_open = false;
    };
    Component open_menu = Menu(session_names.get(), session_selected.get(), open_opt);
    open_menu |= CatchEvent([&](Event e) {
        if (e == Event::Escape) { show_open = false; return true; }
        return false;
    });
    Component open_dialog = Renderer(open_menu, [open_menu] {
        return vbox({
            text("Open Session") | bold,
            separator(),
            open_menu->Render() | frame | size(HEIGHT, LESS_THAN, 16),
            text("Esc close · ↑↓ select · Enter open") | dim,
        }) | border;
    });
    Component theme_menu = theme_editor::makeThemeMenu([&] { show_theme = false; });
    Component theme_editor_dlg = theme_editor::makeThemeEditor([&] { show_theme_editor = false; });

    // F11: model picker (state + refresh lambda defined above).
    MenuOption model_opt;
    model_opt.on_enter = [&, model_names, model_selected] {
        if (*model_selected >= 0 && *model_selected < static_cast<int>(model_names->size())
            && (*model_names)[*model_selected] != "(loading models...)") {
            app.setModel((*model_names)[*model_selected]);
        }
        show_model = false;
    };
    Component model_menu = Menu(model_names.get(), model_selected.get(), model_opt);
    model_menu |= CatchEvent([&](Event e) {
        if (e == Event::Escape) { show_model = false; return true; }
        return false;
    });
    Component model_dialog = Renderer(model_menu, [model_menu] {
        return vbox({
            text("Model") | bold,
            separator(),
            model_menu->Render() | frame | size(HEIGHT, LESS_THAN, 14),
            text("Esc close · ↑↓ select · Enter apply") | dim,
        }) | border;
    });

    // F1: about.
    Component about_close = Button("Close", [&] { show_about = false; });
    Component about_dialog = Renderer(about_close, [about_close] {
        return vbox({
            text("About proJV") | bold,
            separator(),
            text("proJV v0.5.0"),
            text("Native C++ DeepSeek AI agent (FTXUI)."),
            separator(),
            about_close->Render(),
        }) | border;
    });
    about_dialog |= CatchEvent([&](Event e) {
        if (e == Event::Escape) { show_about = false; return true; }
        return false;
    });

    bool showWelcome = !app.hasApiKey();
    Component welcome = config_view::makeWelcome(app, [&] { showWelcome = false; });

    Component with_modals = Modal(Modal(Modal(Modal(Modal(Modal(Modal(
        main_renderer, config_dialog, &show_config),
        approval_dialog, &show_approval),
        open_dialog, &show_open),
        theme_menu, &show_theme),
        theme_editor_dlg, &show_theme_editor),
        model_dialog, &show_model),
        about_dialog, &show_about);

    // Welcome page is a modal overlay while no API key is configured; once
    // saved, showWelcome flips false and the main UI takes focus. This keeps
    // the whole component tree active so keyboard events actually route.
    Component root = Modal(with_modals, welcome, &showWelcome);

    // Apply the theme background across the whole screen.
    Component themed_root = Renderer(root, [root] {
        const auto& T = ThemeManager::instance().current();
        return root->Render() | flex
             | bgcolor(theme_map::hexToColor(T.windowBg))
             | color(theme_map::hexToColor(T.text));
    });

    screen.Loop(themed_root);

    app.shutdown();
    delete procRunner;
    return 0;
}
