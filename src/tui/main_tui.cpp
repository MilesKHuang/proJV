// proJV TUI -- FTXUI event loop + App wiring (replaces the ImGui/GLFW main loop).
#include "terminal.h"
#include "app_tui.h"
#include "debug_log.h"
#include "chat_view.h"
#include "markdown_view.h"
#include "status_line.h"
#include "status_bar.h"
#include "speaker_colors.h"
#include "todo_view.h"
#include "config_view.h"
#include "mcp_view.h"
#include "theme_editor_view.h"
#include "theme_manager.h"
#include "theme_map.h"
#include "platform/isystem_util.h"
#include "platform/iprocess_runner.h"
#include "platform/install_shim.h"
#include "core/terminal_status.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/animation.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
    return std::make_shared<ObserveHeightNode>(ftxui::unpack(std::move(child)),
                                               std::move(on_height));
}

// Companion of ObserveHeightNode: reports the laid-out width in cells. The chat
// column is not necessarily the terminal width (the TODO dock takes 36), so the
// diff bubble has to be told what it actually got.
class ObserveWidthNode : public ftxui::Node {
  public:
    ObserveWidthNode(ftxui::Elements children, std::function<void(int)> on_width)
        : Node(std::move(children)), on_width_(std::move(on_width)) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        if (on_width_) on_width_(box.x_max - box.x_min);
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

  private:
    std::function<void(int)> on_width_;
};

ftxui::Element observeWidth(ftxui::Element child, std::function<void(int)> on_width) {
    return std::make_shared<ObserveWidthNode>(ftxui::unpack(std::move(child)),
                                              std::move(on_width));
}

// Map a subagent activity to the theme color of its state word (mirrors
// status_line::render for the main agent).
std::string ThemeColors::*panelActivityColor(SkillRunner::AgentActivity a) {
    switch (a) {
    case SkillRunner::AgentActivity::Working:
        return &ThemeColors::phaseStreaming;
    case SkillRunner::AgentActivity::Error:
        return &ThemeColors::statusError;
    case SkillRunner::AgentActivity::Idle:
    default:
        return &ThemeColors::statusIdle;
    }
}

const char* panelActivityLabel(SkillRunner::AgentActivity a) {
    switch (a) {
    case SkillRunner::AgentActivity::Working:
        return "working";
    case SkillRunner::AgentActivity::Error:
        return "error";
    case SkillRunner::AgentActivity::Idle:
    default:
        return "idle";
    }
}

// Truncate a UTF-8 string to at most `maxCols` terminal columns (CJK = 2
// columns, ASCII = 1). Appends ".." when clipped. Byte-safe on UTF-8.
std::string truncateToCols(const std::string& s, int maxCols) {
    int cols = 0;
    size_t i = 0, last = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        int w = 1;
        if (c >= 0xF0) {
            len = 4;
            w = 2;
        } else if (c >= 0xE0) {
            len = 3;
            w = 2;
        } else if (c >= 0xC0) {
            len = 2;
            w = 1;
        }
        if (cols + w > maxCols) break;
        cols += w;
        i += len;
        last = i;
    }
    std::string out = s.substr(0, last);
    if (last < s.size()) out += "..";
    return out;
}

// Right-dock Agent panel: two lines per live skill agent.
//   line 1: [name] <state> (+spinner) -- colored like the main status line
//   line 2: dim, truncated last narration (the agent's last bubble text)
ftxui::Element renderAgentPanel(const std::vector<SkillRunner::AgentSnapshot>& agents,
                                int spinnerFrame) {
    const auto& T = ThemeManager::instance().current();
    ftxui::Elements els;
    els.push_back(ftxui::text("Agents") | ftxui::bold);
    els.push_back(ftxui::separator());
    if (agents.empty()) {
        els.push_back(ftxui::text("(no skill running)") | ftxui::dim);
        return ftxui::vbox(std::move(els));
    }
    for (const auto& a : agents) {
        // Line 1: identity + state + live icon, tinted by activity.
        ftxui::Color stateColor = theme_map::hexToColor(T.*panelActivityColor(a.activity));
        ftxui::Color nameColor = speaker_colors::forRole(a.displayName);
        ftxui::Elements line1;
        line1.push_back(ftxui::text("[" + a.displayName + "]") | ftxui::bold |
                        ftxui::color(nameColor));
        line1.push_back(ftxui::text(" " + std::string(panelActivityLabel(a.activity))) |
                        ftxui::color(stateColor));
        if (a.activity == SkillRunner::AgentActivity::Working) {
            line1.push_back(ftxui::text(" "));
            line1.push_back(ftxui::spinner(6, spinnerFrame) | ftxui::color(stateColor));
        }
        els.push_back(ftxui::hbox(std::move(line1)));

        // Line 2: dim, truncated last narration.
        std::string st = a.status;
        auto nl = st.find('\n'); // first line only
        if (nl != std::string::npos) st = st.substr(0, nl);
        if (st.empty())
            st = (a.activity == SkillRunner::AgentActivity::Working) ? "working..." : "(idle)";
        els.push_back(ftxui::text(truncateToCols(st, 34)) | ftxui::dim |
                      ftxui::color(theme_map::hexToColor(T.textDisabled)));
    }
    return ftxui::vbox(std::move(els));
}

