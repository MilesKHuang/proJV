// proJV -- headless end-to-end harness (development only; not part of CI).
//
// Drives the real TuiApp against a fake DeepSeek API (build/e2e/fake_server.py)
// so the paths that normally need a human at the keyboard can be checked
// automatically: fixed context injection, the view_image round trip, /clear and
// F3. Prints one PASS/FAIL line per check and exits non-zero on any failure.
//
// argv[1] = workspace path, argv[2] = captures file written by the fake server.
#include "tui/app_tui.h"

#include "core/config.h"
#include "platform/iprocess_runner.h"
#include "platform/isystem_util.h"
#include "json.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& name, const std::string& detail = std::string()) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++g_failures;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}

void writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// Wait until the agent thread is idle again (turns run in the background).
bool waitIdle(TuiApp& app, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (app.isBusy()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

std::string systemText(const std::vector<Message>& msgs) {
    std::string all;
    for (const auto& m : msgs)
        if (m.role == "system") all += m.content + "\n";
    return all;
}

int countRole(const std::vector<Message>& msgs, const std::string& role) {
    int n = 0;
    for (const auto& m : msgs)
        if (m.role == role) ++n;
    return n;
}

// Are the first N messages system messages (the fixed prefix)?
bool fixedPrefixIsSystem(const std::vector<Message>& msgs, size_t n) {
    if (msgs.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (msgs[i].role != "system") return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: proJV_e2e <workspace> <captures.jsonl>\n");
        return 2;
    }
    const std::string ws = argv[1];
    const fs::path captures = argv[2];

    SystemUtil::Init(CreateSystemUtil());
    IProcessRunner* runner = CreateProcessRunner();

    TuiApp app;
    if (!app.initialize(runner)) {
        std::printf("FAIL  initialize() returned false\n");
        return 1;
    }

    // ---- 1. fixed context at startup -------------------------------------
    {
        auto msgs = app.getAgent()->getSession().getContextMessages();
        const std::string sys = systemText(msgs);
        check(sys.find("[PROJECT CONTEXT]") != std::string::npos,
              "startup: directory tree injected");
        check(sys.find("[AGENTS.MD]") != std::string::npos, "startup: AGENTS.md injected");
        check(sys.find("E2E-MARKER-BUILD-CMD") != std::string::npos,
              "startup: AGENTS.md content reaches the session");
        check(fixedPrefixIsSystem(msgs, 3), "startup: prompt + tree + AGENTS.md form the prefix");
    }

    // ---- 2. config parsing: official enum + fallback ---------------------
    {
        const fs::path dir = fs::path(ws);
        AppConfig c1;
        writeFile(dir / "c_none.toml", "reasoning_effort = \"none\"\n");
        loadTomlConfig(c1, (dir / "c_none.toml").string());
        check(c1.reasoningEffort == "none", "config: 'none' kept as the thinking-off value",
              c1.reasoningEffort);

        AppConfig c2;
        writeFile(dir / "c_off.toml", "reasoning_effort = \"off\"\n");
        loadTomlConfig(c2, (dir / "c_off.toml").string());
        check(c2.reasoningEffort == "high", "config: invalid value falls back to official 'high'",
              c2.reasoningEffort);

        AppConfig c3;
        writeFile(dir / "c_alias.toml", "reasoning_effort = \" ULTRA \"\n");
        loadTomlConfig(c3, (dir / "c_alias.toml").string());
        check(c3.reasoningEffort == "max", "config: alias ultra -> max", c3.reasoningEffort);
    }

    // ---- 3. view_image round trip (fake API) ----------------------------
    app.sendMessage("what is in the screenshot?");
    const bool idle = waitIdle(app, 60);
    check(idle, "turn: finished within 60s");

    // The captures file stores each request as {"n":..,"body":"<json string>"},
    // so the bodies have to be parsed back out before matching real JSON text.
    const std::string caps = readFile(captures);
    std::string bodies;
    int requests = 0;
    {
        std::ifstream in(captures);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line);
                bodies += j.value("body", std::string()) + "\n";
                ++requests;
            } catch (...) {
            }
        }
    }
    check(requests >= 2, "request: tool round + follow-up turn happened",
          "requests=" + std::to_string(requests));
    check(bodies.find("\"reasoning_effort\": \"max\"") != std::string::npos,
          "request: carries reasoning_effort = max");
    check(bodies.find("\"image_url\"") != std::string::npos,
          "request: the follow-up turn carries an image block");
    check(bodies.find("data:image/png;base64,") != std::string::npos,
          "request: image is inline base64");
    check(bodies.find("View the attached image(s)") != std::string::npos,
          "request: synthetic user message text present");

    app.syncChatFromAgent();
    {
        bool autoBubble = false;
        for (const auto& b : app.bubbles())
            if (b.synthetic && b.role == "user" && b.content.find("[Image] shot.png") !=
                                                         std::string::npos)
                autoBubble = true;
        check(autoBubble, "bubble: synthetic user bubble with [Image] shot.png");
    }

    // ---- 4. /clear keeps the database and re-injects the context --------
    app.sendMessage("/clear");
    waitIdle(app, 10);
    {
        auto msgs = app.getAgent()->getSession().getContextMessages();
        const std::string sys = systemText(msgs);
        check(countRole(msgs, "user") == 0, "/clear: no user message left in the context");
        check(sys.find("[PROJECT CONTEXT]") != std::string::npos,
              "/clear: directory tree re-injected");
        check(sys.find("[AGENTS.MD]") != std::string::npos, "/clear: AGENTS.md re-injected");
        check(fixedPrefixIsSystem(msgs, 3), "/clear: system prefix intact");
    }

    // ---- 5. F3 (new chat) re-injects everything -------------------------
    app.newChat();
    {
        auto msgs = app.getAgent()->getSession().getContextMessages();
        const std::string sys = systemText(msgs);
        check(countRole(msgs, "user") == 0, "F3: empty conversation");
        check(sys.find("[PROJECT CONTEXT]") != std::string::npos, "F3: directory tree re-injected");
        check(sys.find("[AGENTS.MD]") != std::string::npos, "F3: AGENTS.md re-injected");
        check(fixedPrefixIsSystem(msgs, 3), "F3: system prefix intact");
    }

    app.shutdown();
    delete runner;

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
