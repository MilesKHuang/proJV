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
//      TuiApp::runAgentTurnLoop). It is the single writer of ToolRegistry and
//      Agent::autoToolNames_, and it runs on the same thread that reads them,
//      so the registry needs no lock either.
//   4. callTool() only ever runs on the Agent thread, which executes tools
//      serially. Two calls can never be in flight on one server.
#pragma once

#include "client/models.h"
#include "core/agent.h"
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
    bool trustedOf(const std::string& name) const;
    bool enabledOf(const std::string& name) const;
    size_t toolCount(const std::string& name) const;    unsigned spawnCount(const std::string& name) const;
    std::vector<std::string> serverNames() const;
    // Registered names ("mcp__<server>__<tool>") this server contributed.
    std::vector<std::string> toolNames(const std::string& name) const;
    bool empty() const { return servers_.empty(); }

    // F4 Toggle: flip `enabled` in mcp.json and start/stop to match. The TUI
    // only calls this while idle (see mcp_view), so it cannot race a turn.
    void setEnabled(const std::string& name, bool enabled);

    // Second visibility channel: a short text block naming the Ready servers and
    // their tools, injected as a system message. Only Ready servers are listed:
    // naming a tool the model cannot call is worse than saying nothing.
    std::string buildContextSnippet() const;

    // Turn boundary, Agent thread only. Registers newly ready servers and drops
    // the tools of servers that stopped.
    void syncTools(ToolRegistry& registry, Agent& agent);

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
};

} // namespace mcp