// Advances a spinner frame only while the agent is busy. FTXUI only calls
// OnAnimation when a frame was requested, so this component re-requests frames
// continuously during busy states and lets the animation go idle otherwise.
class SpinnerTicker : public ftxui::ComponentBase {
  public:
    SpinnerTicker(std::function<bool()> busy, std::function<void()> onTick)
        : busy_(std::move(busy)), onTick_(std::move(onTick)) {}

    ftxui::Element OnRender() override { return ftxui::text(""); }

    void OnAnimation(ftxui::animation::Params&) override {
        if (!busy_()) return;
        onTick_();
        ftxui::animation::RequestAnimationFrame();
    }

  private:
    std::function<bool()> busy_;
    std::function<void()> onTick_;
};

} // namespace

// Install / shim subcommands. They run before FTXUI
// takes over the terminal and never start the TUI: --install is used by people
// who unpacked a ZIP, --post-install by `cmake --install`. Returns -1 when the
// arguments are not an install command, so main() falls through to the TUI.
static int runInstallCommand(int argc, char** argv) {
    std::string command;
    std::string packageArg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--install" || arg == "--post-install" || arg == "--uninstall" ||
            arg == "--purge") {
            command = arg;
            continue;
        }
        if (command == "--install" && !arg.empty() && arg[0] != '-') packageArg = arg;
    }
    if (command.empty()) return -1;

    install_shim::Options opt = install_shim::current_options();
    if (!packageArg.empty()) opt.packageArg = packageArg;

    if (command == "--install") return install_shim::run_install(opt);
    if (command == "--post-install") return install_shim::run_post_install(opt);
    if (command == "--uninstall") return install_shim::run_uninstall(opt);
    return install_shim::run_purge(opt);
}

