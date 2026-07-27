#include "shell_tool.h"
#include "registry.h"
#include "json_utils.h"
#include <cstdlib>

// -- Tool description strings (moved from prompts.h) --------------------
static constexpr const char* TOOL_EXEC_SHELL_DESC_BASE =
    "Execute a shell command and return its output. ";
static constexpr const char* TOOL_EXEC_SHELL_DESC_FALLBACK =
    "Use for running programs, scripts, and diagnostics.";
static constexpr const char* TOOL_PARAM_COMMAND = "The shell command to execute";
static constexpr const char* TOOL_PARAM_STDIN =
    "Optional: content to pipe to the command's stdin. "
    "Use with PowerShell: powershell -Command \"$input | Set-Content -Path 'file.cpp' -Encoding UTF8\"";
#include <array>
#ifdef _MSC_VER
#include <format>
#else
#include <cstdio>
#endif
#include <string>
#include <sstream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// -- R4 超时常量 -----------------------------------------------------------
static constexpr int kPollIntervalMs = 100;
static constexpr int kMaxIdleMs = 30000;

static bool isPathWithinWorkspace(const std::string& path, const std::string& workspace) {
    if (workspace.empty()) return true;  // no restriction
    try {
        fs::path target = fs::absolute(path);
        fs::path base = fs::absolute(workspace);
        auto rel = fs::relative(target, base);
        return !rel.empty() && rel.native()[0] != '.';
    } catch (...) {
        return false;
    }
}

