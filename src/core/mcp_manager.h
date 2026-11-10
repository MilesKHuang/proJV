// proJV -- MCP client: the resident server pool.
//
// One Manager owns every configured server. Each server is a child process
// proJV talks to over stdio, and it stays alive across turns.
//
// Threading contract. There is no cross-thread mutex in
// this class; the invariants below replace it:
//   1. start()/stop()/restart()/shutdownAll() are the ONLY places that spawn,
//      kill or join. Each server owns exactly one std::thread worker, and every
//      one of those entry points joins it before touching the process again.
//      Nothing is ever detached, so the thread can never outlive the handle.
//   2. The worker only ever writes its own handle (state / lastError / tools).
//      It never touches ToolRegistry.
//   3. syncTools() is called from the Agent thread at a turn boundary only (see
//      TuiApp::runAgentTurnLoop). It is the single writer of ToolRegistry, and
//      it runs on the same thread that reads it, so the registry needs no lock
//      either. It reports what it changed instead of reaching into the Agent:
//      this header must not include agent.h (it used to, which is what dragged
//      agent.h -> session.h -> storage.h -> SQLite into every MCP compile unit).
//   4. callTool() only ever runs on the Agent thread, which executes tools
//      serially. Two calls can never be in flight on one server.
//   5. reloadIfChanged() / reapRemoved() are MAIN thread, idle only (see
//      TuiApp::sendMessage, before the agent thread starts). They are the only
//      places that add to or erase from servers_, and the TUI render loop walks
//      that vector every frame -- so they must never run while a turn is in
//      flight. syncTools() runs only during a turn, so the two phases cannot
//      overlap and neither needs a mutex.
#pragma once

#include "client/models.h"
#include "core/mcp_config.h"
#include "core/mcp_process.h"
#include "tools/registry.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mcp {

enum class ConnState { Stopped, Connecting, Handshaking, Ready, Dead };

const char* connStateName(ConnState s);

// One configured server plus everything that lives with it.
struct ServerHandle {
    McpServerConfig cfg;
    // Held by the handle so the lifetime is obvious: start() joins the previous
    // worker before launching a new one, stop() joins it after cancelling.
    std::thread worker;
    std::atomic<ConnState> state{ConnState::Stopped};
    std::atomic<bool> cancelRequested{false};
    std::string lastError;               // worker -> UI, read without a lock
    std::vector<ToolDefinition> tools;   // snapshot from tools/list, worker writes
    std::vector<std::string> toolNames;  // registered names (mcp__<server>__<tool>)
    std::vector<std::string> registeredNames; // what is in the registry right now
    ProcHandles proc;
    long long nextId = 1;
    bool registered = false; // currently present in ToolRegistry
    unsigned spawnCount = 0; // diagnostics: proves a server is not respawned per call
    // Two-phase removal: reloadIfChanged() marks it and stops the process;
    // syncTools() unregisters its tools (it is the registry's only writer);
    // reapRemoved() finally erases the handle while the TUI is idle.
    bool removed = false;
};

class Manager {
  public:
    Manager() = default;
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    // Bind every server's cwd to the workspace proJV was started in (section
    // 6.4). Set once, before startEnabled().
    void setWorkDir(const std::string& ws) { workDir_ = ws; }
    const std::string& workDir() const { return workDir_; }

    bool loadFromJson(const std::string& path);
    const std::string& configError() const { return configError_; }
    const std::string& loadedPath() const { return configPath_; }

    // Spawn every enabled + trusted server. Returns immediately when a server is
    // spawned; the worker does handshake and tools/list.
    void startEnabled();
    void start(const std::string& name);
    void stop(const std::string& name);
    void restart(const std::string& name);
    void shutdownAll();

    ConnState connState(const std::string& name) const;
    std::string lastError(const std::string& name) const;
    std::string protocolOf(const std::string& name) const;
    // mcp.json "description": one line on what this server is for. Empty when the
    // entry omits it (loadConfig logs a warning).
    std::string descriptionOf(const std::string& name) const;
    bool trustedOf(const std::string& name) const;
    bool enabledOf(const std::string& name) const;
    size_t toolCount(const std::string& name) const;
    unsigned spawnCount(const std::string& name) const;
    std::vector<std::string> serverNames() const;

