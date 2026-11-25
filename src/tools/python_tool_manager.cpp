#include "python_tool_manager.h"
#include "registry.h"
#include "shell_output.h"
#include "core/config.h"
#include "debug_log.h"
#include "platform/iprocess_runner.h"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <array>
#include <mutex>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {
// The detection cache. `tryWhere` is the expensive part (see autoDetectPython).
// File scope rather than a function-local static so resetPythonCache() can clear
// the same object the detector reads.
std::mutex g_pythonMutex;
std::optional<std::string> g_pythonCached;
} // namespace

// ============================================================================
// Helper: get projv_pytool directory (exe-relative, same as projv_prompts)
// ============================================================================
std::string PythonToolManager::pytoolDir() {
    return getPytoolDir();
}

// ============================================================================
// Helper: try 'where <candidate>' to find Python
// ============================================================================
static std::string tryWhere(const std::string& name) {
    std::string cmd =
#ifdef _WIN32
        "where " + name + " 2>nul";
#else
        "which " + name + " 2>/dev/null";
#endif
    std::array<char, 512> buf;
    std::string result;
#ifdef _MSC_VER
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    while (fgets(buf.data(), (int)buf.size(), pipe) != nullptr) {
        result += buf.data();
        if (!result.empty() && result.back() == '\n') break;
    }
#ifdef _MSC_VER
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

std::string PythonToolManager::autoDetectPython() {
    // Process-wide, and computed at most once. `tryWhere` starts a shell (cmd.exe
    // on Windows) with NO timeout: on a PATH that reaches a network share, or with
    // an endpoint agent intercepting child processes, one probe costs seconds.
    // The studio called this constructor for EVERY new pane, so opening the second
    // tab paid that bill again -- that is the freeze this cache removes.
    std::lock_guard<std::mutex> lk(g_pythonMutex);
    if (g_pythonCached) return *g_pythonCached;

    const char* candidates[] = {"python", "python3", "py"};
    std::string found;
    for (const auto* c : candidates) {
        std::string path = tryWhere(c);
        if (!path.empty()) {
            found = path;
            debugLogf("[PyTool] auto-detected python: %s", found.c_str());
            break;
        }
    }
    if (found.empty()) debugLog("[PyTool] Python not found via 'where'");

    g_pythonCached = found; // an empty result is cached too: "not found" is worth one probe
    return *g_pythonCached;
}

void PythonToolManager::resetPythonCache() {
    // Called when the configured python path changes: the next detection must not
    // reuse a probe taken before that edit.
    std::lock_guard<std::mutex> lk(g_pythonMutex);
    g_pythonCached.reset();
}

// ============================================================================
// Constructor
// ============================================================================
PythonToolManager::PythonToolManager(std::string pythonPath, std::string workspacePath)
    : pythonPath_(std::move(pythonPath)), workspacePath_(std::move(workspacePath)) {
    if (pythonPath_.empty()) {
        pythonPath_ = autoDetectPython();
    } else {
        debugLogf("[PyTool] using configured python: %s", pythonPath_.c_str());
    }
}

// ============================================================================
// scanAndRegister
// ============================================================================
void PythonToolManager::scanAndRegister(ToolRegistry& registry, IProcessRunner* procRunner) {
    std::string dir = pytoolDir();
    debugLogf("[PyTool] scanning: %s", dir.c_str());

    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
        debugLog("[PyTool] projv_pytool/ not found, skipping");
        return;
    }

    if (pythonPath_.empty()) {
        debugLog("[PyTool] Python not found, skipping scan");
        return;
    }

    int registered = 0;
    int skipped = 0;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!entry.is_directory(ec)) continue;
        if (ec) {
            ec.clear();
            continue;
        }

        auto toolDir = entry.path();
        auto toolName = toolDir.filename().string();

        // (a) Check tool.json
        auto jsonPath = toolDir / "tool.json";
        if (!fs::is_regular_file(jsonPath)) {
            debugLogWarnf("[PyTool] skipping '%s': no tool.json", toolName.c_str());
            skipped++;
            continue;
        }

        nlohmann::json j;
        try {
            std::ifstream ifs(jsonPath);
            ifs >> j;
        } catch (const std::exception& e) {
            debugLogWarnf("[PyTool] skipping '%s': tool.json parse error: %s", toolName.c_str(),
                          e.what());
            skipped++;
            continue;
        }

        if (!j.contains("name") || !j["name"].is_string() || !j.contains("description") ||
            !j["description"].is_string()) {
            debugLogWarnf("[PyTool] skipping '%s': tool.json missing name/description",
                          toolName.c_str());
            skipped++;
            continue;
        }

        std::string jsonName = j["name"].get<std::string>();
        if (jsonName != toolName) {
            debugLogWarnf("[PyTool] warning: json name '%s' != dir '%s', using dir name",
                          jsonName.c_str(), toolName.c_str());
        }

        // (b) Check main.py
        if (!fs::is_regular_file(toolDir / "main.py")) {
            debugLogWarnf("[PyTool] skipping '%s': no main.py", toolName.c_str());
            skipped++;
            continue;
        }

        // Build ToolDefinition
        ToolDefinition def;
        def.name = toolName;
        def.description = j["description"].get<std::string>();

        if (j.contains("parameters") && j["parameters"].is_array()) {
            for (const auto& p : j["parameters"]) {
                ToolParameter tp;
                tp.name = p.value("name", "");
                tp.type = p.value("type", "string");
                tp.description = p.value("description", "");
                tp.required = p.value("required", false);
                if (!tp.name.empty()) def.parameters.push_back(std::move(tp));
            }
        }

        // Register
        std::string tdir = toolDir.string();
        std::string py = pythonPath_;
        std::string ws = workspacePath_;

        registry.registerTool(
            def, [tdir, py, ws, procRunner](const std::string& args) -> std::string {
                return PythonToolManager::executePyTool(tdir, py, ws, args, procRunner);
            });

        registered++;
        debugLogf("[PyTool] Registered '%s' (%zu params)", toolName.c_str(), def.parameters.size());
    }

    debugLogf("[PyTool] scan done: %d registered, %d skipped", registered, skipped);
}

