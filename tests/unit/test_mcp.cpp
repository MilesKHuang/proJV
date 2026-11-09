// Tests for the MCP client: framing, schema pass-through, the auto-visible tool
// channel, and the resident server pool driven by a real child process.
//
// Contract being locked down (docs/temp/mcp-client.md section 11.2):
//   1. a registered MCP tool is visible WITHOUT being listed in the role prompt;
//   2. deleting the "tools available:" line still leaves every builtin usable --
//      the two visibility channels must not contaminate each other;
//   3. one process per server, reused across calls (resident, not per-call);
//   4. a server logging to stdout, a business error, and a protocol error are
//      three different things and must stay three different things;
//   5. shutdownAll() leaves no process behind, including grandchildren.
#include "doctest.h"

#include "client/deepseek.h"
#include "client/models.h"
#include "core/agent.h"
#include "core/mcp_config.h"
#include "core/mcp_manager.h"
#include "core/mcp_protocol.h"
#include "tools/registry.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

namespace {

// The fixture is built into the same directory as this test binary, so it is
// found by path rather than by a baked-in macro: a Windows path inside a string
// literal would have its backslashes eaten as escape sequences.
std::string fakeServerPath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    const std::filesystem::path self(std::wstring(buf, n));
    return (self.parent_path() / "fake_mcp_server.exe").string();
#else
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe");
    return (self.parent_path() / "fake_mcp_server").string();
#endif
}

std::string tempFile(const std::string& tag) {
    static int counter = 0;
    const auto dir = std::filesystem::temp_directory_path();
    return (dir / ("projv_mcp_" + tag + "_" + std::to_string(++counter) + ".json")).string();
}