// MCP smoke test / pre-flight. Runs before FTXUI takes the terminal and never
// starts the TUI, so it can be scripted or run in CI:
//
//   proJV_tui --mcp-check
//       spawn every enabled server, wait for the handshake, print each server's
//       state and the tools it contributed. Exit 0 only when every enabled +
//       trusted server reached "ready".
//
//   proJV_tui --mcp-call <server> <tool> [key=value ...]
//       same, then call one tool and print the raw result text. Arguments are
//       key=value pairs (JSON-parsed when possible, string otherwise) because
//       nested JSON on a cmd.exe command line is not quotable in practice.
//
// Returns -1 when the arguments are not an MCP command, so main() falls through
// to the TUI.
static int runMcpCheck(int argc, char** argv) {
    bool wanted = false;
    std::string callServer, callTool;
    std::vector<std::string> callArgs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mcp-check") {
            wanted = true;
        } else if (arg == "--mcp-call") {
            wanted = true;
            if (i + 2 < argc) {
                callServer = argv[++i];
                callTool = argv[++i];
                while (i + 1 < argc) {
                    const std::string kv = argv[i + 1];
                    if (kv.find('=') == std::string::npos || kv.rfind("--", 0) == 0) break;
                    callArgs.push_back(kv);
                    ++i;
                }
            }
        }
    }
    if (!wanted) return -1;

    // Unbuffered: if a server misbehaves and takes the process down, the lines
    // printed so far must still reach the terminal.
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::string cfgPath;
    try {
        cfgPath = mcp::configPath();
    } catch (const std::exception& e) {
        // A CLI path must never die in std::terminate: say what was missing.
        fprintf(stderr, "[mcp] cannot resolve the config path: %s\n", e.what());
        return 2;
    }
    printf("[mcp] loading %s\n", cfgPath.c_str());

    mcp::Manager mgr;
    mgr.setWorkDir(getWorkspaceDir());
    mcp::ensureConfigFile(cfgPath);
    if (!mgr.loadFromJson(cfgPath)) {
        fprintf(stderr, "[mcp] config error: %s\n", mgr.configError().c_str());
        return 2;
    }
    if (mgr.empty()) {
        fprintf(stderr,
                "[mcp] no servers in %s\n"
                "      add one, e.g. {\"servers\":[{\"name\":\"time\",\"command\":\"uvx\","
                "\"args\":[\"mcp-server-time\"],\"trusted\":true,\"enabled\":true}]}\n",
                mcp::configPath().c_str());
        return 2;
    }

    mgr.startEnabled();
    printf("[mcp] starting %zu server(s), waiting for handshakes\n", mgr.serverNames().size());
    // Wait for every server we asked to start to settle (ready or dead).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;) {
        bool settled = true;
        for (const auto& name : mgr.serverNames()) {
            if (!mgr.enabledOf(name) || !mgr.trustedOf(name)) continue;
            const auto st = mgr.connState(name);
            if (st != mcp::ConnState::Ready && st != mcp::ConnState::Dead &&
                st != mcp::ConnState::Stopped)
                settled = false;
        }
        if (settled) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            fprintf(stderr, "[mcp] timed out waiting for servers to settle\n");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int rc = 0;
    printf("config: %s\n", cfgPath.c_str());
    for (const auto& name : mgr.serverNames()) {
        const auto st = mgr.connState(name);
        printf("\n%-16s state=%-12s protocol=%-9s tools=%zu spawns=%u\n", name.c_str(),
               mcp::connStateName(st), mgr.protocolOf(name).c_str(), mgr.toolCount(name),
               mgr.spawnCount(name));
        if (!mgr.trustedOf(name)) {
            printf("  skipped: trusted=false (set \"trusted\": true to allow it)\n");
            continue;
        }
        if (!mgr.enabledOf(name)) {
            printf("  skipped: enabled=false\n");
            continue;
        }
        if (st != mcp::ConnState::Ready) {
            printf("  FAILED: %s\n", mgr.lastError(name).c_str());
            rc = 1;
        }
        for (const auto& tool : mgr.toolNames(name)) printf("  - %s\n", tool.c_str());
    }

    if (!callServer.empty()) {
        nlohmann::json args = nlohmann::json::object();
        for (const auto& kv : callArgs) {
            const size_t eq = kv.find('=');
            const std::string key = kv.substr(0, eq);
            const std::string raw = kv.substr(eq + 1);
            nlohmann::json value;
            try {
                value = nlohmann::json::parse(raw);
            } catch (...) {
                value = raw;
            }
            args[key] = value;
        }
        printf("\ncall %s / %s %s\n", callServer.c_str(), callTool.c_str(), args.dump().c_str());
        const std::string r = mgr.callTool(callServer, callTool, args.dump(), nullptr);
        printf("result: %s\n", r.c_str());
        if (r.rfind("Error:", 0) == 0) rc = 1;
    }

    mgr.shutdownAll();
    return rc;
}

