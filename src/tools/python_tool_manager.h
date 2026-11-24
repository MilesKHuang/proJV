#pragma once
#include <string>

class ToolRegistry;
class IProcessRunner;

/// Manages discovery, registration, and execution of Python-based tools.
/// Tools live in {exeDir}/projv_pytool/ -- same convention as projv_prompts/.
class PythonToolManager {
  public:
    PythonToolManager(std::string pythonPath, std::string workspacePath);

    /// Scan {exeDir}/projv_pytool/ for valid tool directories and register them.
    void scanAndRegister(ToolRegistry& registry, IProcessRunner* procRunner);

    /// Synchronously execute a Python tool via IProcessRunner.
    /// Returns stdout (or error string).
    /// args is the JSON string from the LLM, passed to the tool via stdin.
    static std::string executePyTool(const std::string& toolDir, const std::string& pythonPath,
                                     const std::string& workspacePath, const std::string& args,
                                     IProcessRunner* procRunner);

    /// Drop the process-wide auto-detection cache. Call this when the configured
    /// python path changes, so the next session probes again instead of reusing a
    /// result taken before the edit. Without an explicit path, detection costs one
    /// shell round trip per PROCESS (not per session) because of that cache.
    static void resetPythonCache();

  private:
    static std::string pytoolDir();
    static std::string autoDetectPython();

    std::string pythonPath_;
    std::string workspacePath_;
};
