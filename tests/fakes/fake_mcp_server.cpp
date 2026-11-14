// proJV test fixture -- a minimal MCP server.
//
// A separate executable on purpose: MCP is a process protocol, so the only
// honest test spawns a real child process and talks to it over real pipes.
//
//   fake_mcp_server --mode=<name> [--pidfile=<path>]
//
// Modes:
//   stateful  legacy handshake + tools/list + tools/call        (default)
//   stateless modern: no handshake, requires the _meta block
//   badmeta   rejects the protocol version (-32602)
//   inputreq  tools/call answers resultType=input_required
//   badline   prints a non-JSON line to stdout before every response
//   noexit    ignores stdin EOF and keeps running
//   hang      accepts requests but never answers
//   slowcall  tools/call answers after 3 seconds
//   softfail  tools/call answers isError=true with text
//   fork      spawns a long-lived grandchild, then behaves normally
//   ok        alias of stateful (explicit "nothing special here")
//
// Writes its own pid and (for fork) the grandchild pid to --pidfile=... so a test
// can check that the whole tree died.
#include "json.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::string gMode = "stateful";
std::string gPidFile;

void writePidFile(const std::string& extra) {
    if (gPidFile.empty()) return;
    FILE* f = fopen(gPidFile.c_str(), "wb");
    if (!f) return;
#ifdef _WIN32
    fprintf(f, "%lu", (unsigned long)GetCurrentProcessId());
#else
    fprintf(f, "%d", (int)getpid());
#endif
    if (!extra.empty()) fprintf(f, " %s", extra.c_str());
    fclose(f);
}