// One server entry, built with nlohmann so no path needs manual escaping.
std::string configWith(const std::string& name, const std::string& mode, bool trusted,
                       bool enabled, int timeoutMs,
                       const std::vector<std::string>& extraArgs = {},
                       const std::string& protocol = "stateful") {
    nlohmann::json srv;
    srv["name"] = name;
    srv["command"] = fakeServerPath();
    srv["args"] = nlohmann::json::array({"--mode=" + mode});
    for (const auto& a : extraArgs) srv["args"].push_back(a);
    srv["protocol"] = protocol;
    srv["trusted"] = trusted;
    srv["enabled"] = enabled;
    srv["timeout_ms"] = timeoutMs;
    nlohmann::json root;
    root["servers"] = nlohmann::json::array({srv});
    return root.dump(2);
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

bool waitForState(mcp::Manager& m, const std::string& name, mcp::ConnState want,
                  int ms = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (m.connState(name) == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return m.connState(name) == want;
}

// Load one server, start it and wait until it is Ready.
bool bringUp(mcp::Manager& m, const std::string& mode, int timeoutMs = 20000,
             const std::vector<std::string>& extraArgs = {},
             const std::string& protocol = "stateful") {
    const std::string path = tempFile(mode);
    writeText(path, configWith("fake", mode, true, true, timeoutMs, extraArgs, protocol));
    m.setWorkDir(".");
    if (!m.loadFromJson(path)) return false;
    m.startEnabled();
    return waitForState(m, "fake", mcp::ConnState::Ready);
}

bool fileExists(const std::string& p) {
    return std::filesystem::exists(p);
}

std::string readText(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s, line;
    while (std::getline(f, line)) {
        if (!s.empty()) s += '\n';
        s += line;
    }
    return s;
}

#ifdef _WIN32
bool pidAlive(unsigned long pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}
#endif

// True when the pid stored in `pidFile` is still running.
bool pidFileEntryAlive(const std::string& pidFile) {
    const std::string text = readText(pidFile);
    if (text.empty()) return true; // unknown: do not fail the test on this
    unsigned long pid = 0;
    try {
        pid = std::stoul(text);
    } catch (...) {
        return true;
    }
    if (pid == 0) return true;
#ifdef _WIN32
    return pidAlive(pid);
#else
    return kill((pid_t)pid, 0) == 0;
#endif
}

} // namespace

TEST_CASE("mcp: newline framing classifies every message shape") {
    SUBCASE("response") {
        const auto m = mcp::parseLine(R"({"jsonrpc":"2.0","id":7,"result":{"ok":true}})");
        CHECK(m.parseOk);
        CHECK(m.isResponse);
        CHECK(m.id == 7);
    }
    SUBCASE("server-to-client request") {
        const auto m =
            mcp::parseLine(R"({"jsonrpc":"2.0","id":1,"method":"sampling/createMessage"})");
        CHECK(m.parseOk);
        CHECK(m.isRequest);
        CHECK(m.method == "sampling/createMessage");
    }
    SUBCASE("notification") {
        const auto m = mcp::parseLine(R"({"jsonrpc":"2.0","method":"notifications/progress"})");
        CHECK(m.parseOk);
        CHECK(m.isNotification);
        CHECK_FALSE(m.isResponse);
    }
    SUBCASE("a log line is not a protocol violation") {
        const auto m = mcp::parseLine("Starting server v1.2 ...");
        CHECK_FALSE(m.parseOk);
        CHECK_FALSE(m.why.empty());
    }
    SUBCASE("blank lines") {
        CHECK(mcp::isBlankLine(""));
        CHECK(mcp::isBlankLine("\r"));
        CHECK_FALSE(mcp::isBlankLine("{}"));
    }
}

TEST_CASE("mcp: stateless requests carry the three _meta keys, legacy ones do not") {
    const nlohmann::json params = nlohmann::json::object();
    const std::string modern = mcp::makeRequest(1, "tools/list", params, true, "9.9.9");
    const auto jm = nlohmann::json::parse(modern);
    REQUIRE(jm["params"].contains("_meta"));
    const auto& meta = jm["params"]["_meta"];
    CHECK(meta["io.modelcontextprotocol/protocolVersion"] == "2026-07-28");
    CHECK(meta.contains("io.modelcontextprotocol/clientCapabilities"));
    CHECK(meta["io.modelcontextprotocol/clientInfo"]["name"] == "proJV");

    const std::string legacy = mcp::makeRequest(1, "tools/list", params, false, "9.9.9");
    const auto jl = nlohmann::json::parse(legacy);
    CHECK_FALSE(jl["params"].contains("_meta"));
    // Framing: a request is always exactly one line.
    CHECK(modern.find('\n') == std::string::npos);
    CHECK(legacy.find('\n') == std::string::npos);
}

TEST_CASE("mcp: tool results are aggregated without binary payloads") {
    nlohmann::json content = nlohmann::json::array();
    content.push_back({{"type", "text"}, {"text", "line one"}});
    content.push_back({{"type", "text"}, {"text", "line two"}});
    content.push_back({{"type", "image"}, {"mimeType", "image/png"}});
    nlohmann::json result = {{"content", content}, {"isError", false}};

    std::string text, err;
    REQUIRE(mcp::aggregateToolResult(result, text, err));
    CHECK(text == "line one\nline two\n[image: image/png]");

    nlohmann::json missing = {{"resultType", "input_required"}};
    CHECK_FALSE(mcp::aggregateToolResult(missing, text, err));
    CHECK(err.find("MRTR") != std::string::npos);

    nlohmann::json structured = {{"structuredContent", 42}};
    REQUIRE(mcp::aggregateToolResult(structured, text, err));
    CHECK(text == "42");
}

TEST_CASE("mcp: registry prefix helpers keep builtins intact") {
    ToolRegistry reg;
    ToolDefinition mcpTool;
    mcpTool.name = "mcp__fs__read";
    mcpTool.description = "d";
    CHECK(reg.registerToolIfAbsent(mcpTool, [](const std::string&) { return std::string("1"); }));
    CHECK_FALSE(
        reg.registerToolIfAbsent(mcpTool, [](const std::string&) { return std::string("2"); }));

    ToolDefinition builtin;
    builtin.name = "read_file";
    builtin.description = "builtin";
    reg.registerTool(builtin, [](const std::string&) { return std::string("b"); });
    CHECK(reg.getToolDefinitions().size() == 2);

    CHECK(reg.unregisterByNamePrefix("mcp__fs__") == 1);
    CHECK(reg.getToolDefinitions().size() == 1);
    CHECK(reg.execute("read_file", "{}") == "b");
    CHECK(reg.execute("mcp__fs__read", "{}").find("Unknown tool") != std::string::npos);
}

TEST_CASE("mcp: an arbitrary input schema is forwarded verbatim") {
    ToolDefinition d;
    d.name = "nested";
    d.description = "d";
    d.parameters = {{"bad", "string", "ignored", false}};
    d.inputSchemaJson =
        R"({"type":"object","properties":{"cfg":{"type":"object"}},"required":["cfg"]})";
    CHECK(d.toJsonSchema() == d.inputSchemaJson);

    nlohmann::json api;
    to_json(api, d);
    CHECK(api["function"]["parameters"]["properties"]["cfg"]["type"] == "object");
    CHECK(api["function"]["parameters"]["required"][0] == "cfg");
    // The degraded path (a flat 4-type schema) must not be taken.
    CHECK_FALSE(api["function"]["parameters"].contains("bad"));
}

TEST_CASE("mcp: auto-registered tools are visible without the prompt listing them") {
    ToolRegistry reg;
    DeepSeekClient client;
    Agent agent(client, reg);
    // A prompt that lists something: the allowlist is non-empty, so anything not
    // named there is filtered out -- exactly the position an MCP tool is in.
    agent.setSystemPrompt("tools available: read_file\n");
    CHECK(agent.isToolAllowed("read_file"));
    CHECK_FALSE(agent.isToolAllowed("mcp__fake__echo"));

    agent.addAutoTools({"mcp__fake__echo"});
    CHECK(agent.isToolAllowed("mcp__fake__echo"));
    CHECK(agent.autoToolCount() == 1);

    agent.removeAutoTools({"mcp__fake__echo"});
    CHECK_FALSE(agent.isToolAllowed("mcp__fake__echo"));
}

TEST_CASE("mcp: an empty tools line still means every tool, as before") {
    ToolRegistry reg;
    DeepSeekClient client;
    Agent agent(client, reg);
    agent.setSystemPrompt("A prompt with no tools line at all.\n");
    // The pre-MCP escape hatch, untouched: an empty allowlist restricts nothing.
    CHECK(agent.isToolAllowed("read_file"));
    CHECK(agent.isToolAllowed("exec_shell"));
    CHECK(agent.isToolAllowed("mcp__anything__at_all"));
    // Registration is still the thing that decides whether a call can succeed.
    CHECK(reg.execute("mcp__anything__at_all", "{}").find("Unknown tool") != std::string::npos);
}

TEST_CASE("mcp: a non-empty prompt allowlist still lets auto tools through") {
    ToolRegistry reg;
    DeepSeekClient client;
    Agent agent(client, reg);
    agent.setSystemPrompt("tools available: read_file\n");
    CHECK(agent.isToolAllowed("read_file"));
    CHECK_FALSE(agent.isToolAllowed("exec_shell")); // filtered by the prompt
    agent.addAutoTools({"mcp__fake__echo"});
    CHECK(agent.isToolAllowed("mcp__fake__echo")); // bypasses the prompt filter
    CHECK_FALSE(agent.isToolAllowed("exec_shell"));
}

TEST_CASE("mcp: a stateful server stays resident and its tools register") {
    mcp::Manager m;
    REQUIRE(bringUp(m, "stateful"));

    ToolRegistry reg;
    DeepSeekClient client;
    Agent agent(client, reg);
    agent.setSystemPrompt("tools available: read_file\n");

    CHECK_FALSE(agent.isToolAllowed("mcp__fake__echo"));
    m.syncTools(reg, agent);
    CHECK(agent.isToolAllowed("mcp__fake__echo"));
    CHECK(agent.isToolAllowed("mcp__fake__nested"));
    CHECK(m.toolCount("fake") == 2);

    // Through the registry: the executor routes back into the same server.
    CHECK(reg.execute("mcp__fake__echo", R"({"msg":"hi"})") == "echo: hi");

    // Resident: a second call reuses the process instead of spawning a new one.
    CHECK(m.callTool("fake", "echo", R"({"msg":"again"})", nullptr) == "echo: again");
    CHECK(m.spawnCount("fake") == 1);

    // Second visibility channel: the snippet names the ready server's tools.
    const std::string snippet = m.buildContextSnippet();
    CHECK(snippet.find("[MCP available tools]") != std::string::npos);
    CHECK(snippet.find("mcp__fake__echo") != std::string::npos);

    m.shutdownAll();
    CHECK(m.connState("fake") == mcp::ConnState::Stopped);
    // A stopped server's tools must disappear at the next sync.
    m.syncTools(reg, agent);
    CHECK_FALSE(agent.isToolAllowed("mcp__fake__echo"));
    CHECK(reg.execute("mcp__fake__echo", "{}").find("Unknown tool") != std::string::npos);
}

TEST_CASE("mcp: a server logging to stdout is skipped, not fatal") {
    mcp::Manager m;
    REQUIRE(bringUp(m, "badline"));
    const std::string r = m.callTool("fake", "echo", R"({"msg":"hi"})", nullptr);
    CHECK(r.find("echo: hi") != std::string::npos);
    CHECK(r.find("[image: image/png]") != std::string::npos); // placeholder, never binary
    m.shutdownAll();
}

TEST_CASE("mcp: a business error stays a business error") {
    mcp::Manager m;
    REQUIRE(bringUp(m, "softfail"));
    // Twice: two "Error:" results in a row would disable tools for the turn.
    for (int i = 0; i < 2; ++i) {
        const std::string r = m.callTool("fake", "echo", R"({"msg":"hi"})", nullptr);
        CHECK(r.compare(0, 6, "Error:") != 0);
        CHECK(r == "fake / echo: cannot do that");
    }
    m.shutdownAll();
}

TEST_CASE("mcp: input_required is an error, never a silent success") {
    mcp::Manager m;
    REQUIRE(bringUp(m, "inputreq"));
    const std::string r = m.callTool("fake", "echo", R"({"msg":"hi"})", nullptr);
    CHECK(r.compare(0, 6, "Error:") == 0);
    CHECK(r.find("MRTR") != std::string::npos);
    m.shutdownAll();
}

TEST_CASE("mcp: stateless mode works with no handshake at all") {
    mcp::Manager m;
    REQUIRE(bringUp(m, "stateless", 20000, {}, "stateless"));
    CHECK(m.protocolOf("fake") == "stateless");
    CHECK(m.callTool("fake", "echo", R"({"msg":"hi"})", nullptr) == "echo: hi");
    m.shutdownAll();
}

TEST_CASE("mcp: a version rejection marks the server dead and is not retried") {
    const std::string path = tempFile("badmeta");
    writeText(path, configWith("fake", "badmeta", true, true, 8000));
    mcp::Manager m;
    m.setWorkDir(".");
    REQUIRE(m.loadFromJson(path));
    m.startEnabled();
    CHECK(waitForState(m, "fake", mcp::ConnState::Dead));
    CHECK(m.spawnCount("fake") == 1); // exactly one attempt
    CHECK(m.lastError("fake").find("protocol version") != std::string::npos);
    CHECK(m.callTool("fake", "echo", "{}", nullptr).compare(0, 6, "Error:") == 0);
    m.shutdownAll();
}

TEST_CASE("mcp: a slow call times out instead of blocking the turn") {
    mcp::Manager m;
    REQUIRE(bringUp(m, "slowcall", 1500));
    const std::string r = m.callTool("fake", "echo", R"({"msg":"hi"})", nullptr);
    CHECK(r.compare(0, 6, "Error:") == 0);
    CHECK(r.find("timed out") != std::string::npos);
    m.shutdownAll();
}

TEST_CASE("mcp: a server that never answers fails its handshake") {
    const std::string path = tempFile("hang");
    writeText(path, configWith("fake", "hang", true, true, 1200));
    mcp::Manager m;
    m.setWorkDir(".");
    REQUIRE(m.loadFromJson(path));
    m.startEnabled();
    CHECK(waitForState(m, "fake", mcp::ConnState::Dead, 12000));
    m.shutdownAll();
}

TEST_CASE("mcp: trusted=false spawns nothing at all") {
    const std::string path = tempFile("untrusted");
    writeText(path, configWith("fake", "stateful", false, true, 5000));
    mcp::Manager m;
    m.setWorkDir(".");
    REQUIRE(m.loadFromJson(path));
    m.startEnabled();
    CHECK(m.connState("fake") == mcp::ConnState::Stopped);
    CHECK(m.spawnCount("fake") == 0);
    CHECK(m.toolCount("fake") == 0);
    m.shutdownAll();
}

TEST_CASE("mcp: enabled=false is not started either") {
    const std::string path = tempFile("disabled");
    writeText(path, configWith("fake", "stateful", true, false, 5000));
    mcp::Manager m;
    REQUIRE(m.loadFromJson(path));
    m.startEnabled();
    CHECK(m.connState("fake") == mcp::ConnState::Stopped);
    CHECK(m.spawnCount("fake") == 0);
}

TEST_CASE("mcp: duplicate server names keep the first entry") {
    nlohmann::json first;
    first["name"] = "fake";
    first["command"] = fakeServerPath();
    first["args"] = nlohmann::json::array({"--mode=stateful"});
    nlohmann::json second = first;
    second["command"] = "definitely_not_a_real_program_xyz";
    nlohmann::json root;
    root["servers"] = nlohmann::json::array({first, second});

    const std::string path = tempFile("dup");
    writeText(path, root.dump(2));
    mcp::Manager m;
    REQUIRE(m.loadFromJson(path));
    CHECK(m.serverNames().size() == 1);
    CHECK(m.serverNames()[0] == "fake");
}

TEST_CASE("mcp: the config file is created empty on demand") {
    const std::string path = tempFile("ensure");
    std::filesystem::remove(path);
    REQUIRE(mcp::ensureConfigFile(path));
    CHECK(fileExists(path));
    std::vector<mcp::McpServerConfig> servers;
    std::string err;
    CHECK(mcp::loadConfig(path, servers, err));
    CHECK(servers.empty());
    std::filesystem::remove(path);
}

TEST_CASE("mcp: an invalid protocol value skips that server only") {
    nlohmann::json good;
    good["name"] = "ok";
    good["command"] = fakeServerPath();
    good["args"] = nlohmann::json::array({"--mode=stateful"});
    good["trusted"] = true;
    good["enabled"] = false;
    nlohmann::json bad = good;
    bad["name"] = "broken";
    bad["protocol"] = "carrier-pigeon";
    nlohmann::json root;
    root["servers"] = nlohmann::json::array({bad, good});

    const std::string path = tempFile("badproto");
    writeText(path, root.dump(2));
    mcp::Manager m;
    REQUIRE(m.loadFromJson(path));
    CHECK(m.serverNames().size() == 1);
    CHECK(m.serverNames()[0] == "ok");
}

TEST_CASE("mcp: shutdownAll reaps grandchild processes too") {
    // In fork mode the fake writes its grandchild's pid to --pidfile=..., so this
    // checks that the Job Object takes the whole tree down, not just the child
    // proJV spawned.
    const std::string pidFile = tempFile("forkpid");
    {
        const std::string path = tempFile("forkcfg");
        writeText(path,
                  configWith("fake", "fork", true, true, 20000, {"--pidfile=" + pidFile}));
        mcp::Manager m;
        m.setWorkDir(".");
        REQUIRE(m.loadFromJson(path));
        m.startEnabled();
        REQUIRE(waitForState(m, "fake", mcp::ConnState::Ready));
        for (int i = 0; i < 60 && !fileExists(pidFile); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(fileExists(pidFile));
    } // destructor -> shutdownAll()

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline && pidFileEntryAlive(pidFile))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(pidFileEntryAlive(pidFile));
}
