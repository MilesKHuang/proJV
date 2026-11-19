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
#include "slash_menu.h"
#include "paste_flatten.h"
#include "clipboard.h"
#include "ui_spinner.h"
#include "config_view.h"
#include "mcp_view.h"
#include "theme_editor_view.h"
#include "theme_manager.h"
#include "theme_map.h"
#include "platform/isystem_util.h"
#include "platform/iprocess_runner.h"
#include "platform/install_shim.h"
#include "platform/cli_args.h"
#include "platform/log_files.h"
#include "core/config.h"
#include "core/events.h"
#include "core/help_text.h"
#include "core/session_runtime.h"
#include "core/terminal_status.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/animation.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <chrono>
#include <csignal>
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
    return std::make_shared<ObserveWidthNode>(ftxui::unpack(std::move(child)), std::move(on_width));
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

// Colour of the spinner glyph itself, used by the status line. Green while work
// is happening: the theme's "executing tools" accent reads against the yellow
// body text, where the streaming yellow the glyph used to share disappeared.
// (The activity strip picks each agent's own colour instead; see renderUnit.)

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

// Insert clipboard text the way a paste behaves: line breaks and tabs become
// spaces (a pasted block must not send itself), control bytes are dropped, and an
// absurd paste is capped instead of pasted forever.
void insertPastedText(std::string& input, int& cursor, const std::string& clip) {
    std::string cleaned;
    cleaned.reserve(clip.size());
    for (const char c : clip) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\r' || c == '\n' || c == '\t') {
            cleaned += ' ';
        } else if (u >= 32 && u != 127) {
            cleaned += c; // UTF-8 continuation bytes are >= 0x80 and survive
        }
    }
    if (cleaned.size() > paste_flatten::kMaxFlattenChars)
        cleaned.resize(paste_flatten::kMaxFlattenChars);
    if (cleaned.empty()) return;
    if (cursor < 0 || static_cast<size_t>(cursor) > input.size())
        cursor = static_cast<int>(input.size());
    input.insert(static_cast<size_t>(cursor), cleaned);
    cursor += static_cast<int>(cleaned.size());
}

// Last line of a multi-paragraph narration: the strip must show what the agent
// is doing NOW, and the first line of a narration is the oldest thing in it.
std::string lastNarrationLine(const std::string& status) {
    std::string st = status;
    while (!st.empty() && (st.back() == '\n' || st.back() == '\r' || st.back() == ' '))
        st.pop_back();
    const auto nl = st.rfind('\n');
    if (nl != std::string::npos) st = st.substr(nl + 1);
    return st;
}

// One cell width for the activity strip and one for the dock. 39 + a 1 column
// gap puts exactly three cells on a 120 column terminal (3*39+2 = 119) and two
// on an 80 column one (2*39+1 = 79), which is what the layout is tuned for.
constexpr int kAgentUnitCols = 39;
constexpr int kDockUnitCols = 34;

// One cell: [name] <spinner-or-space> <narration>. The glyph column always
// exists, so an idle agent does not shift its neighbours left by one.
// The name is one colour for everybody -- eleven coloured names read as noise.
// Identity lives in the single-cell glyph (its own role colour), state lives
// there too (red on error, absent when idle), and an idle row fades to grey.
ftxui::Element renderUnit(const std::string& name, ftxui::Color identityColor,
                          SkillRunner::AgentActivity activity, const std::string& narration,
                          int unitCols, int spinnerFrame) {
    const auto& T = ThemeManager::instance().current();
    const bool idle = activity == SkillRunner::AgentActivity::Idle;
    const ftxui::Color nameColor = theme_map::hexToColor(idle ? T.textDisabled : T.text);
    const ftxui::Color glyphColor = activity == SkillRunner::AgentActivity::Error
                                        ? theme_map::hexToColor(T.statusError)
                                        : identityColor;
    const std::string tag = "[" + truncateToCols(name, 10) + "]";

    ftxui::Elements row;
    ftxui::Element nameElement = ftxui::text(tag) | ftxui::bold | ftxui::color(nameColor);
    if (idle) nameElement = nameElement | ftxui::dim;
    row.push_back(std::move(nameElement));
    row.push_back(ftxui::text(" "));
    if (idle) {
        row.push_back(ftxui::text(" "));
    } else {
        row.push_back(tui::spinner(spinnerFrame, glyphColor));
    }

    // Tags are ASCII ("Penny", "bg#3"), so byte count is column count here.
    const int room = unitCols - static_cast<int>(tag.size()) - 2;
    if (room > 0) {
        std::string detail = lastNarrationLine(narration);
        if (detail.empty()) detail = std::string(panelActivityLabel(activity)) + "...";
        row.push_back(ftxui::text(" " + truncateToCols(detail, room)) | ftxui::dim |
                      ftxui::color(theme_map::hexToColor(T.textDisabled)));
    }
    return ftxui::hbox(std::move(row)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, unitCols);
}