void out(const std::string& line) {
    fputs(line.c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void emitBadLineIfEnabled() {
    if (gMode == "badline") out("[fake] this line is a log, not JSON");
}

void respond(const nlohmann::json& id, nlohmann::json result) {
    emitBadLineIfEnabled();
    nlohmann::json r;
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    if (gMode == "stateless") result["resultType"] = "complete";
    r["result"] = std::move(result);
    out(r.dump());
}

void respondError(const nlohmann::json& id, int code, const std::string& message,
                  const std::string& name = std::string()) {
    emitBadLineIfEnabled();
    nlohmann::json err = {{"code", code}, {"message", message}};
    if (!name.empty()) err["data"] = {{"name", name}};
    nlohmann::json r;
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["error"] = std::move(err);
    out(r.dump());
}

nlohmann::json makeTool(const std::string& name, const std::string& desc) {
    return {{"name", name},
            {"description", desc},
            {"inputSchema",
             {{"type", "object"},
              {"properties", {{"msg", {{"type", "string"}, {"description", "text to echo"}}}}},
              {"required", nlohmann::json::array({"msg"})}}}};
}

nlohmann::json toolList() {
    nlohmann::json tools = nlohmann::json::array();
    tools.push_back(makeTool("echo", "Echo the text back"));
    nlohmann::json nested = makeTool("nested", "Tool with a nested schema");
    nested["inputSchema"] = {
        {"type", "object"},
        {"properties",
         {{"cfg", {{"type", "object"}, {"properties", {{"deep", {{"type", "string"}}}}}}}}}};
    tools.push_back(nested);
    return {{"tools", tools}};
}

bool hasMeta(const nlohmann::json& params) {
    if (!params.is_object() || !params.contains("_meta")) return false;
    const auto& m = params["_meta"];
    return m.is_object() && m.contains("io.modelcontextprotocol/protocolVersion") &&
           m.contains("io.modelcontextprotocol/clientCapabilities") &&
           m.contains("io.modelcontextprotocol/clientInfo");
}

} // namespace

namespace {

void handle(const nlohmann::json& msg) {
    if (!msg.is_object()) return;
    const std::string method = msg.value("method", std::string());
    const bool hasId = msg.contains("id");
    nlohmann::json id = hasId ? msg["id"] : nlohmann::json();

    if (method.empty()) return; // a response: this server never sends requests

    if (gMode == "hang") return;
    if (gMode == "badmeta") {
        if (hasId)
            respondError(id, -32602, "Unsupported protocol version", "UnsupportedProtocolVersion");
        return;
    }

    if (method == "initialize") {
        if (!hasId) return;
        std::string version = "2025-11-25";
        if (msg.contains("params") && msg["params"].is_object())
            version = msg["params"].value("protocolVersion", version);
        respond(id, {{"protocolVersion", version},
                     {"capabilities", {{"tools", nlohmann::json::object()}}},
                     {"serverInfo", {{"name", "fake"}, {"version", "1.0"}}}});
        return;
    }
    if (method == "notifications/initialized") return;
    if (method == "notifications/cancelled") return;

    if (method == "tools/list") {
        if (!hasId) return;
        if (gMode == "stateless" && !hasMeta(msg.value("params", nlohmann::json::object()))) {
            respondError(id, -32602, "missing _meta");
            return;
        }
        respond(id, toolList());
        return;
    }

    if (method == "tools/call") {
        if (!hasId) return;
        if (gMode == "stateless" && !hasMeta(msg.value("params", nlohmann::json::object()))) {
            respondError(id, -32602, "missing _meta");
            return;
        }
        if (gMode == "inputreq") {
            nlohmann::json res = {{"resultType", "input_required"},
                                  {"inputRequests", nlohmann::json::array()}};
            nlohmann::json r = {{"jsonrpc", "2.0"}, {"id", id}, {"result", res}};
            out(r.dump());
            return;
        }
        if (gMode == "slowcall") std::this_thread::sleep_for(std::chrono::seconds(3));

        nlohmann::json args = nlohmann::json::object();
        if (msg.contains("params") && msg["params"].is_object())
            args = msg["params"].value("arguments", nlohmann::json::object());
        const std::string text = args.value("msg", std::string("(none)"));

        if (gMode == "softfail") {
            nlohmann::json content = nlohmann::json::array();
            content.push_back({{"type", "text"}, {"text", "cannot do that"}});
            respond(id, {{"content", content}, {"isError", true}});
            return;
        }
        nlohmann::json content = nlohmann::json::array();
        content.push_back({{"type", "text"}, {"text", "echo: " + text}});
        if (gMode == "badline") content.push_back({{"type", "image"}, {"mimeType", "image/png"}});
        respond(id, {{"content", content}, {"isError", false}});
        return;
    }

    if (hasId) respondError(id, -32601, "Method not found: " + method);
}

// Long-lived grandchild, so the orphan test has something to look for.
void spawnGrandchild() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --mode=sleep";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)pi.dwProcessId);
        writePidFile(buf);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    pid_t p = fork();
    if (p == 0) {
        for (;;)
            sleep(60);
    }
    if (p > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)p);
        writePidFile(buf);
    }
#endif
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const std::string modePrefix = "--mode=";
        const std::string pidPrefix = "--pidfile=";
        if (arg.rfind(modePrefix, 0) == 0) gMode = arg.substr(modePrefix.size());
        if (arg.rfind(pidPrefix, 0) == 0) gPidFile = arg.substr(pidPrefix.size());
    }
    if (gMode == "sleep") {
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    if (gMode == "fork") spawnGrandchild();
    if (gMode != "fork") writePidFile(std::string());

    std::string line;
    int c;
    for (;;) {
        line.clear();
        // One newline-delimited frame. EOF ends the loop unless --mode=noexit,
        // which is exactly what that mode exists to test.
        while ((c = fgetc(stdin)) != EOF && c != '\n')
            line.push_back((char)c);
        if (c == EOF) {
            if (gMode == "noexit") {
                for (;;)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            return 0;
        }
        if (line.empty()) continue;
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(line);
        } catch (...) {
            continue;
        }
        handle(msg);
    }
}
