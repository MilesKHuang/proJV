#include "python_tool_manager.h"
#include "registry.h"
#include "core/config.h"
#include "debug_log.h"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <array>

namespace fs = std::filesystem;

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
    const char* candidates[] = {"python", "python3", "py"};
    for (const auto* c : candidates) {
        std::string path = tryWhere(c);
        if (!path.empty()) {
            debugLogf("[PyTool] auto-detected python: %s", path.c_str());
            return path;
        }
    }
    debugLog("[PyTool] Python not found via 'where'");
    return "";
}

// ============================================================================
// Constructor
// ============================================================================
PythonToolManager::PythonToolManager(std::string pythonPath, std::string workspacePath)
    : pythonPath_(std::move(pythonPath))
    , workspacePath_(std::move(workspacePath))
{
    if (pythonPath_.empty()) {
        pythonPath_ = autoDetectPython();
    } else {
        debugLogf("[PyTool] using configured python: %s", pythonPath_.c_str());
    }
}

// ============================================================================
// scanAndRegister
// ============================================================================
void PythonToolManager::scanAndRegister(ToolRegistry& registry) {
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
        if (ec) { ec.clear(); continue; }
        if (!entry.is_directory(ec)) continue;
        if (ec) { ec.clear(); continue; }

        auto toolDir = entry.path();
        auto toolName = toolDir.filename().string();

        // (a) Check tool.json
        auto jsonPath = toolDir / "tool.json";
        if (!fs::is_regular_file(jsonPath)) {
            debugLogf("[PyTool] skipping '%s': no tool.json", toolName.c_str());
            skipped++; continue;
        }

        nlohmann::json j;
        try {
            std::ifstream ifs(jsonPath);
            ifs >> j;
        } catch (const std::exception& e) {
            debugLogf("[PyTool] skipping '%s': tool.json parse error: %s",
                toolName.c_str(), e.what());
            skipped++; continue;
        }

        if (!j.contains("name") || !j["name"].is_string() ||
            !j.contains("description") || !j["description"].is_string()) {
            debugLogf("[PyTool] skipping '%s': tool.json missing name/description",
                toolName.c_str());
            skipped++; continue;
        }

        std::string jsonName = j["name"].get<std::string>();
        if (jsonName != toolName) {
            debugLogf("[PyTool] warning: json name '%s' != dir '%s', using dir name",
                jsonName.c_str(), toolName.c_str());
        }

        // (b) Check main.py
        if (!fs::is_regular_file(toolDir / "main.py")) {
            debugLogf("[PyTool] skipping '%s': no main.py", toolName.c_str());
            skipped++; continue;
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
                if (!tp.name.empty())
                    def.parameters.push_back(std::move(tp));
            }
        }

        // Register
        std::string tdir = toolDir.string();
        std::string py = pythonPath_;
        std::string ws = workspacePath_;

        registry.registerTool(def,
            [tdir, py, ws](const std::string& args) -> std::string {
                return PythonToolManager::executePyTool(tdir, py, ws, args);
            });

        registered++;
        debugLogf("[PyTool] Registered '%s' (%zu params)",
            toolName.c_str(), def.parameters.size());
    }

    debugLogf("[PyTool] scan done: %d registered, %d skipped", registered, skipped);
}

