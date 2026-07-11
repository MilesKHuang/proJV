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
#include <format>
#include <string>
#include <sstream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// -- R4 超时常量 -----------------------------------------------------------
static constexpr int kPollIntervalMs = 3000;
static constexpr int kMaxIdlePolls = 10;

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
                                const std::string& stdinContent = "") {
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
        return std::format("Error: CreateProcess failed ({})", GetLastError());
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

    // -- 等待子进程：每 3s 检查一次，连续 kMaxIdlePolls 次无进展则终止 --
    bool forcedKill = false;
    int idlePolls = 0;
    size_t lastOutputSize = 0;

    while (idlePolls < kMaxIdlePolls) {
        DWORD waitResult = WaitForSingleObject(pi.hProcess, kPollIntervalMs);
        if (waitResult == WAIT_OBJECT_0) {
            // 进程自然退出
            break;
        }
        // 超时：检查 reader 是否有进展
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            if (output.size() > lastOutputSize) {
                // 有新输出 → 进程仍在工作，清零空闲计数
                lastOutputSize = output.size();
                idlePolls = 0;
                continue;
            }
        }
        // reader 已完成但进程未退出 → 管道已关，等待进程退出即可
        if (readerDone.load(std::memory_order_acquire)) {
            // 再给进程一次短等待
            DWORD w2 = WaitForSingleObject(pi.hProcess, kPollIntervalMs);
            if (w2 == WAIT_OBJECT_0) break;
            // 仍未退出 → 强制终止
            idlePolls = kMaxIdlePolls;
            break;
        }
        idlePolls++;
    }

    if (idlePolls >= kMaxIdlePolls) {
        // 超时：TerminateJobObject 终止整棵进程树
        if (hJob) {
            TerminateJobObject(hJob, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        forcedKill = true;
    }

    // 等待 reader 线程结束
    reader.join();
    CloseHandle(hStdoutRd);
    CloseHandle(pi.hThread);

    if (forcedKill) {
        std::lock_guard<std::mutex> lock(outputMutex);
        output += "\n[Command timed out after "
               + std::to_string(kMaxIdlePolls * kPollIntervalMs / 1000)
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
    registry.registerTool(def, [workspacePath](const std::string& args) -> std::string {
        std::string cmd = extractStringArg(args, "command");
        std::string stdinContent = extractStringArg(args, "stdin");
        if (cmd.empty()) return "Error: Missing 'command' argument";
        return execCommand(cmd, workspacePath, stdinContent);
    });
}