int main(int argc, char** argv) {
    const int installExit = runInstallCommand(argc, argv);
    if (installExit >= 0) return installExit;

    // SystemUtil is the only implementation of "where is my exe / config dir",
    // and Instance() THROWS when Init() has not run yet (isystem_util.cpp:14).
    // --mcp-check resolves projv_files/mcp.json through getProjvDir(), so the
    // platform layer has to be up BEFORE the headless path, not after it. That
    // ordering mistake is what made the first --mcp-check die with 0xC0000409
    // and not one line of output.
    ISystemUtil* sys = CreateSystemUtil();
    SystemUtil::Init(sys);

    const int mcpExit = runMcpCheck(argc, argv);
    if (mcpExit >= 0) return mcpExit;

    tui::initTerminal();
    IProcessRunner* procRunner = CreateProcessRunner();

#ifndef PROJV_RELEASE
    // Debug builds only: route loguru output to a file and silence stderr
    // (FTXUI owns the terminal; stray stderr writes corrupt the alternate
    // screen). Release builds compile in no logging AND must not create a
    // log file at all -- so skip loguru init entirely under PROJV_RELEASE.
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
    loguru::g_colorlogtostderr = false;
    std::string logPath = (std::filesystem::path(getExeDir()) / "proJV.log").string();
    loguru::add_file(logPath.c_str(), loguru::Append, loguru::Verbosity_MAX);
    debugLogf("[main] log file: %s", logPath.c_str());
#endif

    TuiApp app;
    app.initialize(procRunner);

    // Window close / Ctrl+C must still close the DB before the process is
    // reaped (FTXUI does not handle console close events on its own).
    SystemUtil::Instance().InstallExitHandler([&app]() { app.emergencyShutdown(); });

    auto screen = ScreenInteractive::Fullscreen();
    std::string input;
    bool show_config = false;
    bool show_approval = false;
    bool show_mcp = false;
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

    // Animated "working" spinner: advances only while the agent is busy.
    int spinnerFrame = 0;
    // Agent turns AND skill runs both share the busy flag, so the
    // spinner keeps animating during a debate (phase stays Idle then).
    auto busy = [&] { return app.isBusy(); };
    auto spinner_ticker =
        std::make_shared<SpinnerTicker>(busy, [&] { spinnerFrame = (spinnerFrame + 1) % 8; });

    // Idle-edge detector: plays a short system beep once a turn (or debate)
    // finishes and the app goes idle.
    bool prevBusy = false;

    // Model picker state (F11): names come from the API when available.
    auto model_names = std::make_shared<std::vector<std::string>>();
    auto model_selected = std::make_shared<int>(0);
    auto refresh_models = [&] {
        model_names->clear();
        auto models = app.getAvailableModels();
        if (!models.empty()) {
            for (auto& m : models)
                model_names->push_back(m.id);
        } else if (app.modelsLoading()) {
            model_names->push_back("(loading models...)");
        } else {
            model_names->push_back("deepseek-v4-flash");
            model_names->push_back("deepseek-v4-pro");
        }
        *model_selected = 0;
        for (int mi = 0; mi < static_cast<int>(model_names->size()); ++mi) {
            if ((*model_names)[mi] == app.getConfig().model) {
                *model_selected = mi;
                break;
            }
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
            return state.element; // placeholder
        }
        return markdown_view::renderSoftWrappedInput(input, input_cursor, state.focused) | yframe |
               size(HEIGHT, LESS_THAN, 8) | xflex;
    };
    Component input_comp =
        Input(&input, "type a message (Enter=send, Esc=quit, F2=config)", input_opt);

    input_comp |= CatchEvent([&](Event e) {
        if (e == Event::Return) {
            if (!input.empty()) {
                app.sendMessage(input);
                input.clear();
            }
            return true;
        }
        if (e == Event::Escape) {
            if (app.isBusy())
                app.cancelTurn();
            else
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
            show_mcp = true;
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

        // Short completion beep on the busy -> idle edge.
        {
            bool curBusy = app.isBusy();
            if (prevBusy && !curBusy) tui::beepIdle();
            prevBusy = curBusy;
        }

        // Bootstrap the spinner animation loop while the agent is busy.
        if (busy()) screen.RequestAnimationFrame();

        // Drive the approval modal from the agent phase.
        if (app.getPhase() == AgentPhase::AwaitApproval)
            show_approval = true;
        else if (show_approval && app.getPhase() != AgentPhase::AwaitApproval)
            show_approval = false;

        Elements els;
        els.push_back(text("proJV (TUI)") | bold);

        // Agent phase indicator (1:1 with renderChatArea's state machine line).
        {
            const char* phaseName = "?";
            std::string ThemeColors::*phaseColor = &ThemeColors::phaseIdle;
            switch (app.getPhase()) {
            case AgentPhase::Idle:
                phaseName = "Idle";
                phaseColor = &ThemeColors::phaseIdle;
                break;
            case AgentPhase::Streaming:
                phaseName = "Streaming";
                phaseColor = &ThemeColors::phaseStreaming;
                break;
            case AgentPhase::ExecutingTools:
                phaseName = "ExecutingTools";
                phaseColor = &ThemeColors::phaseExecutingTools;
                break;
            case AgentPhase::AwaitApproval:
                phaseName = "AwaitApproval";
                phaseColor = &ThemeColors::phaseAwaitApproval;
                break;
            case AgentPhase::Error:
                phaseName = "Error";
                phaseColor = &ThemeColors::phaseError;
                break;
            }
            const auto& T = ThemeManager::instance().current();
            els.push_back(text(" [Agent: " + std::string(phaseName) + "]") |
                          color(theme_map::hexToColor(T.*phaseColor)));
        }
        els.push_back(separator());

        // Chat area (row-granular scrolling via focusPosition).
        Element chat = chat_view::renderBubbles(app.bubbles(), app.reasoningExpanded(),
                                               app.chatContentWidth());

        // Live streaming: append the in-progress bubble.
        if (app.getPhase() == AgentPhase::Streaming) {
            auto st = app.getStatus();
            if (!st.streamingText.empty() || !st.reasoningText.empty()) {
                chat = vbox({chat, chat_view::renderStreamingBubble(st)});
            }
        }

        Element chatColumn =
            observeHeight(observeHeight(observeWidth(chat, [&](int cols) {
                                          app.setChatContentWidth(cols);
                                      }),
                                        [&](int rows) { app.setChatContentRows(rows); }) |
                              focusPosition(0, app.chatFocusRow()) | vscroll_indicator | yframe,
                          [&](int rows) { app.setChatViewportRows(rows); });

        // Right-side dock: TODO panel on top, live Agent status below
        // (legacy GUI layout, not a bottom strip).
        Element mainArea;
        if (show_todo) {
            Element dock = vbox({
                               todo_view::renderTodoPanel(app.copyTodoData()) | flex | yframe |
                                   vscroll_indicator,
                               separator(),
                               renderAgentPanel(app.skillAgentStatuses(), spinnerFrame),
                           }) |
                           size(WIDTH, EQUAL, 36);
            mainArea = hbox({
                std::move(chatColumn) | flex,
                separator(),
                std::move(dock),
            });
        } else {
            mainArea = std::move(chatColumn) | flex;
        }
        els.push_back(std::move(mainArea) | flex);
        els.push_back(separator());

        // Status line + spinner (outside the frame so the animation redraws).
        {
            auto stLine = status_line::render(app.getStatus());
            const auto& T = ThemeManager::instance().current();
            Element statusEl = text(stLine.text) | color(theme_map::hexToColor(T.*stLine.color));
            if (busy()) {
                statusEl = hbox({
                    statusEl,
                    text("  "),
                    spinner(6, spinnerFrame) | color(Color::RGB(224, 200, 96)),
                });
            }
            {
                std::string ds = app.getSkillStatus();
                if (!ds.empty()) {
                    statusEl = hbox({
                        statusEl,
                        text("  [skill] " + ds) |
                            color(theme_map::hexToColor(T.phaseAwaitApproval)),
                    });
                }
            }
            els.push_back(statusEl);
        }
        els.push_back(input_comp->Render());
        if (app.pendingCount() > 0) {
            els.push_back(text("Queued: " + std::to_string(app.pendingCount()) +
                               " message(s) - sends after current turn") |
                          color(Color::RGB(224, 200, 96)));
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
            Element role = text(sb.roleName.empty() ? "?" : sb.roleName) | bold |
                           color(theme_map::hexToColor(T.phaseStreaming)) |
                           bgcolor(theme_map::hexToColor(T.bubbleUserBg));
            segEls.push_back(text("  role: "));
            segEls.push_back(role);
            segEls.push_back(text("  (Tab)") | dim);
            els.push_back(xflex_grow(hbox(std::move(segEls))) |
                          bgcolor(theme_map::hexToColor(T.menuBarBg)));
        }
        {
            // Compact keycap hint bar: colored keycaps replace the long "F1 about"
            // labels and the - separators, so the line stays short.
            const auto& T = ThemeManager::instance().current();
            Color capFg = theme_map::hexToColor(T.text);
            Color capBg = theme_map::hexToColor(T.buttonActive);
            auto keycap = [&](const std::string& k) {
                return text(k) | bold | color(capFg) | bgcolor(capBg);
            };
            auto hint = [&](Element cap, const std::string& label) {
                return hbox({std::move(cap), text(" " + label) | dim});
            };
            els.push_back(hbox({
                // Single-space gaps: 13 entries now that F4 (mcp) is here, and the
                // row must still fit an 80...120 column terminal.
                hint(keycap("F1"), "help"),    text(" "), hint(keycap("F2"), "cfg"),   text(" "),
                hint(keycap("F3"), "new"),     text(" "), hint(keycap("F4"), "mcp"),   text(" "),
                hint(keycap("F5"), "open"),    text(" "), hint(keycap("F6"), "theme"), text(" "),
                hint(keycap("F7"), "edit"),    text(" "), hint(keycap("F8"), "think"), text(" "),
                hint(keycap("F9"), "todo"),    text(" "), hint(keycap("F10"), "model"), text(" "),
                hint(keycap("↑↓"), "scroll"),  text(" "), hint(keycap("Enter"), "send"), text(" "),
                hint(keycap("Esc"), "quit"),
            }));
        }
        return vbox(std::move(els));
    });

    // Keep the spinner ticker in the component tree so its OnAnimation runs.
    main_renderer->Add(spinner_ticker);

    Component config_dialog = config_view::makeConfigDialog(app, [&] { show_config = false; });
    Component approval_dialog =
        config_view::makeApprovalDialog(app, [&] { show_approval = false; });
    Component mcp_dialog = mcp_view::makeMcpDialog(app, [&] { show_mcp = false; });

    // F5: session picker (sessions live in a fixed directory).
    MenuOption open_opt;
    open_opt.on_enter = [&] {
        if (*session_selected >= 0 && *session_selected < (int)session_paths->size() &&
            !(*session_paths)[*session_selected].empty()) {
            app.switchToDialog((*session_paths)[*session_selected]);
        }
        show_open = false;
    };
    Component open_menu = Menu(session_names.get(), session_selected.get(), open_opt);
    open_menu |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            show_open = false;
            return true;
        }
        return false;
    });
    Component open_dialog = Renderer(open_menu, [open_menu] {
        return vbox({
                   text("Open Session") | bold,
                   separator(),
                   open_menu->Render() | frame | size(HEIGHT, LESS_THAN, 16),
                   text("Esc close · ↑↓ select · Enter open") | dim,
               }) |
               border;
    });
    Component theme_menu = theme_editor::makeThemeMenu([&] { show_theme = false; });
    Component theme_editor_dlg = theme_editor::makeThemeEditor([&] { show_theme_editor = false; });

    // F11: model picker (state + refresh lambda defined above).
    MenuOption model_opt;
    model_opt.on_enter = [&, model_names, model_selected] {
        if (*model_selected >= 0 && *model_selected < static_cast<int>(model_names->size()) &&
            (*model_names)[*model_selected] != "(loading models...)") {
            app.setModel((*model_names)[*model_selected]);
        }
        show_model = false;
    };
    Component model_menu = Menu(model_names.get(), model_selected.get(), model_opt);
    model_menu |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            show_model = false;
            return true;
        }
        return false;
    });
    Component model_dialog = Renderer(model_menu, [model_menu] {
        return vbox({
                   text("Model") | bold,
                   separator(),
                   model_menu->Render() | frame | size(HEIGHT, LESS_THAN, 14),
                   text("Esc close · ↑↓ select · Enter apply") | dim,
               }) |
               border;
    });

    // F1: about.
    Component about_close = Button("Close", [&] { show_about = false; });
    Component about_dialog = Renderer(about_close, [about_close] {
        return vbox({
                   text("About proJV") | bold,
                   separator(),
                   text("proJV v" PROJV_VERSION),
                   text("Native C++ DeepSeek AI agent (FTXUI)."),
                   separator(),
                   about_close->Render(),
               }) |
               border;
    });
    about_dialog |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            show_about = false;
            return true;
        }
        return false;
    });

    bool showWelcome = !app.hasApiKey();
    Component welcome = config_view::makeWelcome(app, [&] { showWelcome = false; });

    Component with_modals =
        Modal(Modal(Modal(Modal(Modal(Modal(Modal(Modal(main_renderer, config_dialog, &show_config),
                                                   approval_dialog, &show_approval),
                                             mcp_dialog, &show_mcp),
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
        // Only writer of the OSC 7501 status sequence: the UI thread, once per
        // frame, just before FTXUI flushes its own frame. The agent thread only
        // records state (terminal_status::working/blocked/...).
        terminal_status::flushIfDirty();
        const auto& T = ThemeManager::instance().current();
        return root->Render() | flex | bgcolor(theme_map::hexToColor(T.windowBg)) |
               color(theme_map::hexToColor(T.text));
    });

    screen.Loop(themed_root);

    app.shutdown();
    // Backstop for exit paths that skip TuiApp::shutdown (e.g. an exception
    // unwinding out of the loop): the host must not keep a zombie status.
    terminal_status::shutdown();
    delete procRunner;
    return 0;
}
