#pragma once
#include <string>

// format_file: run clang-format on a single file, or fall back to a built-in
// whitespace normalizer that needs no external program.
//
// Why a dedicated tool instead of exec_shell: the process runner is a single
// global instance whose Run() holds a lock for the whole process lifetime, so
// concurrent workers using exec_shell would serialize AND could hang the run.
// A narrow tool keeps the command fixed (clang-format -i on one path) and gives
// it a short timeout, which is safe to call from parallel workers.
//
// `procRunner` may be null: the clang mode then reports a clear error instead
// of failing silently. `clangFormatPath` comes from config.toml
// (clang_format_path); empty means "clang-format from PATH".
void registerFormatTool(class ToolRegistry& registry, const std::string& workspacePath,
                        class IProcessRunner* procRunner = nullptr,
                        const std::string& clangFormatPath = "");
