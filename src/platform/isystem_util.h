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
