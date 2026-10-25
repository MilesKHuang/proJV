// proJV -- System utility abstraction
// Windows: GetModuleFileName / ShellExecute / SEH  /  Linux: /proc/self/exe / xdg-open / sigaction
#pragma once
#include <functional>
#include <string>

class ISystemUtil {
  public:
    virtual ~ISystemUtil() = default;

    // Absolute path of the directory containing the executable.
    virtual std::string GetExeDir() = 0;

    // Config directory: {exeDir}/projv_files/
    virtual std::string GetUserConfigDir() = 0;

    // Find a system font by name. Returns empty string if not found.
    // NOTE: the four methods below are implemented on both platforms but are
    // currently unused by the TUI (no caller in src/ or tests/). They are part
    // of the platform abstraction and stay deliberately -- deleting them means
    // also deleting their helpers (SEH handler, sigaction handler, crashExePath_)
    // and the next person would rewrite all of it. Callers: none as of v0.6.0.
    virtual std::string GetSystemFontPath(const std::string& fontName) = 0;

    // Install global crash handler (SEH on Win, sigaction on Linux).
    virtual void InstallCrashHandler() = 0;

    // Install a graceful-exit handler for window close / Ctrl+C. The callback
    // runs best-effort when the OS asks the process to terminate, so the app
    // can close the database before the process is reaped.
    virtual void InstallExitHandler(std::function<void()> onExit) = 0;

    // Platform path separator: "\\" or "/"
    virtual std::string GetPathSeparator() = 0;

    // Open URL in default browser.
    virtual void OpenUrl(const std::string& url) = 0;
};

// Factory: platform-specific implementation
ISystemUtil* CreateSystemUtil();

// Global singleton (created once at startup)
namespace SystemUtil {
ISystemUtil& Instance();
void Init(ISystemUtil* inst);
}