// ============================================================================
// executePyTool (static, synchronous subprocess)
// ============================================================================
std::string PythonToolManager::executePyTool(
    const std::string& toolDir,
    const std::string& pythonPath,
    const std::string& workspacePath,
    const std::string& args)
{
    if (pythonPath.empty()) return "[PyTool Error] Python not configured";

#ifdef _WIN32
    std::string mainPy = toolDir + "\\main.py";
    std::string cmdLine = "\"" + pythonPath + "\" \"" + mainPy + "\"";

    // Create pipes
    HANDLE hStdinRd = nullptr, hStdinWr = nullptr;
    HANDLE hStdoutRd = nullptr, hStdoutWr = nullptr;
    HANDLE hStderrRd = nullptr, hStderrWr = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    if (!CreatePipe(&hStdinRd, &hStdinWr, &sa, 0))
        return "[PyTool Error] Failed to create stdin pipe";
    SetHandleInformation(hStdinWr, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hStdoutRd, &hStdoutWr, &sa, 0)) {
        CloseHandle(hStdinRd); CloseHandle(hStdinWr);
        return "[PyTool Error] Failed to create stdout pipe";
    }
    SetHandleInformation(hStdoutRd, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hStderrRd, &hStderrWr, &sa, 0)) {
        CloseHandle(hStdinRd); CloseHandle(hStdinWr);
        CloseHandle(hStdoutRd); CloseHandle(hStdoutWr);
        return "[PyTool Error] Failed to create stderr pipe";
    }
    SetHandleInformation(hStderrRd, HANDLE_FLAG_INHERIT, 0);

    // Build environment: inherit parent + add PROJV_* vars
    // Merge into parent's env block so PATH, SYSTEMROOT etc. are preserved
    std::wstring wenv;
    {
        LPWCH parentEnv = GetEnvironmentStringsW();
        if (parentEnv) {
            // Copy parent environment block
            const wchar_t* p = parentEnv;
            while (*p) {
                std::wstring entry(p);
                wenv += entry + L'\0';
                p += entry.size() + 1;
            }
            FreeEnvironmentStringsW(parentEnv);
        }
    }
    // Append our custom vars
    std::wstring wsVar = L"PROJV_WORKSPACE=" +
        std::wstring(workspacePath.begin(), workspacePath.end());
    std::wstring tdVar = L"PROJV_TOOL_DIR=" +
        std::wstring(toolDir.begin(), toolDir.end());
    wenv += wsVar + L'\0';
    wenv += tdVar + L'\0';
    wenv += L'\0';  // double-null terminator

    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdinRd;
    si.hStdOutput = hStdoutWr;
    si.hStdError  = hStderrWr;

    PROCESS_INFORMATION pi = {};

    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(),
        (int)cmdLine.size(), nullptr, 0);
    std::wstring wcmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(),
        (int)cmdLine.size(), &wcmd[0], wlen);
    std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        nullptr, cmdBuf.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        wenv.empty() ? nullptr : wenv.data(), nullptr, &si, &pi);

    CloseHandle(hStdinRd);
    CloseHandle(hStdoutWr);
    CloseHandle(hStderrWr);

    if (!ok) {
        DWORD err = GetLastError();
        CloseHandle(hStdinWr); CloseHandle(hStdoutRd); CloseHandle(hStderrRd);
        return "[PyTool Error] Failed to start: " + pythonPath +
               " (error " + std::to_string(err) + ")";
    }

    // Write args to stdin
    {
        DWORD written = 0;
        WriteFile(hStdinWr, args.data(), (DWORD)args.size(), &written, nullptr);
    }
    CloseHandle(hStdinWr);

    // Read stdout
    std::string out, errOut;
    char buf[4096];
    DWORD bytesRead;
    while (ReadFile(hStdoutRd, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        out += buf;
    }
    CloseHandle(hStdoutRd);

    // Read stderr
    while (ReadFile(hStderrRd, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        errOut += buf;
    }
    CloseHandle(hStderrRd);

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    while (!errOut.empty() && (errOut.back() == '\n' || errOut.back() == '\r'))
        errOut.pop_back();

    if (exitCode != 0) {
        return errOut.empty()
            ? "[PyTool Error] exit code: " + std::to_string(exitCode)
            : "[PyTool Error] " + errOut;
    }

    if (out.size() > 32000) {
        out = out.substr(0, 32000) + "\n... (truncated)";
    }

    return out;
#else
    (void)toolDir; (void)pythonPath; (void)workspacePath; (void)args;
    return "[PyTool Error] Not implemented on Linux (S4)";
#endif
}