// Activity strip under the title bar: every live skill agent, wrapped by FTXUI
// when the terminal is narrow (flexbox re-measures on every layout, so a resize
// reflows without any code here). No sorting, no folding: the order is whatever
// SkillRunner::agentStatuses() returns, which is stable while a skill runs.
ftxui::Element renderAgentStrip(const std::vector<SkillRunner::AgentSnapshot>& agents,
                                int spinnerFrame) {
    // The empty state is one dim line, not an empty list: the chat area below
    // keeps its top edge when a skill starts instead of jumping four rows.
    if (agents.empty()) return ftxui::text("(no active agents)") | ftxui::dim;
    ftxui::Elements units;
    units.reserve(agents.size());
    for (const auto& a : agents)
        units.push_back(renderUnit(a.displayName, speaker_colors::forRole(a.displayName),
                                   a.activity, a.status, kAgentUnitCols, spinnerFrame));
    // hflow() takes Elements only and cannot carry a gap, so flexbox() is used
    // directly. Wrap is the default value but stays explicit: the strip's whole
    // point is that it wraps.
    return flexbox(std::move(units), FlexboxConfig().Set(FlexboxConfig::Wrap::Wrap).SetGap(1, 0));
}

// Dock bottom: one line per running background task. Separate from the strip so
// a running build and a running skill never look like the same thing.
ftxui::Element renderBackgroundTasks(const std::vector<bg_task::Snapshot>& tasks,
                                     int spinnerFrame) {
    ftxui::Elements els;
    for (const auto& t : tasks) {
        if (t.state != bg_task::State::Running) continue;
        // A background task has no role, so its glyph takes the working accent
        // instead of a speaker colour.
        const ftxui::Color tagColor =
            theme_map::hexToColor(ThemeManager::instance().current().phaseExecutingTools);
        els.push_back(
            renderUnit("bg#" + std::to_string(t.id), tagColor, SkillRunner::AgentActivity::Working,
                       t.lastLine.empty() ? t.command : t.lastLine, kDockUnitCols, spinnerFrame));
    }
    if (els.empty()) els.push_back(ftxui::text("(no background task)") | ftxui::dim);
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

    void OnAnimation(ftxui::animation::Params& p) override {
        if (!busy_()) return;
        // FTXUI posts an animation task every 15 ms (~66 fps). Advancing one
        // frame per callback made a revolution take 210 ms, which read as noise
        // rather than motion. Accumulate and emit a frame every 120 ms instead:
        // four glyphs at 120 ms = 480 ms per revolution.
        accum_ += std::chrono::duration_cast<std::chrono::milliseconds>(p.duration());
        if (accum_ < kFrameInterval) {
            ftxui::animation::RequestAnimationFrame(); // keep the loop alive
            return;
        }
        accum_ -= kFrameInterval; // subtract, never zero: no drift on slow frames
        onTick_();
        ftxui::animation::RequestAnimationFrame();
    }

  private:
    static constexpr std::chrono::milliseconds kFrameInterval{120};
    std::chrono::milliseconds accum_{0};
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
        for (const auto& tool : mgr.toolNames(name))
            printf("  - %s\n", tool.c_str());
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

// ---- `run`: the headless front-end (design 08) --------------------------
//
// No terminal is touched: no initTerminal, no FTXUI, no OSC 7501. Everything it
// does goes through SessionRuntime, so this function is a printer, nothing else.
namespace {

int runHeadless(const cli_args::RunOptions& o, IProcessRunner* runner) {
    if (o.schema) {
        std::fputs(events::schemaText().c_str(), stdout);
        return 0;
    }

    SessionRuntime rt;
    rt.setProcessRunner(runner);
    rt.setTerminalStatusEnabled(false); // OSC bytes would land inside the stream

    // Output: JSONL when asked, else a readable transcript. One flush per event --
    // a CI job that sees nothing until the end cannot watch progress.
    std::mutex outMutex;
    auto writeLine = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(outMutex);
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    };
    rt.onEvent = [&](const events::Event& e) {
        if (o.jsonl) {
            writeLine(events::toLine(e, !o.noTs));
            return;
        }
        switch (e.kind) {
        case events::Kind::TextDelta:
        case events::Kind::ThinkingDelta:
            writeLine(e.text);
            break;
        case events::Kind::ToolCall:
            writeLine("\n[tool] " + e.name + " " + e.text + "\n");
            break;
        case events::Kind::ToolResult:
            writeLine(std::string("[") + (e.ok ? "ok" : "error") + "] " + e.name + "\n");
            break;
        case events::Kind::Notice:
            writeLine(e.text + "\n");
            break;
        case events::Kind::Error:
            writeLine("[error] " + e.text + "\n");
            break;
        case events::Kind::TurnStart:
            writeLine("\n=== turn " + e.turn + " ===\n");
            break;
        default:
            break;
        }
    };

    if (!rt.initialize()) return 1;
    if (!rt.agent()) return 1;

    // There is no dialog here, so the dial is set before the first tool runs.
    // "always" == Loose: destructive commands run (the red line is still refused).
    const bool allow = (o.approve == "always");
    if (allow) rt.agent()->setApprovalMode(ApprovalMode::Loose);

    // --approve never / ask-without-a-terminal: answer every dialog the moment it
    // appears, from a second thread -- exactly what the TUI's button does. Without
    // this the agent thread would wait on a condition variable forever.
    std::atomic<bool> stopWatch{false};
    std::atomic<bool> refusedAny{false};
    std::thread watch;
    if (!allow) {
        watch = std::thread([&] {
            while (!stopWatch.load(std::memory_order_relaxed)) {
                if (rt.agent()->pendingApproval()) {
                    refusedAny.store(true, std::memory_order_relaxed);
                    rt.agent()->approveTool(0); // deny
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }

    if (o.dumpRequest) {
        auto dumped = std::make_shared<std::atomic<bool>>(false);
        rt.agent()->onRequestBody = [dumped](const std::string& body) {
            if (dumped->exchange(true)) return; // first request of the run only
            std::fprintf(stderr, "%s\n", body.c_str());
            std::fflush(stderr);
        };
    }

    int result = 0;
    if (rt.submit(o.prompt)) {
        int turns = 0;
        do {
            rt.run();
            ++turns;
        } while (turns < o.maxTurns && rt.pendingCount() > 0);
        if (rt.phase() == AgentPhase::Error) result = 1;
        if (rt.agent()->turnFailed()) result = 1;
    }

    stopWatch.store(true);
    if (watch.joinable()) watch.join();
    if (result == 0 && refusedAny.load() && !allow) result = 3;
    return result;
}

} // namespace

int main(int argc, char** argv) {
    // Meta commands first. --help / --version must not be swallowed by the
    // install branch below (it only looks for its own flags), and they must not
    // require the platform layer or the TUI to be up.
    {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i)
            args.emplace_back(argv[i]);
        std::string metaText;
        if (const auto metaExit = cli_args::parseMetaArgs(args, metaText)) {
            std::fputs(metaText.c_str(), stdout);
            std::fflush(stdout);
            return *metaExit;
        }
    }

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
    // Crash handling is the only way to get a stack out of a release build: the
    // handler writes projv_files/logs/crash-*.* and must be installed before the
    // TUI takes the terminal. --version/--help returned above, so they never
    // reach this line (they touch neither the session store nor the screen).
    sys->InstallCrashHandler();

    const int mcpExit = runMcpCheck(argc, argv);
    if (mcpExit >= 0) return mcpExit;

    IProcessRunner* procRunner = CreateProcessRunner();

    // Logging: one file per day under projv_files/logs, 7 days kept.
    //   release -> WARNING and above; debug -> everything.
    // The only difference between the two is a verbosity value: no call site is
    // compiled out, and loguru skips an INFO line with a single integer compare.
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF; // FTXUI owns the terminal
    loguru::g_colorlogtostderr = false;
    {
        const std::string logDir = getProjvDir() + "/logs";
        std::error_code logEc;
        std::filesystem::create_directories(logDir, logEc);
        if (!logEc) log_files::pruneOldLogs(logDir, 7);
        const std::string logPath = log_files::todayLogPath(logDir);
        const loguru::Verbosity fileLevel =
#ifdef PROJV_RELEASE
            loguru::Verbosity_WARNING;
#else
            loguru::Verbosity_MAX;
#endif
        loguru::add_file(logPath.c_str(), loguru::Append, fileLevel);
        debugLogInfof("[main] log file: %s (level %d)", logPath.c_str(), (int)fileLevel);
    }

    // `run`: the headless front-end (design 08). Handled after the platform layer
    // (it resolves projv_files and the workspace) and before initTerminal(): no
    // alternate screen, no raw mode, nothing that would corrupt a JSONL stream.
    {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i)
            args.emplace_back(argv[i]);
        if (cli_args::isRunCommand(args)) {
            std::string err;
            const auto opt = cli_args::parseRun(args, err);
            if (!opt) {
                std::fputs((err + "\n").c_str(), stderr);
                return 1;
            }
            return runHeadless(*opt, procRunner);
        }
    }

    tui::initTerminal();

    TuiApp app;
    app.initialize(procRunner);

    // A window close must still close the DB before the process is reaped
    // (FTXUI does not handle console close events on its own). Ctrl+C no longer
    // arrives here: system_util_win swallows it, and terminal.cpp clears
    // ENABLE_PROCESSED_INPUT so it never becomes a CTRL_C_EVENT at all.
    SystemUtil::Instance().InstallExitHandler([&app]() { app.emergencyShutdown(); });

    auto screen = ScreenInteractive::Fullscreen();
    std::string input;
    bool show_config = false;
    bool show_approval = false;
    bool show_mcp = false;
    bool show_open = false;
    bool show_theme = false;
    bool show_theme_editor = false;
    bool show_dock = true;
    bool show_model = false;
    bool show_help = false;
    bool show_quit = false;

    // "/" completion menu state (M2.5). slash_all is computed once: the skills
    // folder only changes with a restart-worthy action, and listSkills() is a
    // directory listing.
    bool show_slash = false;
    int slash_selected = 0;
    std::vector<slash_menu::Command> slash_items;
    std::vector<slash_menu::Command> slash_all =
        slash_menu::all(app.installedSkills(), app.skillsDir());

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
    auto spinner_ticker = std::make_shared<SpinnerTicker>(
        busy, [&] { spinnerFrame = (spinnerFrame + 1) % tui::kSpinnerFrames; });

    // Idle-edge detector: plays a short system beep once a turn (or debate)
    // finishes and the app goes idle.
    bool prevBusy = false;

    // Model picker state (F10): names come from the API when available.
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

    // Session picker state (F4): display names + full paths.
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
    input_opt.on_change = [&] {
        // Only a leading "/" with no space yet can be a command name. Typing the
        // first character of the arguments ("/wish a...") closes the menu, which is
        // exactly what the user means.
        const size_t sp = input.find_first_of(" \t");
        const bool bare = !input.empty() && input[0] == '/' && sp == std::string::npos;
        if (!bare || input.size() < 1) {
            show_slash = false;
            return;
        }
        slash_items = slash_menu::filter(slash_all, input);
        slash_selected = 0;
        show_slash = !slash_items.empty();
    };

    Component input_comp =
        Input(&input, "type a message (Enter=send, Esc=quit?, F2=config)", input_opt);

    // Paste detection: a multi-line paste arrives as one burst of key events, so
    // the Returns inside it have to become spaces instead of sending the first
    // line. Checked before every other branch -- the "/" menu also consumes
    // Return, and a pasted "/cmd\n..." would be swallowed by it.
    paste_flatten::BurstWindow pasteBurst;

    input_comp |= CatchEvent([&](Event e) {
        // Ctrl+C is a no-op: cancel is Esc's job, and a terminal with a text
        // selection never even delivers this event (the host keeps it for copy).
        // Idle hands still reach this branch, so it has to return true -- the
        // Input component would otherwise insert a raw 0x03.
        if (e == Event::CtrlC) return true;

        // Ctrl+V: conhost normally turns the key into a paste, but that translation
        // belongs to the console's processed-input path, which proJV switches off --
        // the key then arrives as a raw 0x16 and nothing pastes. Do the paste here
        // instead. A terminal that pastes on its own sends text and never 0x16, so
        // this cannot double-paste.
        if (e == Event::Special("\x16")) {
            std::string clip;
            if (clipboard::getText(clip)) {
                insertPastedText(input, input_cursor, clip);
                input_opt.on_change();
            }
            return true;
        }

        // Every event feeds the burst window, characters included: the state is
        // "how long since the previous key", so skipping characters would break
        // the chain in the middle of a paste. Only Return acts on it.
        {
            const bool inBurst = pasteBurst.tick(std::chrono::steady_clock::now());
            if (e == Event::Return && inBurst) {
                if (paste_flatten::shouldFlattenReturn(true, input.size())) {
                    // A byte offset, not a character count: the cursor is
                    // InputOption::cursor_position, a Ref<int> handed to FTXUI.
                    input.insert(static_cast<size_t>(input_cursor), " ");
                    ++input_cursor;
                    // Mutating `input` behind the Input component's back does not
                    // fire on_change, and the "/" menu would stay stale.
                    input_opt.on_change();
                }
                return true; // inside a paste: never send, never fall through
            }
        }

        // The "/" menu is handled next: Enter must fill the input, not send "/w",
        // and Escape must close the menu, not quit the program.
        if (show_slash && !slash_items.empty()) {
            const int n = static_cast<int>(slash_items.size());
            if (e == Event::ArrowUp) {
                slash_selected = (slash_selected + n - 1) % n;
                return true;
            }
            if (e == Event::ArrowDown) {
                slash_selected = (slash_selected + 1) % n;
                return true;
            }
            if (e == Event::Return) {
                input = slash_menu::completion(slash_items[slash_selected]);
                show_slash = false;
                return true;
            }
            if (e == Event::Escape) {
                show_slash = false;
                return true;
            }
        }
        if (e == Event::Return) {
            if (!input.empty()) {
                app.sendMessage(input);
                input.clear();
            }
            return true;
        }
        if (e == Event::Escape) {
            // Three meanings, decided by state rather than by a modifier key:
            // a running turn is cancelled, a typed line is cleared, and an idle
            // empty prompt asks before closing the app.
            if (app.isBusy()) {
                app.cancelTurn();
            } else if (!input.empty()) {
                input.clear();
                // The cursor lives outside the component (Ref<int>), so clearing
                // the string alone leaves it past the end of the buffer.
                input_cursor = 0;
                input_opt.on_change();
            } else {
                show_quit = true;
            }
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
        // F4 opens an old session, F5 opens MCP. The keys swapped, the variable
        // names did not: show_open / show_mcp name the dialog, not the key.
        if (e == Event::F4) {
            refresh_sessions();
            show_open = true;
            return true;
        }
        if (e == Event::F5) {
            show_mcp = true;
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
            show_dock = !show_dock;
            return true;
        }
        if (e == Event::F10) {
            app.refreshModels();
            refresh_models();
            show_model = true;
            return true;
        }
        if (e == Event::F1) {
            show_help = true;
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

        // Activity strip: agents used to be a right-dock block with two lines
        // each. A full-width wrapped row reads better and frees the dock for the
        // TODO list and the background tasks.
        els.push_back(renderAgentStrip(app.skillAgentStatuses(), spinnerFrame));
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

        Element chatColumn = observeHeight(
            observeHeight(observeWidth(chat, [&](int cols) { app.setChatContentWidth(cols); }),
                          [&](int rows) { app.setChatContentRows(rows); }) |
                focusPosition(0, app.chatFocusRow()) | vscroll_indicator | yframe,
            [&](int rows) { app.setChatViewportRows(rows); });

        // Right-side dock: TODO panel on top, running background tasks below.
        Element mainArea;
        if (show_dock) {
            Element dock = vbox({
                               todo_view::renderTodoPanel(app.copyTodoData()) | flex | yframe |
                                   vscroll_indicator,
                               separator(),
                               renderBackgroundTasks(app.taskSnapshots(), spinnerFrame),
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
                    tui::spinner(spinnerFrame, theme_map::hexToColor(T.phaseExecutingTools)),
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
        // The "/" menu sits directly above the input box: it belongs to the line
        // being typed, not to the chat.
        if (show_slash && !slash_items.empty()) {
            const auto& T = ThemeManager::instance().current();
            Elements menu;
            const int n = static_cast<int>(slash_items.size());
            // A scrolling window, not a hard cut: with 16 entries a fixed "first 8"
            // hid half the list behind a "... 8 more" line the arrow keys could not
            // reach. The window follows the selection instead.
            constexpr int kVisible = 10;
            int first = 0;
            if (n > kVisible) {
                first = slash_selected - (kVisible - 2);
                first = std::max(0, std::min(first, n - kVisible));
            }
            const int last = std::min(n, first + kVisible);
            if (first > 0)
                menu.push_back(text("  ^ " + std::to_string(first) + " more above") | dim);
            for (int i = first; i < last; ++i) {
                std::string cmd = "  " + slash_items[i].name;
                if (!slash_items[i].args.empty()) cmd += " " + slash_items[i].args;
                // Padding is on the command alone (ASCII, so bytes == columns); the
                // description gets its own element so it can be muted. One string
                // meant the command and its explanation competed for attention.
                while (cmd.size() < 34)
                    cmd += ' ';
                const std::string desc = slash_items[i].desc;
                Element row;
                if (i == slash_selected) {
                    // Inverted: the highlight carries the selection, the text stays
                    // plain so the inverted colours keep their contrast.
                    row = hbox({text(cmd) | bold, text(desc)}) | inverted |
                          color(theme_map::hexToColor(T.phaseStreaming));
                } else {
                    row = hbox({text(cmd) | bold,
                                text(desc) | color(theme_map::hexToColor(T.textDisabled))});
                }
                menu.push_back(std::move(row));
            }
            if (last < n)
                menu.push_back(text("  v " + std::to_string(n - last) + " more below") | dim);
            menu.push_back(text("  Enter = fill the command, Esc = close, Up/Down = move") | dim);
            els.push_back(vbox(std::move(menu)));
        }
        els.push_back(input_comp->Render());
        if (app.pendingCount() > 0) {
            // The todo title's yellow: both are "waiting for the model" notices.
            const auto& T = ThemeManager::instance().current();
            els.push_back(text("Queued: " + std::to_string(app.pendingCount()) +
                               " message(s) - sends after current turn") |
                          color(theme_map::hexToColor(T.todoTitle)));
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
            // Compact keycap hint bar: colored keycaps replace the long "F1 help"
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
                // Single-space gaps: 13 entries and about 112 columns, so the row
                // fits a 120 column terminal and loses its right end below that.
                hint(keycap("F1"), "help"),   text(" "), hint(keycap("F2"), "cfg"),     text(" "),
                hint(keycap("F3"), "new"),    text(" "), hint(keycap("F4"), "open"),    text(" "),
                hint(keycap("F5"), "mcp"),    text(" "), hint(keycap("F6"), "theme"),   text(" "),
                hint(keycap("F7"), "edit"),   text(" "), hint(keycap("F8"), "reveal"),  text(" "),
                hint(keycap("F9"), "dock"),   text(" "), hint(keycap("F10"), "model"),  text(" "),
                hint(keycap("↑↓"), "scroll"), text(" "), hint(keycap("Enter"), "send"), text(" "),
                hint(keycap("Esc"), "quit?"),
            }));
        }
        return vbox(std::move(els));
    });

    // Keep the spinner ticker in the component tree so its OnAnimation runs.
    main_renderer->Add(spinner_ticker);

    Component config_dialog = config_view::makeConfigDialog(app, [&] { show_config = false; });
    // Esc and F2 close it, always. Without this the only way out was the Cancel
    // button -- and if the dialog was taller than the terminal, that button was
    // clipped off the bottom, leaving no exit at all.
    config_dialog = config_dialog | CatchEvent([&](Event e) {
                        if (e == Event::Escape || e == Event::F2) {
                            show_config = false;
                            return true;
                        }
                        return false;
                    });
    Component approval_dialog =
        config_view::makeApprovalDialog(app, [&] { show_approval = false; });
    Component mcp_dialog = mcp_view::makeMcpDialog(app, [&] { show_mcp = false; });

    // F4: session picker (sessions live in a fixed directory).
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

    // F10: model picker (state + refresh lambda defined above).
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

    // F1: a starter card, not the reference. It lists only what the hint bar at
    // the bottom cannot show -- the "/" menu, "@" attachments, the three
    // meanings of Esc, and the two commands worth knowing on day one. /help still
    // prints every command and 08-reference still lists every key.
    // The Close button is load-bearing: a modal with no focusable child is not
    // Focused(), so ContainerBase::OnEvent drops every key before it reaches us
    // (see tests/unit/test_modal_focus.cpp). Return reaches the button, Esc and
    // F1 are caught before it.
    Component help_close = Button("Close", [&] { show_help = false; });
    Component help_dialog = Renderer(help_close, [&, help_close] {
        const auto& T = ThemeManager::instance().current();
        Elements rows;
        for (const auto& h : help_text::quickHints()) {
            rows.push_back(hbox({
                text(h.key) | bold | color(theme_map::hexToColor(T.phaseExecutingTools)) |
                    size(WIDTH, EQUAL, 8),
                text(h.desc),
            }));
        }
        return vbox({
                   text("Help (F1)") | bold,
                   separator(),
                   vbox(std::move(rows)),
                   separator(),
                   hbox({help_close->Render(), text("   Enter, Esc or F1 closes") | dim}),
               }) |
               size(WIDTH, LESS_THAN, 80) | border;
    });
    help_dialog |= CatchEvent([&](Event e) {
        if (e == Event::Escape || e == Event::F1) {
            show_help = false;
            return true;
        }
        return false;
    });

    // Esc on an empty prompt asks first. Cancel takes the initial focus and Esc
    // inside the box is the same as Cancel, so "get me out of here" never quits
    // by accident.
    Component quit_cancel = Button("Cancel", [&] { show_quit = false; });
    Component quit_ok = Button("Quit", [&] {
        show_quit = false;
        screen.Exit();
    });
    Component quit_buttons = Container::Horizontal({quit_cancel, quit_ok});
    Component quit_dialog = Renderer(quit_buttons, [quit_cancel, quit_ok] {
        return vbox({
                   text("Quit proJV?") | bold,
                   separator(),
                   text("The session is saved and the database is closed.") | dim,
                   separator(),
                   hbox({quit_cancel->Render(), text("  "), quit_ok->Render()}),
               }) |
               border;
    });
    quit_dialog |= CatchEvent([&](Event e) {
        // Esc is Cancel, and ONLY Esc: swallowing Return here made both buttons
        // dead (the key never reached the focused one), so picking "Quit" and
        // pressing Enter just closed the box. Return has to fall through to the
        // button; Cancel is the initially focused one, so Enter is still safe.
        if (e == Event::Escape) {
            show_quit = false;
            return true;
        }
        return false;
    });

    bool showWelcome = !app.hasApiKey();
    Component welcome = config_view::makeWelcome(app, [&] { showWelcome = false; });
    // Without an API key this overlay owns every key: the input box below never
    // sees them, so its Esc branch cannot run. Ctrl+C is a no-op by design now,
    // which would leave a first-run user with no keyboard way out -- Esc quits.
    welcome |= CatchEvent([&](Event e) {
        if (e == Event::Escape) {
            screen.Exit();
            return true;
        }
        return false;
    });

    Component with_modals =
        Modal(Modal(Modal(Modal(Modal(Modal(Modal(Modal(main_renderer, config_dialog, &show_config),
                                                  approval_dialog, &show_approval),
                                            mcp_dialog, &show_mcp),
                                      open_dialog, &show_open),
                                theme_menu, &show_theme),
                          theme_editor_dlg, &show_theme_editor),
                    model_dialog, &show_model),
              help_dialog, &show_help);

    // Welcome page is a modal overlay while no API key is configured; once
    // saved, showWelcome flips false and the main UI takes focus. This keeps
    // the whole component tree active so keyboard events actually route.
    // The quit confirmation wraps everything, welcome included: Modal nests as
    // Container::Tab, so the outermost layer is the one that draws on top and
    // gets the keys first.
    Component root = Modal(Modal(with_modals, welcome, &showWelcome), quit_dialog, &show_quit);

    // Ctrl+C must not be able to end the process in any focus state.
    // FTXUI records SIGINT for an UNHANDLED CtrlC even with ForceHandleCtrlC(false)
    // -- app.cpp:900 is `if (arg == Event::CtrlC && (!handled ||
    // force_handle_ctrl_c_))`, where `!handled` alone is enough -- and the next
    // frame turns that record into Signal(SIGABRT) -> Exit(). Every modal (help,
    // config, MCP, session picker, theme, model, quit, welcome, approval) takes the
    // keyboard away from the input box, so leaving the swallow to input_comp meant
    // Ctrl+C still exited whenever a dialog was open. One wrapper around the whole
    // tree makes it an invariant; the branch inside input_comp stays as
    // documentation of the intent.
    root |= CatchEvent([](Event e) { return e == Event::CtrlC; });

    // Apply the theme background across the whole screen.
    Component themed_root = Renderer(root, [root] {
        // Once, after FTXUI's Install: make Ctrl+C inert for good.
        static bool signalsIgnored = false;
        if (!signalsIgnored) {
            signalsIgnored = true;
#ifdef _WIN32
            // App::Install() (inside Loop) registers a CRT handler with
            // std::signal(SIGINT, ...). Console handlers run LIFO, so that one runs
            // BEFORE the handler in system_util_win and turns a real CTRL_C_EVENT
            // into RecordSignal(SIGINT) -> Signal(SIGABRT) -> Exit(). Replacing it
            // with SIG_IGN here, after Install, is what actually keeps Ctrl+C from
            // ending the process; a window close still exits through the console
            // handler, which owns CTRL_CLOSE_EVENT.
            std::signal(SIGINT, SIG_IGN);
            std::signal(SIGBREAK, SIG_IGN);
#endif
        }
        // Only writer of the OSC 7501 status sequence: the UI thread, once per
        // frame, just before FTXUI flushes its own frame. The agent thread only
        // records state (terminal_status::working/blocked/...).
        terminal_status::flushIfDirty();
        const auto& T = ThemeManager::instance().current();
        return root->Render() | flex | bgcolor(theme_map::hexToColor(T.windowBg)) |
               color(theme_map::hexToColor(T.text));
    });

    // Mouse tracking off: the hosting terminal keeps the selection, which is the
    // only copy path left now that Ctrl+C does nothing inside proJV.
    // ForceHandleCtrlC(false): FTXUI records SIGINT for Event::CtrlC even when a
    // component catches it (force_handle_ctrl_c_ defaults to true), and the next
    // frame turns that record into Exit(). Both calls are public API and must
    // happen before Loop().
    screen.TrackMouse(false);
    screen.ForceHandleCtrlC(false);
    // Startup spawns child processes (python detection, skill setup, MCP
    // handshakes) and one of them resets the console input mode. Re-assert it now
    // that startup is done, so the mode the loop runs under is the one
    // initTerminal() chose.
    tui::initTerminal();

    screen.Loop(themed_root);

    app.shutdown();
    // Backstop for exit paths that skip TuiApp::shutdown (e.g. an exception
    // unwinding out of the loop): the host must not keep a zombie status.
    terminal_status::shutdown();
    delete procRunner;
    return 0;
}