    // Read-only snapshot for the capability map (P3): name, purpose, and the
    // registered tool names, sorted by server name. Only Ready servers that are
    // actually registered are listed -- naming a tool the model cannot call is
    // worse than saying nothing. Agent thread, at a turn boundary (it reads
    // registeredNames, which only syncTools writes).
    struct ServerPurpose {
        std::string name;
        std::string description;
        std::vector<std::string> tools; // short names, prefix stripped
    };
    std::vector<ServerPurpose> serverPurposes() const;
    // Registered names ("mcp__<server>__<tool>") this server contributed.
    std::vector<std::string> toolNames(const std::string& name) const;
    bool empty() const { return servers_.empty(); }

    // F4 Toggle: flip `enabled` in mcp.json and start/stop to match. The TUI
    // only calls this while idle (see mcp_view), so it cannot race a turn.
    void setEnabled(const std::string& name, bool enabled);

    // What one syncTools() call changed in the registry. The caller owns the
    // second visibility channel (Agent::autoToolNames_) and applies it there.
    struct SyncResult {
        std::vector<std::string> added;   // registered this call
        std::vector<std::string> removed; // unregistered this call
    };

    // Turn boundary, Agent thread only. Registers newly ready servers, drops the
    // tools of servers that stopped, and reports both changes.
    SyncResult syncTools(ToolRegistry& registry);

    // Re-read `path` when its (mtime, size) snapshot moved, and apply the diff to
    // the resident handles. Main thread, idle only (see the contract above).
    // Returns true when a new config was applied -- even when no process moved,
    // because a description-only edit must still refresh the capability map.
    // A parse error keeps the current list untouched and only records
    // configError(), so a half-saved file can never take the servers down.
    bool reloadIfChanged(const std::string& path);

    // Drop handles that reloadIfChanged() marked removed once syncTools() has
    // unregistered their tools. Main thread, idle only. Returns how many went.
    size_t reapRemoved();

    // The tool executor body. Returns the text the model sees:
    //   success   -- the aggregated content
    //   business  -- "<server> / <tool>: <text>" (deliberately NOT "Error:",
    //                which the Agent reads as a tool failure)
    //   real      -- "Error: <server> / <tool>: <reason>"
    std::string callTool(const std::string& server, const std::string& tool,
                         const std::string& argsJson,
                         const std::function<bool()>& cancelPoll);

  private:
    ServerHandle* find(const std::string& name);
    const ServerHandle* find(const std::string& name) const;

    void workerMain(ServerHandle* h);
    void joinWorker(ServerHandle& h);
    void killProcess(ServerHandle& h);

    bool handshake(ServerHandle& h, std::string& err);
    bool listTools(ServerHandle& h, std::string& err);

    // Send one request and wait for the matching response. Used by both the
    // worker (handshake / tools/list) and callTool (Agent thread).
    bool requestResponse(ServerHandle& h, const std::string& method, nlohmann::json params,
                         const std::function<bool()>& cancelPoll, int timeoutMs,
                         nlohmann::json& result, std::string& err);

    // Turn buffered stderr lines into log lines. Never parsed as protocol data.
    void logStderr(ServerHandle& h, std::string& errBuf);

    static std::string prefixFor(const std::string& server);
    static std::string registeredName(const std::string& server, const std::string& tool);

    std::vector<std::unique_ptr<ServerHandle>> servers_;
    std::string configPath_;
    std::string configError_;
    std::string workDir_;
    // (mtime ticks, size) of mcp.json as of the last applied load. reloadIfChanged
    // compares against these and returns immediately when nothing moved: parsing
    // on every turn would be wasteful and a half-saved file would be destructive.
    long long configStamp_ = -1;
    unsigned long long configSize_ = 0;
    void rememberStamp(const std::string& path);
};

} // namespace mcp