static std::string execCommand(const std::string& cmd, const std::string& workspacePath,
                                const std::string& stdinContent,
                                ToolRegistry* registry) {
#ifdef _WIN32
    // -- 构建最终命令：注入 Git 环境变量阻断交互式凭据提示 ----------------
    // GIT_TERMINAL_PROMPT=0   → 禁止 Git 弹出终端凭据提示
    // GCM_INTERACTIVE=Never   → 禁止 Git Credential Manager 交互
    // 清空 *_ASKPASS         → 禁止 askpass 助手
    std::string gitEnvBlock =
        "set GIT_TERMINAL_PROMPT=0 && "
        "set GCM_INTERACTIVE=Never && "
        "set GIT_ASKPASS= && "
        "set SSH_ASKPASS= && "
        "set CORE_ASKPASS= && ";

    std::string finalCmd;
    if (!workspacePath.empty()) {
        finalCmd = "cd /d \"" + workspacePath + "\" && " + gitEnvBlock + cmd;
    } else {
        finalCmd = gitEnvBlock + cmd;
    }

    // -- 创建 Job Object（KILL_ON_JOB_CLOSE 保证析构时终止整棵树）--------
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
    }

    // -- 创建管道 --------------------------------------------------------
    HANDLE hStdoutRd, hStdoutWr;
    HANDLE hStdinRd = nullptr, hStdinWr = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    if (!CreatePipe(&hStdoutRd, &hStdoutWr, &sa, 0)) {
        if (hJob) CloseHandle(hJob);
        return "Error: Failed to create stdout pipe";
    }
    // 子进程继承写端，本进程不继承读端
    SetHandleInformation(hStdoutRd, HANDLE_FLAG_INHERIT, 0);

    // stdin pipe（如有内容）
    bool hasStdin = !stdinContent.empty();
    if (hasStdin) {
        if (!CreatePipe(&hStdinRd, &hStdinWr, &sa, 0)) {
            CloseHandle(hStdoutRd);
            CloseHandle(hStdoutWr);
            if (hJob) CloseHandle(hJob);
            return "Error: Failed to create stdin pipe";
        }
        SetHandleInformation(hStdinWr, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdoutWr;
    si.hStdError  = hStdoutWr;
    si.hStdInput  = hasStdin ? hStdinRd : nullptr;

    PROCESS_INFORMATION pi = {};

    // 宽字符转换
    int wlen = MultiByteToWideChar(CP_UTF8, 0, finalCmd.c_str(),
                                    (int)finalCmd.size(), nullptr, 0);
    std::wstring wcmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, finalCmd.c_str(),
                         (int)finalCmd.size(), &wcmd[0], wlen);

    wcmd = L"cmd.exe /c " + wcmd;
    std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
    cmdBuf.push_back(L'\0');

    // -- CREATE_SUSPENDED → AssignProcessToJobObject → ResumeThread ----
    BOOL success = CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi
    );

    // 子进程已创建，关闭父进程侧的写端/读端句柄
    CloseHandle(hStdoutWr);
    if (hasStdin) { CloseHandle(hStdinRd); }

    if (!success) {
        CloseHandle(hStdoutRd);
        if (hasStdin) CloseHandle(hStdinWr);
        if (hJob) CloseHandle(hJob);
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "Error: CreateProcess failed (%lu)", GetLastError());
        return std::string(errBuf);
    }

    // 将主进程纳入 Job Object（子进程创建的子进程也会自动纳入）
    if (hJob) {
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    // 恢复主线程执行
    ResumeThread(pi.hThread);

    // 写入 stdin 内容
    if (hasStdin) {
        DWORD bytesWritten;
        WriteFile(hStdinWr, stdinContent.data(),
                   (DWORD)stdinContent.size(), &bytesWritten, nullptr);
        CloseHandle(hStdinWr);
    }

    // -- 独立 reader 线程：读 stdout，不阻塞主等待循环 ------------------
    std::string output;
    std::mutex outputMutex;
    std::atomic<bool> readerDone{false};

    std::thread reader([&]() {
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hStdoutRd, buf, sizeof(buf) - 1, &bytesRead, nullptr)
               && bytesRead > 0) {
            buf[bytesRead] = '\0';
            std::lock_guard<std::mutex> lock(outputMutex);
            output += buf;
        }
        readerDone.store(true, std::memory_order_release);
    });

    // -- Register process handle with registry for cancel support --
    if (registry) {
        std::lock_guard<std::mutex> lk(registry->activeProcessMutex_);
        registry->activeProcessHandle_ = pi.hProcess;
    }

    // -- 等待子进程：每 100ms 轮询，check cancel + 30s idle timeout --
    bool forcedKill = false;
    size_t lastOutputSize = 0;
    auto lastOutputTime = std::chrono::steady_clock::now();

    while (true) {
        DWORD waitResult = WaitForSingleObject(pi.hProcess, kPollIntervalMs);
        if (waitResult == WAIT_OBJECT_0) {
            // Process exited naturally
            break;
        }
        // Check cancel flag
        if (registry && registry->isCancelled()) {
            if (hJob) {
                TerminateJobObject(hJob, 1);
            } else {
                TerminateProcess(pi.hProcess, 1);
            }
            forcedKill = true;
            break;
        }
        // Check reader progress
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            if (output.size() > lastOutputSize) {
                lastOutputSize = output.size();
                lastOutputTime = std::chrono::steady_clock::now();
                continue;
            }
        }
        // Reader done but process still alive -- give a short grace period
        if (readerDone.load(std::memory_order_acquire)) {
            DWORD w2 = WaitForSingleObject(pi.hProcess, 2000);
            if (w2 == WAIT_OBJECT_0) break;
            if (hJob) {
                TerminateJobObject(hJob, 1);
            } else {
                TerminateProcess(pi.hProcess, 1);
            }
            forcedKill = true;
            break;
        }
        // Idle timeout check (30s with no new output)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastOutputTime).count() > kMaxIdleMs) {
            if (hJob) {
                TerminateJobObject(hJob, 1);
            } else {
                TerminateProcess(pi.hProcess, 1);
            }
            forcedKill = true;
            break;
        }
    }

    // -- Clean up active process handle --
    if (registry) {
        std::lock_guard<std::mutex> lk(registry->activeProcessMutex_);
        if (registry->activeProcessHandle_ == pi.hProcess) {
            registry->activeProcessHandle_ = nullptr;
        }
    }

    // For forced kill (cancel/timeout): close read handle first to force
    // ReadFile() to return immediately, preventing reader.join() deadlock.
    // For normal exit: let reader finish consuming pipe buffer first.
    if (forcedKill) {
        CloseHandle(hStdoutRd);
    }
    reader.join();
    if (!forcedKill) {
        CloseHandle(hStdoutRd);
    }
    CloseHandle(pi.hThread);

    if (forcedKill) {
        std::lock_guard<std::mutex> lock(outputMutex);
        output += "\n[Command timed out after "
               + std::to_string(kMaxIdleMs / 1000)
               + " seconds and was terminated]";
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    if (hJob) CloseHandle(hJob);

    if (exitCode != 0 && output.empty()) {
        output = "Exit code: " + std::to_string(exitCode);
    }

    // 修剪尾部换行
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();

    if (output.size() > 32000) {
        output = output.substr(0, 32000) + "\n... (truncated)";
    }

    return output;
#else
    std::string finalCmd = cmd;
    if (!workspacePath.empty()) {
        finalCmd = "cd \"" + workspacePath + "\" && " + cmd;
    }
    std::array<char, 4096> buf;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(finalCmd.c_str(), "r"), pclose);
    if (!pipe) return "Error: popen failed";
    while (fgets(buf.data(), buf.size(), pipe.get()) != nullptr)
        result += buf.data();
    return result;
#endif
}

void registerShellTool(ToolRegistry& registry, const std::string& workspacePath) {
    ToolDefinition def;
    def.name = "exec_shell";
    def.description = std::string(TOOL_EXEC_SHELL_DESC_BASE);
    if (!workspacePath.empty()) {
        def.description += "Commands run inside the workspace directory: " + workspacePath;
    } else {
        def.description += TOOL_EXEC_SHELL_DESC_FALLBACK;
    }
    def.parameters = {
        {"command", "string", TOOL_PARAM_COMMAND, true},
        {"stdin", "string", TOOL_PARAM_STDIN, false}
    };
    registry.registerTool(def, [&registry, workspacePath](const std::string& args) -> std::string {
        std::string cmd = extractStringArg(args, "command");
        std::string stdinContent = extractStringArg(args, "stdin");
        if (cmd.empty()) return "Error: Missing 'command' argument";
        return execCommand(cmd, workspacePath, stdinContent, &registry);
    });
}