// ============================================================================
// executePyTool (static, via IProcessRunner -- dual platform)
// ============================================================================
std::string PythonToolManager::executePyTool(const std::string& toolDir,
                                             const std::string& pythonPath,
                                             const std::string& workspacePath,
                                             const std::string& args, IProcessRunner* procRunner) {
    if (pythonPath.empty()) return "[PyTool Error] Python not configured";
    if (!procRunner) return "[PyTool Error] Process runner not available";

    std::string mainPy = (fs::path(toolDir) / "main.py").string();

    ProcessConfig cfg;
    cfg.command = "\"" + pythonPath + "\" \"" + mainPy + "\"";
    cfg.workDir = workspacePath;
    cfg.stdinContent = args;
    cfg.timeoutMs = kProcessTimeoutMs;
    cfg.extraEnv["PROJV_WORKSPACE"] = workspacePath;
    cfg.extraEnv["PROJV_TOOL_DIR"] = toolDir;

    ProcessResult result = procRunner->Run(cfg);

    std::string out = result.stdout_;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();

    if (result.cancelled) return "[PyTool Error] cancelled";
    if (result.timedOut) return "[PyTool Error] timed out";

    if (result.exitCode != 0) {
        // stderr_ is only populated on POSIX; on Windows stderr shares the stdout
        // pipe, so fall back to it (this used to read an always-empty field).
        std::string errOut = result.stderr_.empty() ? out : result.stderr_;
        while (!errOut.empty() && (errOut.back() == '\n' || errOut.back() == '\r'))
            errOut.pop_back();
        return errOut.empty() ? "[PyTool Error] exit code: " + std::to_string(result.exitCode)
                              : "[PyTool Error] " + errOut;
    }

    // Same rule as exec_shell: keep the TAIL. This cut used to keep the head, so a
    // pytool that printed a long report lost its conclusion -- exactly the bug M2-A
    // fixed for exec_shell, missed here because pytool has its own return path.
    return shell_output::apply(out, shell_output::kDefaultTailLines, false).text;
}
