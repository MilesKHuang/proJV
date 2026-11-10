#include "core/mcp_manager.h"

#include "core/mcp_protocol.h"
#include "debug_log.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>

namespace mcp {
namespace {

namespace fs = std::filesystem;

std::string describeError(const nlohmann::json& e) {
    if (!e.is_object()) return e.dump();
    std::string msg = e.value("message", std::string("protocol error"));
    if (e.contains("code") && e["code"].is_number_integer())
        msg += " (code " + std::to_string(e["code"].get<int>()) + ")";
    return msg;
}

// One line, bounded: a tool description with newlines would break the injected
// context block, and a 4KB description would waste the model's budget.
std::string oneLine(const std::string& s, size_t maxLen) {
    std::string out;
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') {
            if (out.empty() || out.back() != ' ') out.push_back(' ');
        } else {
            out.push_back(c);
        }
        if (out.size() >= maxLen) {
            out += "...";
            break;
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// (mtime ticks, size) of a config file. Both together are the "did the user touch
// it" test: mtime alone can repeat on filesystems with coarse timestamps.
bool readFileStamp(const std::string& path, long long& ticks, unsigned long long& size) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return false;
    ticks = static_cast<long long>(t.time_since_epoch().count());
    const auto s = fs::file_size(path, ec);
    size = ec ? 0ull : static_cast<unsigned long long>(s);
    return true;
}

// Everything that decides how the child process is spawned. `enabled` is handled
// separately (it is a start/stop toggle, not a redefinition), and `description`
// is excluded on purpose: changing the purpose text must not restart a process.
std::string fingerprint(const McpServerConfig& c) {
    std::string f = c.command;
    f += '\x1f';
    for (const auto& a : c.args) {
        f += a;
        f += '\x1f';
    }
    for (const auto& kv : c.env) {
        f += kv.first;
        f += '=';
        f += kv.second;
        f += '\x1f';
    }
    f += c.protocol;
    f += '\x1f';
    f += c.trusted ? '1' : '0';
    f += '\x1f';
    f += std::to_string(c.timeoutMs);
    return f;
}

} // namespace

const char* connStateName(ConnState s) {
    switch (s) {
    case ConnState::Stopped:
        return "stopped";
    case ConnState::Connecting:
        return "connecting";
    case ConnState::Handshaking:
        return "handshaking";
    case ConnState::Ready:
        return "ready";
    case ConnState::Dead:
        return "dead";
    }
    return "?";
}

std::string Manager::prefixFor(const std::string& server) {
    return "mcp__" + server + "__";
}

std::string Manager::registeredName(const std::string& server, const std::string& tool) {
    return prefixFor(server) + tool;
}

Manager::~Manager() {
    shutdownAll();
}

ServerHandle* Manager::find(const std::string& name) {
    for (auto& up : servers_)
        if (up->cfg.name == name) return up.get();
    return nullptr;
}

const ServerHandle* Manager::find(const std::string& name) const {
    for (const auto& up : servers_)
        if (up->cfg.name == name) return up.get();
    return nullptr;
}

bool Manager::loadFromJson(const std::string& path) {
    configPath_ = path;
    configError_.clear();
    std::vector<McpServerConfig> loaded;
    if (!loadConfig(path, loaded, configError_)) {
        debugLogf("[mcp] config error: %s", configError_.c_str());
        return false;
    }
    servers_.clear();
    for (auto& cfg : loaded) {
        auto h = std::make_unique<ServerHandle>();
        h->cfg = std::move(cfg);
        servers_.push_back(std::move(h));
    }
    rememberStamp(path);
    return true;
}

ConnState Manager::connState(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h ? h->state.load() : ConnState::Stopped;
}

std::string Manager::lastError(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h ? h->lastError : std::string();
}

std::string Manager::protocolOf(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h ? h->cfg.protocol : std::string();
}

std::string Manager::descriptionOf(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h ? h->cfg.description : std::string();
}

bool Manager::trustedOf(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h && h->cfg.trusted;
}

bool Manager::enabledOf(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h && h->cfg.enabled;
}

size_t Manager::toolCount(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h ? h->tools.size() : 0;
}

unsigned Manager::spawnCount(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h ? h->spawnCount : 0;
}

std::vector<std::string> Manager::serverNames() const {
    std::vector<std::string> names;
    names.reserve(servers_.size());
    for (const auto& up : servers_)
        names.push_back(up->cfg.name);
    return names;
}

std::vector<std::string> Manager::toolNames(const std::string& name) const {
    const ServerHandle* h = find(name);
    return h ? h->toolNames : std::vector<std::string>();
}

std::vector<Manager::ServerPurpose> Manager::serverPurposes() const {
    std::vector<ServerPurpose> out;
    for (const auto& up : servers_) {
        const ServerHandle& h = *up;
        if (h.removed || !h.registered || h.registeredNames.empty()) continue;
        ServerPurpose p;
        p.name = h.cfg.name;
        p.description = h.cfg.description;
        const std::string prefix = prefixFor(h.cfg.name);
        for (const auto& full : h.registeredNames)
            p.tools.push_back(full.size() > prefix.size() ? full.substr(prefix.size()) : full);
        std::sort(p.tools.begin(), p.tools.end());
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(),
              [](const ServerPurpose& a, const ServerPurpose& b) { return a.name < b.name; });
    return out;
}

void Manager::rememberStamp(const std::string& path) {
    long long ticks = -1;
    unsigned long long size = 0;
    if (readFileStamp(path, ticks, size)) {
        configStamp_ = ticks;
        configSize_ = size;
    }
}

bool Manager::reloadIfChanged(const std::string& path) {
    long long ticks = -1;
    unsigned long long size = 0;
    if (!readFileStamp(path, ticks, size)) {
        // Missing or unreadable: keep the resident list. Every server in it still
        // works, and a save that temporarily removes the file must not kill them.
        return false;
    }
    if (path == configPath_ && ticks == configStamp_ && size == configSize_) return false;

    std::vector<McpServerConfig> loaded;
    std::string err;
    if (!loadConfig(path, loaded, err)) {
        // Half-saved file: never tear down a working set because of it. Record the
        // stamp so the same broken file is not parsed again on every message.
        configError_ = err;
        configStamp_ = ticks;
        configSize_ = size;
        debugLogf("[mcp] reload skipped, keeping %zu server(s): %s", servers_.size(), err.c_str());
        return false;
    }
    configPath_ = path;
    configError_.clear();
    configStamp_ = ticks;
    configSize_ = size;

    std::set<std::string> wanted;
    for (const auto& cfg : loaded)
        wanted.insert(cfg.name);

    for (auto& up : servers_) {
        ServerHandle& h = *up;
        if (wanted.count(h.cfg.name)) continue;
        if (h.removed) continue; // already queued for the reaper
        h.removed = true;
        stop(h.cfg.name);
        // The tools go at the next turn boundary: syncTools is the registry's only
        // writer. Erasing the handle here would leave mcp__<name>__* registered
        // forever, so the model would keep calling a capability that is gone.
        debugLogf("[mcp] '%s' removed from the config; tools drop at the next turn boundary",
                  h.cfg.name.c_str());
    }

    int added = 0, changed = 0, toggled = 0;
    for (const auto& cfg : loaded) {
        ServerHandle* h = find(cfg.name);
        if (!h) {
            auto up = std::make_unique<ServerHandle>();
            up->cfg = cfg;
            servers_.push_back(std::move(up));
            ++added;
            if (cfg.enabled && cfg.trusted) start(cfg.name);
            continue;
        }
        const bool wasEnabled = h->cfg.enabled;
        if (fingerprint(h->cfg) != fingerprint(cfg)) {
            // The process definition moved: restart it, but re-registering the
            // tools still happens at the turn boundary.
            stop(cfg.name);
            h->cfg = cfg;
            h->tools.clear();
            h->toolNames.clear();
            ++changed;
            if (cfg.enabled && cfg.trusted) start(cfg.name);
            continue;
        }
        // Same process: adopt the new text (description, enabled) without touching
        // it. A Dead server is NOT retried here -- F4's R is the retry entry point,
        // otherwise every message would retry a broken server.
        h->cfg.description = cfg.description;
        h->cfg.enabled = cfg.enabled;
        if (cfg.enabled != wasEnabled) {
            if (cfg.enabled)
                start(cfg.name);
            else
                stop(cfg.name);
            ++toggled;
        }
    }
    debugLogf("[mcp] reloaded '%s': %d added, %d changed, %d toggled", path.c_str(), added, changed,
              toggled);
    // True means "the new list was applied", not "a process moved": a
    // description-only edit changes no process but must still refresh the map.
    return true;
}

size_t Manager::reapRemoved() {
    size_t n = 0;
    for (auto it = servers_.begin(); it != servers_.end();) {
        if ((*it)->removed && !(*it)->registered) {
            debugLogf("[mcp] '%s' dropped from the resident pool", (*it)->cfg.name.c_str());
            it = servers_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

void Manager::joinWorker(ServerHandle& h) {
    if (h.worker.joinable()) h.worker.join();
}

void Manager::startEnabled() {
    for (const auto& up : servers_) {
        if (!up->cfg.enabled) continue;
        if (!up->cfg.trusted) {
            debugLogf("[mcp] skip '%s' (trusted=false)", up->cfg.name.c_str());
            continue;
        }
        start(up->cfg.name);
    }
}

void Manager::start(const std::string& name) {
    ServerHandle* h = find(name);
    if (!h) return;
    if (!h->cfg.trusted) {
        // fail closed: the only security switch this version has.
        debugLogf("[mcp] skip '%s' (trusted=false)", name.c_str());
        h->lastError = "not trusted (set \"trusted\": true in mcp.json to allow it)";
        h->state.store(ConnState::Stopped);
        return;
    }
    if (h->state.load() == ConnState::Ready) return; // already up

    // Never two workers on one handle: join the previous one first.
    joinWorker(*h);
    if (h->proc.valid) killProcess(*h); // leftovers from a failed previous run

    h->cancelRequested.store(false);
    h->lastError.clear();
    h->state.store(ConnState::Connecting);
    h->worker = std::thread(&Manager::workerMain, this, h);
}

void Manager::stop(const std::string& name) {
    ServerHandle* h = find(name);
    if (!h) return;
    h->cancelRequested.store(true);
    joinWorker(*h);
    killProcess(*h);
    h->cancelRequested.store(false);
    h->state.store(ConnState::Stopped);
    debugLogf("[mcp] server '%s' stopped", name.c_str());
}

void Manager::restart(const std::string& name) {
    stop(name);
    start(name);
}

void Manager::shutdownAll() {
    for (const auto& up : servers_) {
        const std::string name = up->cfg.name;
        up->cancelRequested.store(true);
        joinWorker(*up);
        killProcess(*up);
        up->state.store(ConnState::Stopped);
    }
}

void Manager::killProcess(ServerHandle& h) {
    // Documented close sequence: close stdin (EOF), wait up to 2s, then kill the
    // tree. MCP has no `shutdown` request -- closing stdin is the standard
    // "I will send nothing more" signal.
    closeStdin(h.proc);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    std::string out, errbuf;
    while (h.proc.valid && alive(h.proc)) {
        // Keep draining, otherwise a chatty server blocks on a full pipe while
        // we are waiting for it to exit.
        readAvailable(h.proc, out, errbuf);
        logStderr(h, errbuf);
        if (std::chrono::steady_clock::now() >= deadline) {
            logStderr(h, errbuf);
            debugLogf("[mcp:%s] did not exit after EOF; terminating the process tree",
                      h.cfg.name.c_str());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (h.proc.valid) terminate(h.proc, false);
    if (h.proc.valid) terminate(h.proc, true);
    closeAll(h.proc);
    h.proc.eof = false;
}

void Manager::setEnabled(const std::string& name, bool enabled) {
    ServerHandle* h = find(name);
    if (!h) return;
    if (!configPath_.empty()) {
        std::string err;
        if (!saveEnabled(configPath_, name, enabled, err))
            debugLogf("[mcp] could not persist enabled=%d for '%s': %s", (int)enabled, name.c_str(),
                      err.c_str());
        else
            rememberStamp(configPath_); // our own write is not an external change
    }
    h->cfg.enabled = enabled;
    if (enabled)
        start(name);
    else
        stop(name);
}

void Manager::logStderr(ServerHandle& h, std::string& errBuf) {
    size_t nl;
    while ((nl = errBuf.find('\n')) != std::string::npos) {
        std::string l = errBuf.substr(0, nl);
        errBuf.erase(0, nl + 1);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty()) continue;
        debugLogf("[mcp:%s] stderr: %s", h.cfg.name.c_str(), l.c_str());
    }
    // A server that writes a megabyte with no newline must not grow this forever.
    if (errBuf.size() > 8192) errBuf.clear();
}

void Manager::workerMain(ServerHandle* h) {
    std::string err;
    SpawnSpec spec;
    spec.command = h->cfg.command;
    spec.args = h->cfg.args;
    spec.env = h->cfg.env;
    spec.workDir = workDir_;

    if (!spawn(spec, h->proc, err)) {
        h->lastError = err;
        debugLogf("[mcp:%s] spawn failed: %s", h->cfg.name.c_str(), err.c_str());
        h->state.store(ConnState::Dead);
        return;
    }
    ++h->spawnCount;
    debugLogf("[mcp] server '%s' spawned (command=%s workDir=%s)", h->cfg.name.c_str(),
              h->cfg.command.c_str(), workDir_.c_str());

    auto poll = [h] { return h->cancelRequested.load(); };
    h->state.store(ConnState::Handshaking);
    if (!handshake(*h, err) || !listTools(*h, err)) {
        if (h->cancelRequested.load())
            h->lastError = "stopped during setup";
        else
            h->lastError = err;
        debugLogf("[mcp:%s] setup failed: %s", h->cfg.name.c_str(), h->lastError.c_str());
        killProcess(*h);
        h->state.store(ConnState::Dead);
        return;
    }
    h->lastError.clear();
    // Set Ready last: a reader that sees Ready must never race a worker that is
    // still writing the handle.
    h->state.store(ConnState::Ready);
    (void)poll;
    debugLogf("[mcp:%s] ready: %zu tool(s)", h->cfg.name.c_str(), h->tools.size());
}

bool Manager::handshake(ServerHandle& h, std::string& err) {
    if (h.cfg.protocol == "stateless") {
        // Modern mode has no handshake at all: every request carries its own
        // _meta block (see mcp_protocol::makeRequest).
        debugLogf("[mcp:%s] stateless mode: skipping initialize", h.cfg.name.c_str());
        return true;
    }
    nlohmann::json params = {{"protocolVersion", kOfferedProtocolVersion},
                             {"capabilities", nlohmann::json::object()},
                             {"clientInfo", {{"name", "proJV"}, {"version", PROJV_VERSION}}}};
    nlohmann::json result;
    auto poll = [&h] { return h.cancelRequested.load(); };
    if (!requestResponse(h, "initialize", params, poll, h.cfg.timeoutMs, result, err)) {
        if (h.cancelRequested.load()) err = "cancelled";
        return false;
    }
    const std::string negotiated =
        result.is_object() ? result.value("protocolVersion", std::string()) : std::string();
    if (!negotiated.empty() && !isKnownProtocolVersion(negotiated)) {
        // Spec: the client must disconnect when it cannot speak what the server
        // chose. Retrying would negotiate the same thing again.
        err = "server chose an unsupported protocol version: " + negotiated;
        return false;
    }
    debugLogf("[mcp:%s] negotiated protocolVersion=%s (offered %s)", h.cfg.name.c_str(),
              negotiated.empty() ? "(none)" : negotiated.c_str(), kOfferedProtocolVersion);

    std::string werr;
    if (!writeLine(h.proc, makeNotification("notifications/initialized", nullptr), werr)) {
        err = "could not send notifications/initialized: " + werr;
        return false;
    }
    return true;
}

bool Manager::listTools(ServerHandle& h, std::string& err) {
    nlohmann::json params = nlohmann::json::object();
    nlohmann::json result;
    auto poll = [&h] { return h.cancelRequested.load(); };
    if (!requestResponse(h, "tools/list", params, poll, h.cfg.timeoutMs, result, err)) {
        if (h.cancelRequested.load()) err = "cancelled";
        return false;
    }
    if (result.is_object() && result.contains("nextCursor") && result["nextCursor"].is_string()) {
        const std::string cursor = result["nextCursor"].get<std::string>();
        if (!cursor.empty()) {
            // Documented gap: registering only the first page would leave the
            // model guessing at tool names that exist but are not registered.
            err = "tools/list returned nextCursor (pagination is not supported in this version)";
            return false;
        }
    }
    h.tools.clear();
    h.toolNames.clear();
    if (!result.is_object() || !result.contains("tools") || !result["tools"].is_array()) {
        err = "tools/list result has no 'tools' array";
        return false;
    }
    for (const auto& t : result["tools"]) {
        if (!t.is_object()) continue;
        const std::string name = t.value("name", std::string());
        if (name.empty()) continue;

        ToolDefinition def;
        def.name = registeredName(h.cfg.name, name);
        def.description = t.value("description", std::string());
        if (t.contains("inputSchema") && !t["inputSchema"].is_null()) {
            // Validate now: a non-object schema would be sent as-is and the model
            // would invent argument names. Rejecting one tool is better than
            // silently degrading it (models.cpp has a catch that returns
            // {"type":"object"} and drops properties/required).
            if (!t["inputSchema"].is_object()) {
                debugLogf("[mcp:%s] tool '%s' skipped: inputSchema is not an object",
                          h.cfg.name.c_str(), name.c_str());
                continue;
            }
            try {
                def.inputSchemaJson = t["inputSchema"].dump();
            } catch (const std::exception& e) {
                debugLogf("[mcp:%s] tool '%s' skipped: inputSchema dump failed: %s",
                          h.cfg.name.c_str(), name.c_str(), e.what());
                continue;
            }
        } else {
            def.inputSchemaJson = "{\"type\":\"object\"}";
        }
        const std::string regName = def.name;
        h.tools.push_back(std::move(def));
        h.toolNames.push_back(regName);
    }
    return true;
}

bool Manager::requestResponse(ServerHandle& h, const std::string& method, nlohmann::json params,
                              const std::function<bool()>& cancelPoll, int timeoutMs,
                              nlohmann::json& result, std::string& err) {
    err.clear();
    result = nlohmann::json();

    const long long id = ++h.nextId;
    const bool stateless = h.cfg.protocol == "stateless";
    const std::string line = makeRequest(id, method, std::move(params), stateless, PROJV_VERSION);
    if (!writeLine(h.proc, line, err)) return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string buf, errbuf;
    std::string unhandled; // first server->client request we cannot answer
    bool cancelled = false;
    bool timedOut = false;

    auto consume = [&]() -> int {
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string l = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (isBlankLine(l)) continue;
            const Message m = parseLine(l);
            if (!m.parseOk) {
                // C3: skip, never abort. A server logging to stdout is common.
                debugLogf("[mcp:%s] skip non-protocol stdout line (%s)", h.cfg.name.c_str(),
                          m.why.c_str());
                continue;
            }
            if (m.isResponse && m.id == id) {
                if (!m.error.is_null()) {
                    err = describeError(m.error);
                    if (isVersionRejection(m.error)) err += " [protocol version rejected]";
                    return -1;
                }
                result = m.result;
                return 1;
            }
            if (m.isRequest && unhandled.empty()) {
                unhandled = m.method;
                continue;
            }
            // notifications/progress, tools/list_changed: read and dropped.
        }
        return 0;
    };

    for (;;) {
        if (cancelPoll && cancelPoll()) {
            cancelled = true;
            break;
        }
        readAvailable(h.proc, buf, errbuf);
        logStderr(h, errbuf);
        const int r = consume();
        if (r == 1) return true;
        if (r == -1) return false;

        if (buf.size() > kMaxLineBytes) {
            err = "server sent a line longer than 1 MiB (framing violation)";
            return false;
        }
        if (h.proc.eof || !alive(h.proc)) {
            err = "server closed stdout / exited";
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // C2: a slow server that already answered must not be a failure.
            readAvailable(h.proc, buf, errbuf);
            logStderr(h, errbuf);
            const int late = consume();
            if (late == 1) {
                debugLogf("[mcp:%s] response for %s arrived after the deadline; counted as "
                          "success",
                          h.cfg.name.c_str(), method.c_str());
                return true;
            }
            timedOut = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (cancelled) {
        std::string werr;
        nlohmann::json p = {{"requestId", id}, {"reason", "cancelled by user"}};
        writeLine(h.proc, makeNotification("notifications/cancelled", p), werr);
        err = "cancelled";
        return false;
    }
    err = timedOut ? ("timed out after " + std::to_string(timeoutMs) + "ms")
                   : std::string("server is gone");
    if (!unhandled.empty()) {
        // Name it: "did not answer" alone never tells an operator that the
        // server asked for a capability we declared we do not have.
        err += "; server sent a request we cannot answer: " + unhandled;
    }
    return false;
}

} // namespace mcp

namespace mcp {

Manager::SyncResult Manager::syncTools(ToolRegistry& registry) {
    // Agent thread only, at a turn boundary. This is the single writer of
    // ToolRegistry, and it runs on the same thread that reads it, so the registry
    // needs no lock (see the header). The caller applies `added`/`removed` to the
    // Agent's auto-tool set: that set is the second visibility channel and it
    // lives in the Agent, which is why this function reports instead of reaching.
    SyncResult result;
    for (auto& up : servers_) {
        ServerHandle& h = *up;
        const bool ready = h.state.load() == ConnState::Ready;

        if (!ready) {
            if (h.registered) {
                const size_t n = registry.unregisterByNamePrefix(prefixFor(h.cfg.name));
                result.removed.insert(result.removed.end(), h.registeredNames.begin(),
                                      h.registeredNames.end());
                h.registered = false;
                h.registeredNames.clear();
                debugLogf("[mcp:%s] unregistered %zu tool(s)", h.cfg.name.c_str(), n);
            }
            continue;
        }
        if (h.tools.empty()) continue;
        // Already in sync (same names in the same order) -> nothing to do.
        if (h.registered && h.registeredNames == h.toolNames) continue;

        if (h.registered) {
            registry.unregisterByNamePrefix(prefixFor(h.cfg.name));
            result.removed.insert(result.removed.end(), h.registeredNames.begin(),
                                  h.registeredNames.end());
            h.registered = false;
            h.registeredNames.clear();
        }

        const std::string prefix = prefixFor(h.cfg.name);
        std::vector<std::string> added;
        for (const auto& def : h.tools) {
            const std::string server = h.cfg.name;
            const std::string tool =
                def.name.size() > prefix.size() ? def.name.substr(prefix.size()) : def.name;
            // The executor closes over the Manager, never over the handle: a
            // restart may replace the process while the name stays the same.
            ToolExecutor ex = [this, &registry, server, tool](const std::string& args) {
                return callTool(server, tool, args, [&registry] { return registry.isCancelled(); });
            };
            if (registry.registerToolIfAbsent(def, ex)) {
                added.push_back(def.name);
            } else {
                debugLogf("[mcp:%s] tool '%s' already registered; skipped", server.c_str(),
                          def.name.c_str());
            }
        }
        result.added.insert(result.added.end(), added.begin(), added.end());
        h.registered = true;
        h.registeredNames = h.toolNames;
        debugLogf("[mcp:%s] registered %zu tool(s) (auto-visible)", h.cfg.name.c_str(),
                  added.size());
    }
    return result;
}

std::string Manager::callTool(const std::string& server, const std::string& tool,
                              const std::string& argsJson,
                              const std::function<bool()>& cancelPoll) {
    const std::string label = server + " / " + tool;
    ServerHandle* h = find(server);
    if (!h) return "Error: " + label + ": unknown MCP server '" + server + "'";
    const ConnState st = h->state.load();
    if (st != ConnState::Ready) {
        std::string msg = "Error: " + label + ": server is ";
        msg += connStateName(st);
        if (!h->lastError.empty()) msg += " (" + h->lastError + ")";
        return msg;
    }

    nlohmann::json args = nlohmann::json::object();
    if (!argsJson.empty()) {
        try {
            args = nlohmann::json::parse(argsJson);
        } catch (const std::exception&) {
            return "Error: " + label + ": arguments are not valid JSON";
        }
        if (!args.is_object()) return "Error: " + label + ": arguments must be a JSON object";
    }

    nlohmann::json params = {{"name", tool}, {"arguments", args}};
    nlohmann::json result;
    std::string err;
    if (!requestResponse(*h, "tools/call", params, cancelPoll, h->cfg.timeoutMs, result, err)) {
        return "Error: " + label + ": " + err;
    }

    std::string text, aggErr;
    if (!aggregateToolResult(result, text, aggErr)) {
        return "Error: " + label + ": " + aggErr;
    }
    const bool businessError = result.is_object() && result.contains("isError") &&
                               result["isError"].is_boolean() && result["isError"].get<bool>();

    if (text.empty()) text = "(no output)";
    if (businessError) {
        // Deliberately NOT "Error: ...": agent.cpp:827 reads that prefix as a
        // tool failure, and two of them in a row disable tools for the turn.
        return label + ": " + text;
    }
    return text;
}

} // namespace mcp
